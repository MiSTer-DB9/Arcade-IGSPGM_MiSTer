# Logic-analyzer findings — IGS023 CPU/VRAM arbitration (2026-07-04)

Capture setup: DSLogic U3Pro32, `dsl-capture --config util/vram_bench/dsl-capture.cfg`
(250 MHz, VCD; signals: CPU_AS#/RW/DTACK#/CLK, VRAM_WR#/OE#/CE#, VRAM A0-A3/A8-A11,
SYNC#).  Board runs `PAGE=vram_bench`; a long RUN_TEST (e.g. word_w x 1200 frames,
ACTIVE window) gives a steady repeating pattern; trigger on `VRAM_WR#=F`.
Analysis scripts: parse the VCD with pywellen (times in ps).

## Confirmed facts (word_w capture, la_word_w.vcd.gz)

- CPU_CLK = 20.0 MHz exactly; SYNC# line period = 64.000 us = 1280 CPU cycles.
- VRAM_CE#/OE# are static on this board - carry no information.
- A CPU word write reaches VRAM as TWO WR# pulses (high byte then low byte),
  ~40 ns apart - matches the FPGA's HIGH/LOW phase model.
- CPU_DTACK# (as probed) asserts ~8 ns after AS# for EVERY cycle, including
  stalled ones - DTACK at this probe point is NOT the wait mechanism visible.
