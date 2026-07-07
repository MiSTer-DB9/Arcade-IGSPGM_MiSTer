#if !defined(VRAM_BENCH_PROTOCOL_H)
#define VRAM_BENCH_PROTOCOL_H 1

/* VRAM access-timing benchmark debug-link protocol.
 *
 * Framing matches ics_remote: 6-byte header + payload, big-endian fields.
 *   request:  'V' 'B' ver seq cmd    len [payload]
 *   response: 'v' 'b' ver seq status len [payload]
 * RUN_TEST is synchronous: the page goes deaf for the duration of the test
 * (frames + 2 vblanks) and replies with the RESULT record when done.  Host
 * must size its read timeout accordingly; GET_RESULT is the recovery path.
 */

#define VRAM_BENCH_REQ_MAGIC0 0x56 /* V */
#define VRAM_BENCH_REQ_MAGIC1 0x42 /* B */
#define VRAM_BENCH_RSP_MAGIC0 0x76 /* v */
#define VRAM_BENCH_RSP_MAGIC1 0x62 /* b */
#define VRAM_BENCH_VERSION    0x01
#define VRAM_BENCH_HEADER_SIZE 6

#define VRAM_BENCH_STATUS_OK          0x00
#define VRAM_BENCH_STATUS_BAD_VERSION 0x02
#define VRAM_BENCH_STATUS_BAD_LENGTH  0x03
#define VRAM_BENCH_STATUS_BAD_CMD     0x04
#define VRAM_BENCH_STATUS_BAD_PARAM   0x05
#define VRAM_BENCH_STATUS_NO_RESULT   0x06

/* response: u16 magic(0xB37C), u8 proto_ver, u32 vblank_count */
#define VRAM_BENCH_CMD_PING         0x01
#define VRAM_BENCH_PING_MAGIC       0xB37C
/* response: u32 cpu_hz, u8 n_variants, u8 n_targets, u8 n_windows */
#define VRAM_BENCH_CMD_INFO         0x02
/* payload: u8 variant, target, window, flags; u16 frames, win_start, win_end.
 * response: RESULT (28 B), sent after the test completes. */
#define VRAM_BENCH_CMD_RUN_TEST     0x10
/* response: last RESULT, or status NO_RESULT */
#define VRAM_BENCH_CMD_GET_RESULT   0x11
/* payload: u16 frames.
 * response: u16 min, max, at_irq6, after_wrap; u32 samples, wraps (16 B) */
#define VRAM_BENCH_CMD_CAL_SCANLINE 0x20
/* payload: u32 addr; response: u16 value */
#define VRAM_BENCH_CMD_PEEK16       0x30
/* payload: u32 addr, u16 value */
#define VRAM_BENCH_CMD_POKE16       0x31

/* RESULT record (28 B, big-endian):
 *   [0]  u8  variant   [1] u8 target   [2] u8 window   [3] u8 flags
 *   [4]  u16 frames    [6] u16 win_start  [8] u16 win_end
 *   [10] u16 accesses_per_chunk
 *   [12] u32 chunks    [16] u32 poll_spins
 *   [20] u32 vbl_start [24] u32 vbl_end
 */
#define VRAM_BENCH_RESULT_SIZE 28

/* Access variants.  Every memory variant executes an identical outer loop with
 * a 16-word-access unrolled chunk; only the access instructions differ. */
#define VRAM_BENCH_VAR_NOMEM     0  /* register-only reference chunk */
#define VRAM_BENCH_VAR_WORD_R    1
#define VRAM_BENCH_VAR_WORD_W    2
#define VRAM_BENCH_VAR_LONG_R    3
#define VRAM_BENCH_VAR_LONG_W    4
#define VRAM_BENCH_VAR_BYTE_R_HI 5  /* even addresses (UDS) */
#define VRAM_BENCH_VAR_BYTE_R_LO 6  /* odd addresses (LDS) */
#define VRAM_BENCH_VAR_BYTE_W_HI 7
#define VRAM_BENCH_VAR_BYTE_W_LO 8
#define VRAM_BENCH_VAR_MOVEM_R   9  /* movem.l (a0),8 regs = 16 word reads */
#define VRAM_BENCH_VAR_MOVEM_W   10
#define VRAM_BENCH_VAR_RMW       11 /* 8 x addq.w #1,(o,a0) = 8 reads + 8 writes */
/* Gap-probe variants: word read followed by N nops (N*4 idle CPU cycles
 * between accesses).  Throughput-vs-gap on hardware fingerprints the
 * arbitration grant structure (contiguous lock vs per-grant vs fragmented). */
#define VRAM_BENCH_VAR_WORD_R_P1 12
#define VRAM_BENCH_VAR_WORD_R_P2 13
#define VRAM_BENCH_VAR_WORD_R_P4 14
#define VRAM_BENCH_VAR_WORD_R_P8 15
/* Alternating separate read / write instructions (8 pairs = 16 accesses) -
 * distinguishes R<->W direction-turnaround cost from rmw's single-instruction
 * read-modify-write. */
#define VRAM_BENCH_VAR_ALT_RW    16
#define VRAM_BENCH_NUM_VARIANTS  17

#define VRAM_BENCH_TGT_WORK_RAM 0
#define VRAM_BENCH_TGT_BG_VRAM  1
#define VRAM_BENCH_TGT_FG_VRAM  2
#define VRAM_BENCH_NUM_TARGETS  3

/* Windows: ALL = free-running for the whole armed period.  RANGE = chunks run
 * only while win_start <= SCANLINE < win_end (polled between chunks).  LINE =
 * RANGE with win_end = win_start + 1. */
#define VRAM_BENCH_WIN_ALL   0
#define VRAM_BENCH_WIN_RANGE 1
#define VRAM_BENCH_WIN_LINE  2
#define VRAM_BENCH_NUM_WINDOWS 3

#define VRAM_BENCH_FLAG_BUS_MASTER 0x01 /* ctrl[14] bit10: CPU never stalled */
#define VRAM_BENCH_FLAG_SPRITE_DMA 0x02 /* ctrl[14] bit0 */
#define VRAM_BENCH_FLAG_LAYERS_OFF 0x04 /* ctrl[14] DISABLE_FG|DISABLE_BG */

#define VRAM_BENCH_ACCESSES_PER_CHUNK 16

#endif
