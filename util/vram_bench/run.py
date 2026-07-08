#!/usr/bin/env python3
"""VRAM access-timing benchmark suite runner.

Runs the vram_bench testrom matrix against the Verilator sim or real hardware
(PicoROM debug link), writes one JSON object per test (JSONL), and compares two
result files to localize FPGA-vs-hardware arbitration differences.

Usage:
  python3 util/vram_bench/run.py --sim [--quick] [--frames N] -o sim.jsonl
  python3 util/vram_bench/run.py --hw  [--quick] [--frames N] [--repeats 3] -o hw.jsonl
  python3 util/vram_bench/run.py --compare sim.jsonl hw.jsonl [--tol 0.5]

Build the testrom first:
  make -j8 -C testroms TARGET=pgm_test PAGE=vram_bench
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.vram_bench.remote import (  # noqa: E402
    CORE_VARIANTS,
    FLAG_BUS_MASTER,
    TARGETS,
    VARIANTS,
    WIN_ALL,
    WIN_LINE,
    WIN_RANGE,
    VramBenchRemote,
)

WINDOW_NAMES = ["all", "vblank", "active", "hline"]

# Logical windows -> (window_id, start, end).  Bounds for vblank are derived
# from CAL_SCANLINE at startup; these are the defaults if derivation fails.
DEFAULT_ACTIVE = (16, 200)
DEFAULT_HLINE = 100


def derive_windows(cal) -> dict[str, tuple[int, int, int]]:
    """Map logical window name -> (window_id, win_start, win_end)."""
    # SCANLINE counts up from 0 (vblank exit) through ~max at wrap.  IRQ6 fires
    # at vblank entry (~224); the fetch-free vblank span runs from there to the
    # wrap.  Guard bands keep chunk overruns inside the region.
    vbl_lo = cal.at_irq6 + 4
    vbl_hi = max(cal.max - 2, vbl_lo + 4)
    return {
        "all": (WIN_ALL, 0, 0),
        "vblank": (WIN_RANGE, vbl_lo, vbl_hi),
        "active": (WIN_RANGE, *DEFAULT_ACTIVE),
        "hline": (WIN_LINE, DEFAULT_HLINE, 0),
    }


def enumerate_matrix(quick: bool) -> list[tuple[str, str, str, int]]:
    """Return list of (variant, target, window, flags)."""
    tests: list[tuple[str, str, str, int]] = []
    if quick:
        variants = ["word_r", "word_w", "movem_r", "movem_w"]
        targets = ["work_ram", "bg_vram"]
        windows = ["all", "vblank", "active"]
        for v in variants:
            for t in targets:
                for w in windows:
                    tests.append((v, t, w, 0))
        tests.append(("nomem", "work_ram", "all", 0))
        tests.append(("word_r", "bg_vram", "all", FLAG_BUS_MASTER))
        return tests

    mem_variants = [v for v in CORE_VARIANTS if v != "nomem"]
    for v in mem_variants:
        for t in TARGETS:
            for w in WINDOW_NAMES:
                for bm in (0, FLAG_BUS_MASTER):
                    tests.append((v, t, w, bm))
    for w in WINDOW_NAMES:
        tests.append(("nomem", "work_ram", w, 0))
    return tests


def run_suite(args) -> int:
    os.environ.setdefault("PGM_ROM_DIR", str(Path.home() / "Documents" / "PGM_Roms"))

    if args.sim:
        remote = VramBenchRemote.open_sim(cwd=str(REPO_ROOT / "sim"))
        platform = "sim"
        repeats = 1
    else:
        remote = VramBenchRemote.open_hw(target=args.target)
        platform = "hw"
        repeats = args.repeats

    tests = enumerate_matrix(args.quick)
    out_path = Path(args.output) if args.output else None
    out = out_path.open("w") if out_path else None

    def emit(obj: dict) -> None:
        line = json.dumps(obj, sort_keys=True)
        if out:
            out.write(line + "\n")
            out.flush()
        else:
            print(line)

    try:
        vbl = remote.ping()
        info = remote.info()
        cal = remote.cal_scanline(frames=4)
        windows = derive_windows(cal)
        header = {
            "type": "header",
            "platform": platform,
            "cpu_hz": info.cpu_hz,
            "frames": args.frames,
            "repeats": repeats,
            "vblank_at_connect": vbl,
            "cal": vars(cal),
            "windows": {k: list(v) for k, v in windows.items()},
            "ts": time.time(),
        }
        emit(header)
        print(f"[{platform}] cal: min={cal.min} max={cal.max} at_irq6={cal.at_irq6} "
              f"after_wrap={cal.after_wrap} wraps={cal.wraps}", file=sys.stderr)

        for i, (v, t, w, flags) in enumerate(tests):
            win_id, lo, hi = windows[w]
            samples = []
            for rep in range(repeats):
                res = remote.run_test(VARIANTS.index(v), TARGETS.index(t), win_id,
                                      flags, args.frames, lo, hi)
                d = res.to_dict()
                d.update({
                    "type": "test",
                    "logical_window": w,
                    "platform": platform,
                    "repeat": rep,
                })
                samples.append(d)
                emit(d)
            med = statistics.median(s["accesses_per_frame"] for s in samples)
            spread = (max(s["accesses_per_frame"] for s in samples)
                      - min(s["accesses_per_frame"] for s in samples))
            rel = (spread / med * 100.0) if med else 0.0
            note = "  UNSTABLE" if repeats > 1 and rel > 0.5 else ""
            print(f"[{platform}] {i + 1}/{len(tests)} {v:10s} {t:8s} {w:6s} "
                  f"flags={flags} acc/frame={med:12.1f}{note}", file=sys.stderr)
    finally:
        if out:
            out.close()
        remote.close()
    return 0


def load_jsonl(path: str) -> tuple[dict, dict[tuple, list[dict]]]:
    header: dict = {}
    tests: dict[tuple, list[dict]] = {}
    with open(path) as f:
        for line in f:
            obj = json.loads(line)
            if obj.get("type") == "header":
                header = obj
            elif obj.get("type") == "test":
                key = (obj["variant"], obj["target"], obj["logical_window"], obj["flags"])
                tests.setdefault(key, []).append(obj)
    return header, tests


def compare(args) -> int:
    ha, ta = load_jsonl(args.compare[0])
    hb, tb = load_jsonl(args.compare[1])
    pa = ha.get("platform", "a")
    pb = hb.get("platform", "b")

    rows = []
    for key in sorted(set(ta) & set(tb)):
        ma = statistics.median(s["accesses_per_frame"] for s in ta[key])
        mb = statistics.median(s["accesses_per_frame"] for s in tb[key])
        if mb:
            delta = (ma - mb) / mb * 100.0
        else:
            delta = float("inf") if ma else 0.0
        rows.append((key, ma, mb, delta))

    rows.sort(key=lambda r: abs(r[3]), reverse=True)

    print(f"{'variant':<10s} {'target':<8s} {'window':<6s} {'flg':>3s} "
          f"{pa + ' acc/frm':>14s} {pb + ' acc/frm':>14s} {'delta%':>8s}")
    worst = 0.0
    for (v, t, w, flags), ma, mb, delta in rows:
        mark = " *" if abs(delta) > args.tol else ""
        print(f"{v:<10s} {t:<8s} {w:<6s} {flags:>3d} {ma:>14.1f} {mb:>14.1f} "
              f"{delta:>8.2f}{mark}")
        worst = max(worst, abs(delta))

    only_a = set(ta) - set(tb)
    only_b = set(tb) - set(ta)
    if only_a:
        print(f"only in {args.compare[0]}: {len(only_a)} tests", file=sys.stderr)
    if only_b:
        print(f"only in {args.compare[1]}: {len(only_b)} tests", file=sys.stderr)

    print(f"\nworst |delta| = {worst:.2f}% (tolerance {args.tol}%)")
    return 0 if worst <= args.tol else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sim", action="store_true", help="run against the Verilator sim")
    mode.add_argument("--hw", action="store_true", help="run against hardware via PicoROM")
    mode.add_argument("--compare", nargs=2, metavar=("A.jsonl", "B.jsonl"),
                      help="compare two result files")
    p.add_argument("--quick", action="store_true", help="reduced test matrix")
    p.add_argument("--frames", type=int, default=None,
                   help="frames per test (default: 10 sim, 60 hw)")
    p.add_argument("--repeats", type=int, default=3, help="hw repeats per test")
    p.add_argument("--target", default="pgm", help="pypicorom target name")
    p.add_argument("--tol", type=float, default=0.5, help="compare tolerance in %%")
    p.add_argument("-o", "--output", help="output JSONL path")
    args = p.parse_args()

    if args.compare:
        return compare(args)
    if args.frames is None:
        args.frames = 10 if args.sim else 60
    return run_suite(args)


if __name__ == "__main__":
    sys.exit(main())
