"""Host runner for the sdr_stress testrom page (testroms/pages/sdr_stress.c).

Measures 68k loop throughput under ROM-cache stress while the sprite engine
generates configurable A/B-ROM SDRAM load.  Transports and framing mirror
util/vram_bench/remote.py; wire protocol in testroms/sdr_stress_protocol.h.

Usage:
  python3 util/sdr_stress/run.py --sim [-o out.jsonl]
  python3 util/sdr_stress/run.py --hw  [--repeats 3] [-o out.jsonl]
  python3 util/sdr_stress/run.py --compare a.jsonl b.jsonl
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ics2115_remote import SimServerClient, SimDebugLinkDevice  # noqa: E402

REQ_MAGIC = b"SS"
RSP_MAGIC = b"ss"
VERSION = 1
HEADER_SIZE = 6

CMD_PING = 0x01
CMD_RUN_TEST = 0x10
CMD_GET_RESULT = 0x11

STATUS_NAMES = {
    0x00: "OK", 0x02: "BAD_VERSION", 0x03: "BAD_LENGTH",
    0x04: "BAD_CMD", 0x05: "BAD_PARAM", 0x06: "NO_RESULT",
}

PING_MAGIC = 0x5D51
RESULT_SIZE = 20

VARIANTS = ["nomem", "rom_hot", "rom_miss", "rom_code"]
SPRITE_COUNTS = [0, 32, 64, 128, 224]
SIM_CYCLES_PER_FRAME = 900_000


class StressError(RuntimeError):
    pass


@dataclass
class StressResult:
    variant: int
    flags: int
    frames: int
    sprite_count: int
    accesses_per_chunk: int
    chunks: int
    vbl_start: int
    vbl_end: int

    @classmethod
    def unpack(cls, payload: bytes) -> "StressResult":
        if len(payload) != RESULT_SIZE:
            raise StressError(f"bad result payload length {len(payload)}")
        (variant, flags, frames, sprite_count, apc,
         chunks, vbl_start, vbl_end) = struct.unpack(">BBHHHIII", payload)
        return cls(variant, flags, frames, sprite_count, apc, chunks,
                   vbl_start, vbl_end)

    def to_dict(self) -> dict:
        return {
            "variant": VARIANTS[self.variant] if self.variant < len(VARIANTS) else self.variant,
            "flags": self.flags,
            "frames": self.frames,
            "sprite_count": self.sprite_count,
            "accesses_per_chunk": self.accesses_per_chunk,
            "chunks": self.chunks,
            "chunks_per_frame": self.chunks / self.frames if self.frames else 0.0,
            "accesses_per_frame": self.chunks * self.accesses_per_chunk / self.frames
                                  if self.frames else 0.0,
        }


class StressRemote:
    def __init__(self, dev, *, is_sim: bool = False):
        self.dev = dev
        self.is_sim = is_sim
        self.seq = 0

    @classmethod
    def open_hw(cls, target: str = "pgm", comms_addr: int = 0x1F800, *,
                reset: str = "l") -> "StressRemote":
        import pypicorom

        p = pypicorom.open(target)
        p.end_comms()
        if reset:
            p.set_parameter("reset", reset)
            time.sleep(0.1)
            p.set_parameter("reset", "z")
        p.start_comms(comms_addr)
        return cls(p, is_sim=False)

    @classmethod
    def open_sim(cls, *, executable: str = "./sim", cwd: str = "sim",
                 game: str = "pgm_test", comms_addr: int = 0x1F800,
                 boot_attempts: int = 60) -> "StressRemote":
        sim = SimServerClient.start(executable, cwd=cwd)
        try:
            sim.call("sim.initialize", {"headless": True})
            sim.call("sim.load_game", {"name": game})
            dev = SimDebugLinkDevice(sim, comms_addr=comms_addr)
            sim.call("sim.reset", {"cycles": 100})
            remote = cls(dev, is_sim=True)
            last_exc: Optional[Exception] = None
            for _ in range(boot_attempts):
                sim.call("sim.run_frames", {"count": 10})
                try:
                    remote.ping()
                    return remote
                except Exception as exc:  # noqa: BLE001
                    last_exc = exc
            raise StressError(f"testrom never answered ping: {last_exc}")
        except Exception:
            sim.close()
            raise

    @property
    def sim(self):
        return self.dev.sim if self.is_sim else None

    def close(self) -> None:
        close = getattr(self.dev, "close", None)
        if close is not None:
            close()

    # ---- framing ----------------------------------------------------------

    def _request(self, cmd: int, payload: bytes = b"", *,
                 timeout_s: float = 3.0, sim_frames: int = 10) -> bytes:
        self.seq = (self.seq + 1) & 0xFF
        frame = REQ_MAGIC + bytes([VERSION, self.seq, cmd, len(payload)]) + payload

        if self.is_sim:
            self.dev.read_timeout_cycles = (sim_frames + 10) * SIM_CYCLES_PER_FRAME
        self.dev.write(frame)

        # Read responses, resyncing on the magic and discarding stale
        # responses (e.g. late replies to boot-time pings) until our seq.
        for _ in range(8):
            hdr = self._read_header()
            status, length = hdr[4], hdr[5]
            body = self._read_exact(length) if length else b""
            if hdr[3] != self.seq:
                continue
            if status != 0x00:
                raise StressError(
                    f"command failed: {STATUS_NAMES.get(status, hex(status))}")
            return body
        raise StressError(f"no response with seq {self.seq}")

    def _read_header(self) -> bytes:
        window = b""
        for _ in range(4096):
            window = (window + self._read_exact(1))[-2:]
            if window == RSP_MAGIC:
                return RSP_MAGIC + self._read_exact(4)
        raise StressError("no response magic found")

    def _read_exact(self, n: int) -> bytes:
        data = bytes(self.dev.read_exact(n))
        if len(data) != n:
            raise StressError(f"short read: wanted {n}, got {len(data)}")
        return data

    # ---- commands ---------------------------------------------------------

    def ping(self) -> int:
        body = self._request(CMD_PING, sim_frames=2)
        magic, ver, vbl = struct.unpack(">HBI", body)
        if magic != PING_MAGIC:
            raise StressError(f"bad ping magic {magic:#06x}")
        return vbl

    def run_test(self, variant: int, frames: int, sprite_count: int,
                 seed: int = 0) -> StressResult:
        payload = struct.pack(">BBHHH", variant, 0, frames, sprite_count, seed)
        body = self._request(CMD_RUN_TEST, payload,
                             timeout_s=frames / 60.0 + 5.0,
                             sim_frames=frames + 6)
        return StressResult.unpack(body)


def run_suite(remote: StressRemote, platform: str, frames: int, repeats: int,
              out_path: Optional[str]) -> list:
    rows = []
    total = len(VARIANTS) * len(SPRITE_COUNTS)
    n = 0
    for variant_idx, variant in enumerate(VARIANTS):
        for count in SPRITE_COUNTS:
            n += 1
            samples = []
            for _ in range(repeats):
                r = remote.run_test(variant_idx, frames, count, seed=0x1234)
                samples.append(r)
            best = max(samples, key=lambda r: r.chunks)
            med = sorted(samples, key=lambda r: r.chunks)[len(samples) // 2]
            row = med.to_dict()
            row["platform"] = platform
            row["samples"] = [s.chunks for s in samples]
            rows.append(row)
            print(f"[{platform}] {n:2d}/{total} {variant:9s} spr={count:3d} "
                  f"apf={row['accesses_per_frame']:12.1f} chunks={med.chunks}",
                  flush=True)
            _ = best
    if out_path:
        with open(out_path, "w") as f:
            f.write(json.dumps({"type": "header", "platform": platform,
                                "frames": frames, "repeats": repeats,
                                "ts": time.time()}) + "\n")
            for row in rows:
                f.write(json.dumps(row, sort_keys=True) + "\n")
        print(f"wrote {out_path}")
    return rows


def compare(a_path: str, b_path: str) -> int:
    def load(p):
        out = {}
        for line in open(p):
            r = json.loads(line)
            if r.get("type") == "header":
                continue
            out[(r["variant"], r["sprite_count"])] = r
        return out

    a, b = load(a_path), load(b_path)
    rows = []
    for k in sorted(set(a) & set(b), key=lambda k: (k[0], k[1])):
        ra, rb = a[k], b[k]
        va, vb = ra["accesses_per_frame"], rb["accesses_per_frame"]
        delta = 100.0 * (va - vb) / vb if vb else 0.0
        rows.append((k, va, vb, delta))
    print(f"{'variant':9s} {'spr':>4s} {'A apf':>12s} {'B apf':>12s} {'delta%':>8s}")
    worst = 0.0
    for (variant, count), va, vb, delta in rows:
        flag = " *" if abs(delta) > 0.5 else ""
        print(f"{variant:9s} {count:4d} {va:12.1f} {vb:12.1f} {delta:+8.2f}{flag}")
        worst = max(worst, abs(delta))
    print(f"worst |delta| = {worst:.2f}%")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--sim", action="store_true")
    g.add_argument("--hw", action="store_true")
    g.add_argument("--compare", nargs=2, metavar=("A.jsonl", "B.jsonl"))
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    if args.compare:
        return compare(args.compare[0], args.compare[1])

    if args.sim:
        remote = StressRemote.open_sim(
            cwd=str(REPO_ROOT / "sim"))
        platform = "sim"
    else:
        remote = StressRemote.open_hw()
        platform = "hw"
        if args.repeats == 1:
            args.repeats = 3

    try:
        last_exc = None
        for _ in range(10):
            try:
                remote.ping()
                last_exc = None
                break
            except StressError as exc:
                last_exc = exc
                time.sleep(1.0)
        if last_exc is not None:
            raise last_exc
        run_suite(remote, platform, args.frames, args.repeats, args.output)
    finally:
        remote.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
