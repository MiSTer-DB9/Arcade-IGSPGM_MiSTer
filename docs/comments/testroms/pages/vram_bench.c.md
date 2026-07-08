# testroms/pages/vram_bench.c — commentary

VRAM access-timing benchmark, host-driven over the debug link.  The IGS023
gates 68k VRAM access with DTACK while its FG/BG tile fetchers own the VRAM
bus (rtl/igs023.sv).  This page measures how many VRAM accesses of a given
shape complete in an IRQ6-framed window so the FPGA arbitration can be tuned
against real hardware.  Host driver: util/vram_bench/run.py and
gap_probe.py; protocol: testroms/vram_bench_protocol.h; hardware reference
data and LA findings: util/vram_bench/results/.

### Measurement discipline

From cpu_cache_timing.c: every variant runs a byte-identical outer loop with
a 16-word-access unrolled chunk; only the access instructions differ, and
only the base address register changes per target (so work-RAM and VRAM
streams are instruction-identical).  Chunk accesses use fixed offsets 0..30
(reload, don't increment) so all accesses stay inside one 32-byte window.
Scanline-windowed runs poll the SCANLINE register (0xb07000, immediate
DTACK — never touches the VRAM arbitration) BETWEEN chunks, never inside
them; window-edge overrun is bounded to one chunk and identical across
targets, so it cancels in sim-vs-hw comparisons.

IRQ4 stays disabled throughout.  IRQ6 stays enabled and frames the
measurement window.

### `vblank_handler`

Must call `igs023_ack_irq6()` — pages with a registered IRQ6 handler ack
themselves; a missing ack livelocks the system in an IRQ6 storm.
