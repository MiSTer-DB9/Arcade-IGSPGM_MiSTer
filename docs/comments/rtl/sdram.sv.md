# rtl/sdram.sv — commentary

### `STATE_IDLE` channel priority

ch3 (68k program fetch, shared with the rom loader) is checked first: the
CPU is stalled dead on every rom_cache miss, while the audio (ch2), tile
(ch1) and sprite B/A (ch4/ch5) streams all have prefetch slack or caches.
Before this ordering ch3 was LAST, and sprite fetch traffic starved the CPU
ROM path — measured with the sdr_stress testrom as −10..−12% throughput on
miss-heavy loops at 224 sprites vs a flat −0.5% on real hardware (where
sprite ROMs share no bus with the CPU).  That starvation was the mechanism
behind espgalbl running ~3 s per 16 min behind real hardware.  The
remaining sprite-load sensitivity (~0.6-1.3%) is the non-preemptible
in-flight burst a miss must wait out.  The sim mirrors this priority in
sim/sim_top.sv's arbiter model; keep them in sync.
