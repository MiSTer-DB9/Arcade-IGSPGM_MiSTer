"""Host-side client for the vram_bench testrom page (testroms/pages/vram_bench.c).

Wire protocol: see testroms/vram_bench_protocol.h.  Framing mirrors ics_remote:
6-byte header [magic2|ver|seq|cmd|len] + payload, big-endian, responses echo the
sequence number.  RUN_TEST is synchronous on the target — the page is deaf for
the duration of the test and replies with the result record when done, so the
host must size its read timeout to the requested frame count.

Transports:
  * hardware: PicoROM (pypicorom), same open sequence as ICS2115Remote.open —
    always pulse reset (stale debug-link seq numbers never recover).
  * simulator: sim/sim --server debug_link.* methods via SimDebugLinkDevice.
"""

from __future__ import annotations

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

REQ_MAGIC = b"VB"
RSP_MAGIC = b"vb"
VERSION = 1
HEADER_SIZE = 6

CMD_PING = 0x01
CMD_INFO = 0x02
CMD_RUN_TEST = 0x10
CMD_GET_RESULT = 0x11
CMD_CAL_SCANLINE = 0x20
CMD_PEEK16 = 0x30
CMD_POKE16 = 0x31

STATUS_OK = 0x00
STATUS_NAMES = {
    0x00: "OK",
    0x02: "BAD_VERSION",
    0x03: "BAD_LENGTH",
    0x04: "BAD_CMD",
    0x05: "BAD_PARAM",
    0x06: "NO_RESULT",
}

PING_MAGIC = 0xB37C
RESULT_SIZE = 28

VARIANTS = [
    "nomem", "word_r", "word_w", "long_r", "long_w",
    "byte_r_hi", "byte_r_lo", "byte_w_hi", "byte_w_lo",
    "movem_r", "movem_w", "rmw",
    # gap-probe variants (word read + N nops; alternating separate R/W insns)
    "word_r_p1", "word_r_p2", "word_r_p4", "word_r_p8", "alt_rw",
]

# The standard comparison matrix (run.py) uses only these; the gap-probe
# variants are exercised by gap_probe.py.
CORE_VARIANTS = VARIANTS[:12]
TARGETS = ["work_ram", "bg_vram", "fg_vram"]
WINDOWS = ["all", "range", "line"]

WIN_ALL = 0
WIN_RANGE = 1
WIN_LINE = 2

FLAG_BUS_MASTER = 0x01
FLAG_SPRITE_DMA = 0x02
FLAG_LAYERS_OFF = 0x04

# Simulator: 50 MHz core clock, TickOneCycle = 2 sim "cycles" per clk in
# sim.run_cycles terms is handled by the server; a frame is ~833k clk.  Budget
# generously — the read simply returns as soon as the response arrives.
SIM_CYCLES_PER_FRAME = 900_000


class VramBenchError(RuntimeError):
    pass


class VramBenchProtocolError(VramBenchError):
    pass


class VramBenchCommandError(VramBenchError):
    def __init__(self, status: int, payload: bytes = b""):
        name = STATUS_NAMES.get(status, f"0x{status:02x}")
        super().__init__(f"vram_bench command failed: {name}")
        self.status = status
        self.payload = payload


@dataclass
class BenchInfo:
    cpu_hz: int
    n_variants: int
    n_targets: int
    n_windows: int


@dataclass
class ScanlineCal:
    min: int
    max: int
    at_irq6: int
    after_wrap: int
    samples: int
    wraps: int


@dataclass
class BenchResult:
    variant: int
    target: int
    window: int
    flags: int
    frames: int
    win_start: int
    win_end: int
    accesses_per_chunk: int
    chunks: int
    poll_spins: int
    vbl_start: int
    vbl_end: int

    @classmethod
    def unpack(cls, payload: bytes) -> "BenchResult":
        if len(payload) != RESULT_SIZE:
            raise VramBenchProtocolError(f"bad result payload length {len(payload)}")
        (variant, target, window, flags, frames, win_start, win_end, apc,
         chunks, spins, vbl_start, vbl_end) = struct.unpack(">BBBBHHHHIIII", payload)
        return cls(variant, target, window, flags, frames, win_start, win_end,
                   apc, chunks, spins, vbl_start, vbl_end)

    @property
    def accesses(self) -> int:
        return self.chunks * self.accesses_per_chunk

    @property
    def vbl_delta(self) -> int:
        return (self.vbl_end - self.vbl_start) & 0xFFFFFFFF

    def to_dict(self) -> dict:
        return {
            "variant": VARIANTS[self.variant] if self.variant < len(VARIANTS) else self.variant,
            "target": TARGETS[self.target] if self.target < len(TARGETS) else self.target,
            "window": WINDOWS[self.window] if self.window < len(WINDOWS) else self.window,
            "flags": self.flags,
            "frames": self.frames,
            "win_start": self.win_start,
            "win_end": self.win_end,
            "accesses_per_chunk": self.accesses_per_chunk,
            "chunks": self.chunks,
            "poll_spins": self.poll_spins,
            "vbl_delta": self.vbl_delta,
            "accesses": self.accesses,
            "accesses_per_frame": self.accesses / self.frames if self.frames else 0.0,
        }


