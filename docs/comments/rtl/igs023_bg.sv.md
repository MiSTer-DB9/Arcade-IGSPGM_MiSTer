# rtl/igs023_bg.sv — commentary

### `cpu_slot` yield / `vram_master`

> assign vram_master = vram_reading & ~cpu_slot;

During CPU grant slots the BG fetcher yields the bus and freezes its read
FSM.  Holding the state keeps the issued-address/latched-data pairing
intact: the registered address settles back on the bus before the next
ce_pixel tick latches, so no corrupt data is captured.  BG can afford the
deferral because it runs ahead of scanout (the scheduler's head-start
phase) and buffers a column of pixels.

### `headstart` / `HEADSTART_SLOTS` / `scroll_align`

> assign headstart = |headstart_cnt;

The post-FG prefetch lock: on real hardware, after the FG line fetch
releases, BG streams VRAM at pixel rate building its column lead while the
CPU stays locked out — the scheduler's "hole" phase 2.  The counter loads
at the `fg_fetching` (fg_real_vram_master) falling edge with
HEADSTART_SLOTS − scroll_align and decrements per ce_pixel; igs023 keeps
its slot grid at 0 while `headstart` is high, anchoring the microcycle at
the hole end.

`scroll_align` is the per-line scrolled x alignment within the 32 px tile
(bg_x + per-line rowscroll), latched at APPLY_SCROLL — the head-start is
one pixel slot shorter per pixel of alignment.  Measured hardware law:
hole end = 11.8 µs − 100 ns × (scroll mod 32) after sync, which falls out
of "prefetch runs to a fixed number of whole tile columns past the scanout
start".  HEADSTART_SLOTS (47) is calibrated so the sim hole end lands at
11.8 µs at alignment 0.  APPLY_SCROLL happens right after the FG
fpga-early release, so scroll_align is stable long before the counter
loads at FG real release.  Unverified for global_flip_x — all LA data is
unflipped; a flipped board might need the mirrored alignment
(~scrolled_x_r).
