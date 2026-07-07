# testroms/pages/sdr_stress.c — commentary

SDR ROM-cache stress test.  The 68k program ROM goes through a direct-
mapped cache (rtl/rom_cache.sv, 16 KB: 2048 × 8-byte lines, index =
word-addr[12:2]) in front of SDRAM, which it shares with the sprite A/B-ROM
fetchers.  Real hardware has zero-wait mask ROM, so every cache miss is
FPGA-only stall time — and sprite fetch traffic can lengthen it.  This page
measures IRQ6-framed loop throughput for cache-resident, cache-hit,
conflict-miss and sequential-fetch loops while a configurable number of
random sprites generate A/B-ROM load.  Host driver: util/sdr_stress/run.py;
protocol: testroms/sdr_stress_protocol.h; reference results:
util/sdr_stress/results/.

### `sdr_code_blob`

32 KB of sequential nops — bigger than the 16 KB rom_cache, so every pass
misses once per 8-byte line (4096 misses/pass).  This is the game-like
fetch pattern.

### `C_ROM_MISS`

Alternating reads 16 KB apart: same direct-mapped index, different tag —
every read is a conflict miss regardless of history.

### `build_sprites`

Static seeded-random list (deterministic across sim/hw); the chip re-renders
and re-fetches A/B-ROM art for every sprite every frame, so a static list
still generates constant per-frame fetch load.  Addresses are scattered
across 16 MB of A-ROM space — the art content is irrelevant, only the
bandwidth matters.  Regenerating per frame inside the IRQ handler would
pollute the measurement with CPU time.

### `vblank_handler`

Pages that register an IRQ6 handler MUST call `igs023_ack_irq6()` inside
it — main.c only auto-acks when no handler is registered.  A missing ack
is an IRQ6 storm: the CPU livelocks in level6_handler and the page never
runs (white screen, dead debug link).

### auto-cycle (`idle_frames`)

With no debug link attached (e.g. running as a ROM on a MiSTer core) the
page auto-cycles ROM_CODE through several sprite counts on a fixed
120-frame window and prints raw chunk totals on screen, so a human can
compare a MiSTer build against real hardware by eye.  The 3600-frame idle
threshold exists because an auto test makes the page deaf to the debug
link for its duration — a shorter threshold raced the sim host's boot-ping
loop and timed out the mailbox.