class VramBenchRemote:
    def __init__(self, dev, *, is_sim: bool = False):
        self.dev = dev
        self.is_sim = is_sim
        self.seq = 0

    # ---- transports -------------------------------------------------------

    @classmethod
    def open_hw(cls, target: str = "pgm", comms_addr: int = 0x1F800, *,
                reset: str = "l") -> "VramBenchRemote":
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
                 reset_cycles: int = 100, boot_attempts: int = 60,
                 boot_frames_per_attempt: int = 10) -> "VramBenchRemote":
        sim = SimServerClient.start(executable, cwd=cwd)
        try:
            sim.call("sim.initialize", {"headless": True})
            sim.call("sim.load_game", {"name": game})
            dev = SimDebugLinkDevice(sim, comms_addr=comms_addr)
            sim.call("sim.reset", {"cycles": reset_cycles})
            remote = cls(dev, is_sim=True)
            # Boot until the page answers (debug_link_bench.py pattern).
            last_exc: Optional[Exception] = None
            for _ in range(boot_attempts):
                sim.call("sim.run_frames", {"count": boot_frames_per_attempt})
                try:
                    remote.ping()
                    return remote
                except Exception as exc:  # noqa: BLE001 - retry until boot completes
                    last_exc = exc
            raise VramBenchError(f"testrom never answered ping: {last_exc}")
        except Exception:
            sim.close()
            raise

    @property
    def sim(self) -> Optional[SimServerClient]:
        return self.dev.sim if self.is_sim else None

    def close(self) -> None:
        close = getattr(self.dev, "close", None)
        if close is not None:
            close()

    def __enter__(self) -> "VramBenchRemote":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        del exc_type, exc, tb
        self.close()

    # ---- framing -----------------------------------------------------------

    def _read_exact(self, n: int) -> bytes:
        data = self.dev.read_exact(n)
        if data is None or len(data) != n:
            raise VramBenchProtocolError(
                f"short read: wanted {n}, got {0 if data is None else len(data)}")
        return bytes(data)

    def _request(self, cmd: int, payload: bytes = b"", *,
                 timeout_frames: int = 4) -> bytes:
        if len(payload) > 255:
            raise ValueError("payload too large")
        self.seq = (self.seq + 1) & 0xFF
        frame = REQ_MAGIC + bytes([VERSION, self.seq, cmd & 0xFF, len(payload)]) + payload

        if self.is_sim:
            # Response arrival requires simulated time; scale the read budget
            # to the expected test length.
            self.dev.read_timeout_cycles = (timeout_frames + 10) * SIM_CYCLES_PER_FRAME
        self.dev.write(frame)

        hdr = self._read_exact(HEADER_SIZE)
        if hdr[:2] != RSP_MAGIC:
            raise VramBenchProtocolError(f"bad response magic: {hdr[:2]!r}")
        version, seq, status, length = hdr[2], hdr[3], hdr[4], hdr[5]
        if version != VERSION:
            raise VramBenchProtocolError(f"bad response version: {version}")
        if seq != self.seq:
            raise VramBenchProtocolError(f"bad response seq: got {seq}, expected {self.seq}")
        rsp_payload = self._read_exact(length) if length else b""
        if status != STATUS_OK:
            raise VramBenchCommandError(status, rsp_payload)
        return rsp_payload

    # ---- commands ----------------------------------------------------------

    def ping(self) -> int:
        payload = self._request(CMD_PING)
        if len(payload) != 7:
            raise VramBenchProtocolError(f"bad ping payload length {len(payload)}")
        magic, ver, vbl = struct.unpack(">HBI", payload)
        if magic != PING_MAGIC or ver != VERSION:
            raise VramBenchProtocolError(f"bad ping magic/version: {magic:#x}/{ver}")
        return vbl

    def info(self) -> BenchInfo:
        payload = self._request(CMD_INFO)
        if len(payload) != 7:
            raise VramBenchProtocolError(f"bad info payload length {len(payload)}")
        cpu_hz, nv, nt, nw = struct.unpack(">IBBB", payload)
        return BenchInfo(cpu_hz, nv, nt, nw)

    def run_test(self, variant: int, target: int, window: int, flags: int,
                 frames: int, win_start: int = 0, win_end: int = 0) -> BenchResult:
        payload = struct.pack(">BBBBHHH", variant, target, window, flags,
                              frames, win_start, win_end)
        rsp = self._request(CMD_RUN_TEST, payload, timeout_frames=frames + 4)
        return BenchResult.unpack(rsp)

    def get_result(self) -> BenchResult:
        return BenchResult.unpack(self._request(CMD_GET_RESULT))

    def cal_scanline(self, frames: int = 4) -> ScanlineCal:
        rsp = self._request(CMD_CAL_SCANLINE, struct.pack(">H", frames),
                            timeout_frames=frames + 4)
        if len(rsp) != 16:
            raise VramBenchProtocolError(f"bad cal payload length {len(rsp)}")
        vmin, vmax, at_irq6, after_wrap, samples, wraps = struct.unpack(">HHHHII", rsp)
        return ScanlineCal(vmin, vmax, at_irq6, after_wrap, samples, wraps)

    def peek16(self, addr: int) -> int:
        rsp = self._request(CMD_PEEK16, struct.pack(">I", addr))
        return struct.unpack(">H", rsp)[0]

    def poke16(self, addr: int, value: int) -> None:
        self._request(CMD_POKE16, struct.pack(">IH", addr, value))