- No CPU clock stretching (period rock-steady).
- THE mechanism: one hole per fetched line where the CPU's in-flight VRAM
  access is held (AS# low throughout), while the fetchers stream on the VRAM
  address bus.  The held write COMMITS AT THE END of the hole (WR# pulses),
  then the CPU resumes at full speed immediately (steady 800 ns/write cadence,
  no post-hole slotting, no grant ceiling).
- The hole is END-ANCHORED: corr(start, length) = -0.94.
    start: when the CPU first touches VRAM after fetch start (~57.3 us after
           SYNC# fall == FPGA hcnt~638 arm point - the FPGA trigger is correct)
    end:   11.0 +/- 1.5 us after the NEXT SYNC# fall (per-line variance,
           plausibly BG ROM fetch length / rowscroll dependent)
    => typical full fetch window ~ 17.8 us (not fixed).
- Loops that never touch the 023 are never held (work-RAM baseline unaffected).

## Implications for the FPGA model (rtl/igs023.sv)

- Contiguous-lock structure and arm point are RIGHT.
- Current CPU_FETCH_LOCK=1043 clk (20.9 us) is throughput-calibrated; direct
  hole length is ~17.8 us ending ~11 us into the next line.  The ~3 us
  difference vs throughput calibration is not yet explained (candidates:
  fetch-end variance distribution, loop phase-locking).  A structurally exact
  model would end the lock at an hcnt anchor (~hcnt 173 = 11 us past sync)
  with per-line variance TBD.
- The "grant-slot ceiling" and per-access arbitration inferred from aggregate
  data were artifacts of the end-anchored hole; do not model them.

## Open questions (next captures)

1. alt_rw/rmw R<->W penalty: benchmark shows mixed R/W patterns clamp harder
   than the hole alone explains; first alt_rw capture (la_alt_rw.vcd.gz) shows
   a stretched-write-cycle tail (AS low 200-250 ns vs normal 125 ns) but the
   write counts in that capture need re-analysis (suspect: capture window vs
   test phase).  Redo: longer capture, verify against RUN_TEST result, measure
   read->write and write->read cycle-length pairs in the free region.
2. Hole-end variance: correlate end phase against line index / BG scroll
   config (poke bg_x/rowscroll via POKE16 between runs).
3. Map hole end to hcnt precisely using SYNC# serrations for vsync alignment.

## Update: the alt_rw question — ANSWERED (second alt capture, la_alt2 verified in-test)

The first alt capture accidentally caught the idle text-render loop (worthless);
the verified capture shows:
- alt pairs stream at exactly 1600 ns (89.4%) between holes — NO per-pair or
  per-flip turnaround penalty exists on hardware.
- Hole anatomy is IDENTICAL to word_w (start ~57.9, end ~11.0, len 16.4 us);
  78% of alt holes catch a read, 22% a write — symmetric handling.
- KEY REINTERPRETATION: the steady 800 ns word cadence is NOT the free rate —
  bus_master/vblank run faster (63.9 acc/line would not fit a 64 us line at
  800 ns).  On fetched lines, outside the hole, hardware PACES CPU VRAM access
  STARTS to ~16 CPU cycles (800 ns).  Back-to-back continuations (long 2nd
  word, movem streams) escape the slot partially (hw movem contended ~540-650
  ns/word — faster than paced, slower than free).
- This pacing is what equalizes word=byte=rmw=alt contended throughput
  (each pays one slot per after-idle access start).  The earlier "decaying
  R/W turnaround" model was a wrong mechanism fitted to two points; removed.

Model now in rtl/igs023.sv:
- End-jittered hole: CPU_FETCH_LOCK_BASE=811 clk + LFSR 0..127 (mean 17.5 us).
- Pacing: CPU_FETCH_PACE=40 clk min start-to-start, PACE_BYPASS_WINDOW=15 clk
  (request within 15 clk of previous completion = back-to-back, escapes slot).
- Verified vs hw (ACTIVE window): word -1.5, word_w -1.7, alt -1.7, rmw -0.02,
  byte_r -1.7, byte_w_lo -3.7; vblank/bus_master exact (<=0.4%).

## Remaining opens

1. Burst semantics: full waiver lets movem/long run hole-limited (+10-15/+5.6%
   fast).  Hardware runs bursts at an intermediate rate.  Need: movem_r/long_r
   LA captures -> measure contended intra-burst word cadence, then model (e.g.
   burst words pay a shorter slot, ~13 cyc?).
2. Sparse-access pacing semantics: pads p2/p4/p8 now +8/+18/+9% fast — hw slots
   may be an absolute grid (wait for next slot boundary) rather than min-spacing.
   Need: word_r_p4 capture -> access-start phase histogram mod slot period.

## BG_CTRL sweep (2026-07-04, la_bg00/la_bg10/la_bg1f.vcd.gz)

Corrected architecture (from Martin): TWO consumers share VRAM inside the 023 -
FG (8x8 tiles, fetches during hblank into a line buffer, 4 byte-reads per tile)
and BG (32x32 tiles, fetched DURING the active line, 3-4 reads per 32 pixels).
BG_CTRL (0xb04000) bits [4:0] set BG x-scale hence fetch rate: 0x00 = highest,
0x10 = normal (4 reads/32px), 0x1f = lowest.  Board/testrom default = 0x0610,
i.e. the game-normal rate - all prior hw reference data is at 0x10.

Sweep results (word_w ACTIVE benchmark + captures):
- apf: 0x00=7780, 0x10=7856, 0x1f=7856 - CPU throughput ~invariant to BG rate.
- CPU write cadence: 800ns flat at 0x00/0x10; 700ns med / 980ns p90 at 0x1f.
- Hole: med 18.4-18.8us all settings; 0x00 has wider variance (p10 14.7us).

KEY REALIZATION: VRAM CE#/OE# are tied active, so the data bus always drives -
internal "reads" are just address applications + invisible sampling.  The
observable is the ADDRESS SEQUENCE.  Mid-line the address bus streams at
~100ns (pixel) cadence at ALL BG_CTRL settings, so the continuous stream is
NOT the BG fetch itself: the 023 is an address-bus MULTIPLEXER running a
mostly-fixed positional schedule.  A CPU access = a mux slot granted to the
CPU (~every 8 pixels during active lines, none during the FG hblank phase,
unrestricted in vblank).  This positional-mux picture explains: fixed CPU
pacing regardless of BG demand, the end-anchored hole (FG line-buffer build
owns the mux), and vblank freedom.

## Next session: decode the mux schedule

Decode A0-A3/A8-A11 VALUES (not just event times) within mid-line trains
across the three BG_CTRL captures:
- nibble progression -> how many interleaved address streams, slot period,
  which slot positions the CPU can take, what scales with BG_CTRL[4:0].
- same decode inside the hole -> FG fetch sequence structure (4-read groups,
  tile stride) and what determines hole-end variance.
- then replace hole+pace+jitter+bypass in rtl/igs023.sv with the actual slot
  schedule; hole/pacing/burst behavior should become emergent.
Analysis harness: pywellen on the VCDs; build (time, nibble) streams by
sampling all 8 address bits at each merged bus-change event.

## DECODED: the VRAM mux schedule (from la_bg00/10/1f value-level analysis)

Per active line:
1. FG line-buffer fetch from ~hcnt 638: linear 4-bytes-per-tile crawl at ~30ns
   per access (33.8688 MHz master clock), ~13.4us total (matches the FPGA's
   464-tick FG window).  One 2-event rowscroll-like blip mid-phase.  CPU locked.
2. BG head-start: pixel-rate BG stream alone ~4.5us (~1.4 columns prefetch),
   CPU STILL locked.  (Missing from the FPGA model - its BG+CPU share starts
   immediately after FG release.)
3. Steady state: 100ns pixel slots.  BG replays the current 32x32 tile entry
   in an up-down byte scan {0,1,2,1} (byte 3 never accessed); tile entry
   advances +4 bytes every (BG_CTRL[4:0]+16) pixels (0x00->16px, 0x10->32px,
   0x1f->47px - verified).  CPU granted exactly 1 slot in 8 (800ns), fixed
   phase (after BG byte-0 slot, alternate microcycles); the whole CPU access
   (both byte pulses, 48ns apart, EVEN byte then ODD - reverse of the FPGA's
   HIGH->LOW order) completes within the ~150ns grant.
4. Vblank lines: no schedule, CPU free.

Evidence: quiet-line sequences (c1c2c1c0 microcycles, column advance verified
at all three BG_CTRL values), CPU-interleave sequences (7 BG slots + write
pair per 800ns, grant phase fixed), hole decode (hi=0 FG crawl 449 events
@~30ns, hi=1 blip, hi=9 BG stream 4.5us, then the held CPU write commits).

Implications for rtl/igs023.sv: replace CPU_FETCH_LOCK/jitter/pace/bypass with
this schedule as a real FSM: lock = FG fetch + BG head-start (end variance
likely = scroll-alignment of the BG handover - test by poking bg_x); pacing
emerges from the 1-in-8 grant; burst behavior (movem/long) should emerge from
grant-slot occupancy - verify against the full benchmark matrix.  Also fix
CPU write byte order (even->odd).  Open: what sets BG head-start length
(scroll? fixed?); grant cadence at BG_CTRL=0x1f measured 700/980 mix - grant
phase interacts with the 47px column period; FG crawl lo-nibble pattern
suggests {+1,+2,+3,+0}-style group order - decode precisely when implementing.

## DECODED (2026-07-04b): CPU gets TWO slots per microcycle — {0, 3}

la_movem.vcd.gz (movem_w, BG_CTRL default): back-to-back word writes alternate
320/480ns intervals, summing to exactly 800ns per pair — the CPU is granted 2
slots per 8-slot microcycle, 3 slots apart.  Single-access loops (word/byte/
rmw/alt) overshoot the +300ns slot and quantize to 800ns, which is why the
word_w capture looked like 1-in-8.  Chunk gap + first interval = 6400ns = 8
grant periods (grid-locked); per-line hole gaps bin sharply at 22.0us (no
±1.5us variance visible in this capture — hole appears deterministic).

la_movemr.vcd.gz (movem_r): reads are invisible on the VRAM bus (OE# tied) and
CPU_DTACK# shows zero latency even for stalled cycles (confirmed: DTACK is not
the wait signal at the probe point).  Read stalls appear as stretched CPU_AS#
low: stalled reads (AS low ~424ns) complete at exactly 800ns end-to-end
spacing, and only ~8 of each 16-read movem chunk stall — the other 8 complete
fast (≤200ns).  Reads pair up exactly like writes: wait for slot 0, follow-up
catches the +3 slot with near-zero wait.  Same {0,3} grant for reads and
writes; hw movem_r == movem_w == 11792 apf.

## IMPLEMENTED in rtl/igs023.sv + igs023_bg.sv (sim-verified)

Schedule FSM replaces all phenomenological knobs (CPU_FETCH_LOCK/jitter/pace/
bypass): lock = fg_real_vram_master (13.7us) + BG_HEADSTART_SLOTS=41 ce_pixel
(4.1us; hw total hole ~17.8us); steady state slot_phase counter at ce_pixel,
CPU dispatch on slot_phase ∈ {0,3}, phase anchored at hole end; vblank/
bus_master unscheduled.  CPU write byte order fixed to even-then-odd.
IGS023_BG yields the bus during CPU grant slots (cpu_slot input freezes its
read FSM + drops vram_master; its prefetch buffer absorbs the deferral) —
without this, BG's unaligned burst reads squat on CPU slots and movem_r
randomly misses the +3 grant (was −10%).

Quick-matrix result vs hw_full.jsonl: ALL contended active-window patterns
EXACT (word_r/w, movem_r/w = 0.00%); baselines/vblank ≤0.27%; remaining:
movem_w all +1.57, word_r all −1.36, bus_master word_r −0.87 (sub-tick raw
cycle length).  BIOS boot clean.

## DECODED (2026-07-04c): BG_X scroll dependence of the schedule

Sweep (word_w/movem_r, BG active window, poke 0xb03000):
- movem_r apf is FLAT at 11792 for all BG_X — total CPU slot capacity is
  scroll-invariant.
- word_w: 7856 for bg_x mod 32 in {0..4}, ~8008 NON-INTEGER (per-frame
  metastability, the only nondeterminism seen anywhere) at {8,12}, 8848
  (+12.6%) for {16..31}; exact mod-32 periodicity.

LA captures la_bgx00/04/08/16.vcd.gz (word_w x 3600 frames each):
- Hole END = 11.8us - 100ns x (bg_x mod 32) after SYNC#, single-bin sharp at
  every value (739/739/683/683 counts).  The hole is DETERMINISTIC; the
  earlier "end-anchored +/-1.5us jitter" was a last-write-before-hole loop
  artifact (hole-gap length bimodality 18800/22000 at 2:1 = 3-line chunk
  phase pattern, NOT hole variance).  The BG head-start shortens by one
  pixel slot per pixel of BG scroll alignment within the 32px tile.
- Post-hole transient: exactly 4.0 microcycles of 700ns per line at every
  bg_x (intervals slide the write phase -100ns each: 650->550->450->350->
  steady 250 at bg_x=0), i.e. four 7-slot microcycles while BG consumes its
  prefetch lead, then 8-slot steady state.
- Grid anchor disambiguation: steady phase is hole_end - 350 mod 800 (bgx00
  hole 11800 -> phase 250; bgx04 hole 11400 -> phase 650) — the grant grid
  is HOLE-END-anchored, not sync-anchored.  (bg_x multiples of 8 are
  degenerate mod 800; bgx04 breaks the tie.)
- word_w's +12.6% step is pure loop-phase alignment against the shifted
  grid; capacity does not change.

RTL model updated: IGS023_BG exports scroll_align = scrolled_x[4:0]
(bg_x + rowscroll, latched at APPLY_SCROLL); headstart_cnt load =
BG_HEADSTART_SLOTS(47) - scroll_align (base tuned so the sim hole end is
11800ns after sync at align 0 — measured via FST, was 11200 at 41);
trans_cnt gives 4 post-hole 7-slot microcycles before the 8-slot steady
state.  Sim BG_X staircase now matches hw bin-for-bin (edges at 8/16,
plateaus 7856/8848, mod-32 wrap); the hw 8008-metastable points read 8112
in the deterministic sim (same knife edge).  Quick matrix worst delta
0.87% (movem_w all +0.83).

## DECODED (2026-07-04d): a THIRD CPU slot — grant set is {0, 2, 3}

la_longw.vcd.gz (long_w x 3600): steady repeating interval cycle
[800, 320, 700, 580] per 2 longs (3 microcycles) is unbuildable from slots
{0,3}.  Fit: CPU-eligible slots {0, 2, 3} per microcycle.  Each benchmark
pattern samples a different subset by its natural request spacing:
- movem (request ~250ns after completion): overshoots slot 2, sees {0,3}
  -> 320/480 pairs.
- word loop (~450ns): misses everything until slot 0 -> 800 flat.
- long inter-instruction gap (~900ns): rounds into slot 2 at +1000 ->
  interval 700, then its pair partner misses +1120 and commits at +1600
  -> 580.  Three patterns were needed to expose all three slots.
Sim gap-probe suite after the change: word_r_p1/p4/p8, alt_rw, rmw ALL
contended-EXACT (0.00%); p2 -0.32% is another hw metastable (6661.1
non-integer).  Active-window word/movem unchanged (they cannot reach
slot 2).  Remaining known deltas: hw word ALL r/w asymmetry (hw word_w
loses ~200 apf vs word_r near grid boundaries; sim r ALL now +0.04%,
w ALL +1.43%) and the -0.2..-0.9% bus_master read raw-cycle offset.

## CORRECTED MODEL (2026-07-04e): half-cycle CPU window; transient and
## slot-set readings were artifacts — this supersedes 04b/04d slot sets

Union of post-hole commit phases across la_movem/la_bgx00/la_longw:
movem commits at E+{0,400,700,1200,1500,2000,2300,2800,...} — a steady
800ns grid live IMMEDIATELY from the hole end E, with a contiguous 4-slot
CPU WINDOW [+400,+700] per microcycle.  The microcycle = BG half-cycle
(4 slots, the {0,1,2,1} byte scan) followed by the CPU half.  Unifications:
- "grant slots {0,3}" (04b) and "{0,2,3}" (04d) = the window sampled at
  different request spacings (movem ~250ns, word ~650ns, long ~700/900ns).
- The "4x700ns post-hole transient" (04c) = word's request spacing sliding
  100ns/cycle deeper into the window until it passes the window start,
  then locking at 800ns.  No transient exists.  (The old RTL transient
  produced the same steady anchor E+400 mod 800 — 4x(-100) == -400 — which
  is why only re-entry-sensitive patterns exposed the error.)
- The access held during the hole commits AT E, ahead of BG's first
  half-cycle; its FOLLOWER waits for the first window at E+400 (movem/long
  word2 at +400 on hw — the discriminating observation).
RTL: slot_phase 0-7 anchored at E (BG half = 0-3, CPU window = 4-7,
cpu_slot_grant = slot_phase[2]); held_in_hole one-shot commits the held
access at hole exit and clears on dispatch; trans_cnt deleted.
RESULT: long_r/long_w bg active = 9584.0 EXACT (was +2.50%); sim
reproduces the hw hole-straddle limit cycle ([18.6,18.5,18.6,20.9]us gap
populations, straddle every 4th line, hole end 11800 all lines, re-entry
sequences match within pulse offsets); word/movem/alt/rmw/pads and the
BG_X staircase all unchanged-exact.
Remaining: hw word/byte WRITE ALL-window r/w asymmetry (~200 apf, sim
+1.4%); bus_master reads -0.2..-0.9% (sub-tick raw read cycle).

## DECODED (2026-07-04f): the ALL-window write asymmetry = window-edge
## write straggle, metastable on hardware

Accounting first: la_wall/la_wall2 (word_w WIN_ALL, verified mid-test)
seemed to show ~250 words/frame MORE than the benchmark counts.  These are
SPLIT WRITE PULSES: byte0 at window end (~anchor+764), byte1 336ns later
at anchor+1100 = slot 3 of the following BG half - BG's redundant byte-1
re-read slot, donated to the CPU straggler.  Counting splits as one word,
LA and benchmark agree exactly (RANGE mode: LA = 7856 words + 184 holes
per frame, digit-exact).

Mechanism: a WRITE dispatched in the tail of the last window slot cannot
fit its second WR pulse (48ns pair + recovery) before the window closes;
the odd byte stretches to donated slot 3, stalling the CPU ~1 grant period.
READS complete in-slot at SRAM pace (~30ns/byte - cf. the 33.87MHz FG
crawl) and never straggle: this is the entire hw word/byte ALL-window
read-vs-write deficit (word_r 14505.6 vs word_w 14307.2; ~250 straggles/
frame in free-running ALL mode).  RANGE windows re-align each poll and
avoid tail dispatches entirely - why active windows show no asymmetry.

The edge is METASTABLE on hw: per-frame LA counts vary +/-15 words while
the 3600-frame mean is stable (+/-0.01% across sessions/durations); same
physics as the fractional apf at bg_x=8/12.  The deterministic sim lands
all its boundary dispatches on one ce_50m tick (0% or 100% only), so the
RTL dithers the boundary resolve (straggle_lfsr, p=1/4 empirically best).

RTL: access_straggle (write-only, slot 7 tail, dithered) + donated slot 3
via cpu_slot/vram_cpu_high_free; held-in-hole access exempt.  Result:
word_r ALL +0.04, word_w ALL +0.38, movem_w ALL -0.80; worst full-matrix
delta 0.87% (bus_master read raw-cycle residual).  Gap probes/active
windows/BIOS unchanged-exact.

Addendum (dither characterization): with straggle_lfsr p=1/4, long-run
sim means: word_w ALL +0.36% (stable 10/60/120 frames), movem_w ALL
-1.4..-1.7% (sim's back-to-back-write ride hits the window edge 2-4x more
often than hw's; a single p cannot serve both - fixing movem_w needs
sub-50ns 68k bus-timing fidelity in the ride phases).  Same-command sim
runs remain byte-identical; across DIFFERENT test orderings the LFSR
phase shifts, so 10-frame ALL-window write rows sample within ~+/-1%
(full-matrix movem_w read -3.09% on one ordering).  Use --frames 60+ when
comparing ALL-window write rows precisely.  Chosen default: dither ON -
scattered word/byte writes (the game-typical pattern) are the fidelity
priority; deterministic-off (never straggle) would put word/byte ALL at
+1.43/+1.48%.

## OPEN (2026-07-04g): free-running ride fraction / chunk re-entry

After the bus-footprint restructure (2-tick bus occupancy, DTACK latency
preserved; commit e022695) + the 68k clock-beat model, the hw ALL-window
r/w asymmetry is EMERGENT (sim w-r = -195 vs hw -198 apf, deterministic
straggle at slot7 subslot>=4, no dither).  Remaining: a uniform -1.3% on
free-running ALL rows (word_r/word_w/movem_w alike; RANGE/vblank/actives
exact).  ALL-mode interval comparison (la_wall2 vs sim FST):
- hw spends 37% of intervals at 700ns (window slide) vs sim 16% - hw
  rides ~2.4x longer;
- hw chunk re-entry interval = 2800ns SHARP; sim = 3100/3200 (+300-400ns)
  with an extra 5600 population - sim's post-gap request waits for the
  window where hw is granted earlier;
- 2800 is not an integer number of 800ns cycles => the two commits
  bracket different grant offsets; suggests requests that WAITED through
  the BG half may be granted at slot 3 (donated slot as general re-entry,
  not just straggler byte1) or the window opens earlier for queued
  requests.  Decode next: hole-anchored commit phases of post-gap words
  in la_wall2 (data in hand).

## RESOLVED 04g (2026-07-04h): per-slot arbitration - the soft boundary

Decode of la_wall2 hole-anchored commit phases:
- In-chunk steady words commit at phase 400 (window start) - the loop
  waits through the BG half and is granted when the window opens.
- Post-chunk-gap words commit at phase 0-20 (start of the BG half!) with
  BOTH pulses inside slot 0, ~90% of re-entries; the other ~10% commit at
  phase 400 (the 3100ns gap-length population).
- Address-bus slot map (A0-A3 merged events, E-anchored): EVERY 100ns slot
  carries a bus event (75/175/275/375 + 475/575/675/775) - BG fills all
  slots the CPU does not take; the "window" is a preemption right, not
  exclusive occupancy.

Arbitration model: at each slot boundary the mux re-arbitrates.  Window
slots {4-7}: CPU wins when pending or mid-access.  Slot 0: a pending NEW
request at the boundary (within ~40ns) wins it and completes there; a
mid-access CONTINUATION does not - a late slot-7 write's odd byte loses
slot 0 to BG and waits for the donated slot 3 (reconciles the split-pulse
evidence with the soft boundary).  Slots 1-3: never granted to new
requests (mid-chunk arrivals at phase ~250 wait for the window).

RTL: cpu_boundary_grant = (slot_phase==0 && subslot<2) added to dispatch;
BG yields slot 0 while a CPU access is in flight.  RESULT: worst quick-
matrix delta 0.87% (bus_master read raw-cycle residual, the only row
left); word/movem/byte ALL rows all within +/-0.3%; actives/vblank/gap
probes exact; BIOS clean.  The free-running 700ns ride fraction and the
2800ns chunk re-entry are now emergent.
