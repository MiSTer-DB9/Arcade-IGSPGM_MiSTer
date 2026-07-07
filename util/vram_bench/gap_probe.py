#!/usr/bin/env python3
"""Arbitration grant-structure fingerprint via gap-swept VRAM accesses.

Runs the word-read gap-probe variants (0/1/2/4/8 nops between accesses) plus
alt_rw and rmw on bg_vram in the ACTIVE window, with and without bus_master,
and a work_ram control.  For each variant it reports the per-access
arbitration cost:

    arb = (T_contended - T_free) / 16      [CPU cycles per access]

where T_* are cycles per chunk derived from accesses/frame.  The shape of
arb-vs-gap discriminates arbitration models:
  * contiguous per-line lock L: arb rises linearly with chunk duration
    (arb = L * T_free / (16 * (LINE - L)))
  * per-grant cost g: arb is constant vs gap
  * fragmented lock: steps/kinks as the access+gap period beats against the
    fragment spacing
alt_rw vs word_r (same gap ~ 0) isolates the R<->W turnaround cost; rmw adds
the same-address single-instruction case.

Usage:
  python3 util/vram_bench/gap_probe.py --sim [-o sim_gap.jsonl]
  python3 util/vram_bench/gap_probe.py --hw  [--repeats 3] [-o hw_gap.jsonl]
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
    FLAG_BUS_MASTER,
    TARGETS,
    VARIANTS,
    WIN_RANGE,
    VramBenchRemote,
)

PROBES = ["word_r", "word_r_p1", "word_r_p2", "word_r_p4", "word_r_p8", "alt_rw", "rmw"]
LINE_CYC = 20_000_000 / (264 * 60)   # 68k cycles per line
ACTIVE = (16, 200)
NLINES = ACTIVE[1] - ACTIVE[0]


def median_run(remote, variant, target, flags, frames, repeats):
    vals = []
    for _ in range(repeats):
        res = remote.run_test(VARIANTS.index(variant), TARGETS.index(target),
                              WIN_RANGE, flags, frames, *ACTIVE)
        vals.append(res.accesses / res.frames)
    return statistics.median(vals), vals


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sim", action="store_true")
    mode.add_argument("--hw", action="store_true")
    p.add_argument("--frames", type=int, default=None)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--target", default="pgm")
    p.add_argument("-o", "--output", help="output JSONL path")
    args = p.parse_args()

    os.environ.setdefault("PGM_ROM_DIR", str(Path.home() / "Documents" / "PGM_Roms"))
    if args.sim:
        remote = VramBenchRemote.open_sim(cwd=str(REPO_ROOT / "sim"))
        platform, repeats = "sim", 1
        frames = args.frames or 10
    else:
        remote = VramBenchRemote.open_hw(target=args.target)
        platform, repeats = "hw", args.repeats
        frames = args.frames or 60

    out = open(args.output, "w") if args.output else None

    def emit(obj):
        if out:
            out.write(json.dumps(obj, sort_keys=True) + "\n")
            out.flush()

    try:
        remote.ping()
        emit({"type": "header", "platform": platform, "frames": frames,
              "repeats": repeats, "ts": time.time()})

        rows = []
        print(f"{'variant':10s} {'gap':>4s} {'free a/f':>10s} {'lock a/f':>10s} "
              f"{'T_free':>7s} {'T_lock':>7s} {'arb/acc':>8s}")
        for v in PROBES:
            gap = {"word_r": 0, "word_r_p1": 4, "word_r_p2": 8,
                   "word_r_p4": 16, "word_r_p8": 32, "alt_rw": 0, "rmw": 0}[v]
            free_apf, free_samples = median_run(remote, v, "bg_vram", FLAG_BUS_MASTER, frames, repeats)
            lock_apf, lock_samples = median_run(remote, v, "bg_vram", 0, frames, repeats)
            work_apf, work_samples = median_run(remote, v, "work_ram", 0, frames, repeats)
            # cycles per chunk: window cycles / chunks in window
            t_free = LINE_CYC * NLINES / (free_apf / 16) if free_apf else 0.0
            t_lock = LINE_CYC * NLINES / (lock_apf / 16) if lock_apf else 0.0
            t_work = LINE_CYC * NLINES / (work_apf / 16) if work_apf else 0.0
            arb = (t_lock - t_free) / 16
            row = {"type": "probe", "platform": platform, "variant": v, "gap_cycles": gap,
                   "free_apf": free_apf, "lock_apf": lock_apf, "work_apf": work_apf,
                   "t_free_cyc": t_free, "t_lock_cyc": t_lock, "t_work_cyc": t_work,
                   "arb_per_access_cyc": arb,
                   "free_samples": free_samples, "lock_samples": lock_samples,
                   "work_samples": work_samples}
            rows.append(row)
            emit(row)
            print(f"{v:10s} {gap:4d} {free_apf:10.1f} {lock_apf:10.1f} "
                  f"{t_free:7.1f} {t_lock:7.1f} {arb:8.2f}")

        # Model fits over the pure-gap sweep (word_r..word_r_p8)
        sweep = [r for r in rows if r["variant"].startswith("word_r")]
        print("\ncontiguous-lock prediction: arb = L*T_free/(16*(LINE-L))")
        for L in (390.0, 417.0, 430.0):
            pred = ["%.2f" % (L * r["t_free_cyc"] / (16 * (LINE_CYC - L))) for r in sweep]
            print(f"  L={L:5.0f}: predicted arb/acc = {pred}")
        print(f"  measured arb/acc          = {['%.2f' % r['arb_per_access_cyc'] for r in sweep]}")
    finally:
        if out:
            out.close()
        remote.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
