#ifndef SDR_STRESS_PROTOCOL_H
#define SDR_STRESS_PROTOCOL_H

// SDR ROM-cache stress test, host-driven over the debug link.
//
// Measures 68k throughput on loops with controlled ROM-cache behavior while
// the sprite engine generates configurable A/B-ROM fetch load on the shared
// SDRAM.  Framing mirrors vram_bench: [magic2|ver|seq|cmd|len]+payload,
// big-endian, responses echo seq.  RUN_TEST is synchronous - the page is
// deaf for the duration of the test.

#define SDR_STRESS_REQ_MAGIC0 'S'
#define SDR_STRESS_REQ_MAGIC1 'S'
#define SDR_STRESS_RSP_MAGIC0 's'
#define SDR_STRESS_RSP_MAGIC1 's'
#define SDR_STRESS_VERSION    1

#define SDR_STRESS_CMD_PING       0x01
#define SDR_STRESS_CMD_RUN_TEST   0x10
#define SDR_STRESS_CMD_GET_RESULT 0x11

#define SDR_STRESS_STATUS_OK          0x00
#define SDR_STRESS_STATUS_BAD_VERSION 0x02
#define SDR_STRESS_STATUS_BAD_LENGTH  0x03
#define SDR_STRESS_STATUS_BAD_CMD     0x04
#define SDR_STRESS_STATUS_BAD_PARAM   0x05
#define SDR_STRESS_STATUS_NO_RESULT   0x06

#define SDR_STRESS_PING_MAGIC 0x5D51

// Variants
//  NOMEM    - register-only chunk (raw CPU cadence + IRQ overhead control)
//  ROM_HOT  - 16 reads of one ROM longword (cache-hit path cost)
//  ROM_MISS - 16 reads at 2KB stride (same direct-mapped index, different
//             tags: every read is a conflict miss)
//  ROM_CODE - one call through a 16KB nop blob (sequential prefetch: one
//             miss per 8-byte line, the game-like fetch pattern)
#define SDR_STRESS_VAR_NOMEM    0
#define SDR_STRESS_VAR_ROM_HOT  1
#define SDR_STRESS_VAR_ROM_MISS 2
#define SDR_STRESS_VAR_ROM_CODE 3
#define SDR_STRESS_NUM_VARIANTS 4

// RUN_TEST payload (8 bytes):
//  u8  variant
//  u8  flags        (reserved, 0)
//  u16 frames
//  u16 sprite_count (0..224; static seeded-random list, DMA enabled)
//  u16 seed         (sprite LFSR seed; 0 -> default)
//
// RESULT payload (20 bytes):
//  u8  variant, u8 flags, u16 frames, u16 sprite_count, u16 accesses_per_chunk
//  u32 chunks, u32 vbl_start, u32 vbl_end
#define SDR_STRESS_RESULT_SIZE 20

#define SDR_STRESS_MAX_SPRITES 224

#endif
