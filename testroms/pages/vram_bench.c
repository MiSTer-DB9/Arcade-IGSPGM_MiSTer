#include <stddef.h>

#include "../system.h"
#include "../memory_map.h"
#include "../page.h"
#include "../tilemap.h"
#include "../igs023.h"
#include "../color.h"
#include "../debug_link.h"
#include "../vram_bench_protocol.h"
#include "../util.h"

// VRAM access-timing benchmark, host-driven over the debug link.
// WARNING: keep all variant loops byte-identical outside the chunk, and
// never touch VRAM between chunks (the SCANLINE poll is DTACK-immediate).

#define RX_BUF_SIZE 128
#define TX_BUF_SIZE 64

static u8 rx_buf[RX_BUF_SIZE];
static u16 rx_len;
static u16 command_count;
static u16 error_count;
static u8 last_cmd;
static u8 last_status_code;

// ---- IRQ6 window state machine (cpu_cache_timing.c pattern) ----

typedef enum
{
    TS_DONE,
    TS_START,
    TS_RUN
} TestState;

static volatile TestState test_state;
static volatile u16       test_active;
static volatile u16       test_count;
static volatile u16       cal_at_irq6;   // SCANLINE latched at IRQ6 entry

static void vblank_handler()
{
    cal_at_irq6 = IGS023_SCANLINE_RAW();

    switch (test_state)
    {
        case TS_DONE:
            break;

        case TS_START:
            test_state = TS_RUN;
            test_active = 0xffff;
            break;

        case TS_RUN:
            test_count--;
            if (test_count == 0)
            {
                test_active = 0x0000;
                test_state = TS_DONE;
            }
            break;
    }

    igs023_ack_irq6();
}

static void start_test(u16 count)
{
    test_count = count;
    test_state = TS_START;
    test_active = 0x0000;

    while (test_active == 0) {};   // wait for the next vblank to arm the window
}

// ---- measurement targets ----

// 32-byte access window per target.  VRAM targets sit on the LAST row of each
// tilemap (offscreen) so write variants don't disturb the on-screen text.
static u16 work_buf[16] __attribute__((aligned(32)));

static volatile u8 *target_ptr(u8 target)
{
    switch (target)
    {
    case VRAM_BENCH_TGT_BG_VRAM: return (volatile u8 *)&VRAM->bg[64 * 63];
    case VRAM_BENCH_TGT_FG_VRAM: return (volatile u8 *)&VRAM->fg[64 * 31];
    default:                     return (volatile u8 *)work_buf;
    }
}

// ---- measurement loops ----
// Base register must be reloaded, never incremented - the instruction
// stream has to stay identical across targets.

#define C_NOMEM \
    "moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n" \
    "moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n" \
    "moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n" \
    "moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n moveq #0,%%d2\n"

#define C_WORD_R \
    "move.w 0(%%a0),%%d2\n  move.w 2(%%a0),%%d2\n  move.w 4(%%a0),%%d2\n  move.w 6(%%a0),%%d2\n" \
    "move.w 8(%%a0),%%d2\n  move.w 10(%%a0),%%d2\n move.w 12(%%a0),%%d2\n move.w 14(%%a0),%%d2\n" \
    "move.w 16(%%a0),%%d2\n move.w 18(%%a0),%%d2\n move.w 20(%%a0),%%d2\n move.w 22(%%a0),%%d2\n" \
    "move.w 24(%%a0),%%d2\n move.w 26(%%a0),%%d2\n move.w 28(%%a0),%%d2\n move.w 30(%%a0),%%d2\n"

#define C_WORD_W \
    "move.w %%d3,0(%%a0)\n  move.w %%d3,2(%%a0)\n  move.w %%d3,4(%%a0)\n  move.w %%d3,6(%%a0)\n" \
    "move.w %%d3,8(%%a0)\n  move.w %%d3,10(%%a0)\n move.w %%d3,12(%%a0)\n move.w %%d3,14(%%a0)\n" \
    "move.w %%d3,16(%%a0)\n move.w %%d3,18(%%a0)\n move.w %%d3,20(%%a0)\n move.w %%d3,22(%%a0)\n" \
    "move.w %%d3,24(%%a0)\n move.w %%d3,26(%%a0)\n move.w %%d3,28(%%a0)\n move.w %%d3,30(%%a0)\n"

#define C_LONG_R \
    "move.l 0(%%a0),%%d2\n  move.l 4(%%a0),%%d2\n  move.l 8(%%a0),%%d2\n  move.l 12(%%a0),%%d2\n" \
    "move.l 16(%%a0),%%d2\n move.l 20(%%a0),%%d2\n move.l 24(%%a0),%%d2\n move.l 28(%%a0),%%d2\n"

#define C_LONG_W \
    "move.l %%d3,0(%%a0)\n  move.l %%d3,4(%%a0)\n  move.l %%d3,8(%%a0)\n  move.l %%d3,12(%%a0)\n" \
    "move.l %%d3,16(%%a0)\n move.l %%d3,20(%%a0)\n move.l %%d3,24(%%a0)\n move.l %%d3,28(%%a0)\n"

#define C_BYTE_R_HI \
    "move.b 0(%%a0),%%d2\n  move.b 2(%%a0),%%d2\n  move.b 4(%%a0),%%d2\n  move.b 6(%%a0),%%d2\n" \
    "move.b 8(%%a0),%%d2\n  move.b 10(%%a0),%%d2\n move.b 12(%%a0),%%d2\n move.b 14(%%a0),%%d2\n" \
    "move.b 16(%%a0),%%d2\n move.b 18(%%a0),%%d2\n move.b 20(%%a0),%%d2\n move.b 22(%%a0),%%d2\n" \
    "move.b 24(%%a0),%%d2\n move.b 26(%%a0),%%d2\n move.b 28(%%a0),%%d2\n move.b 30(%%a0),%%d2\n"

#define C_BYTE_R_LO \
    "move.b 1(%%a0),%%d2\n  move.b 3(%%a0),%%d2\n  move.b 5(%%a0),%%d2\n  move.b 7(%%a0),%%d2\n" \
    "move.b 9(%%a0),%%d2\n  move.b 11(%%a0),%%d2\n move.b 13(%%a0),%%d2\n move.b 15(%%a0),%%d2\n" \
    "move.b 17(%%a0),%%d2\n move.b 19(%%a0),%%d2\n move.b 21(%%a0),%%d2\n move.b 23(%%a0),%%d2\n" \
    "move.b 25(%%a0),%%d2\n move.b 27(%%a0),%%d2\n move.b 29(%%a0),%%d2\n move.b 31(%%a0),%%d2\n"

#define C_BYTE_W_HI \
    "move.b %%d3,0(%%a0)\n  move.b %%d3,2(%%a0)\n  move.b %%d3,4(%%a0)\n  move.b %%d3,6(%%a0)\n" \
    "move.b %%d3,8(%%a0)\n  move.b %%d3,10(%%a0)\n move.b %%d3,12(%%a0)\n move.b %%d3,14(%%a0)\n" \
    "move.b %%d3,16(%%a0)\n move.b %%d3,18(%%a0)\n move.b %%d3,20(%%a0)\n move.b %%d3,22(%%a0)\n" \
    "move.b %%d3,24(%%a0)\n move.b %%d3,26(%%a0)\n move.b %%d3,28(%%a0)\n move.b %%d3,30(%%a0)\n"

#define C_BYTE_W_LO \
    "move.b %%d3,1(%%a0)\n  move.b %%d3,3(%%a0)\n  move.b %%d3,5(%%a0)\n  move.b %%d3,7(%%a0)\n" \
    "move.b %%d3,9(%%a0)\n  move.b %%d3,11(%%a0)\n move.b %%d3,13(%%a0)\n move.b %%d3,15(%%a0)\n" \
    "move.b %%d3,17(%%a0)\n move.b %%d3,19(%%a0)\n move.b %%d3,21(%%a0)\n move.b %%d3,23(%%a0)\n" \
    "move.b %%d3,25(%%a0)\n move.b %%d3,27(%%a0)\n move.b %%d3,29(%%a0)\n move.b %%d3,31(%%a0)\n"

#define C_MOVEM_R "movem.l (%%a0),%%d4-%%d7/%%a2-%%a5\n"
#define C_MOVEM_W "movem.l %%d4-%%d7/%%a2-%%a5,(%%a0)\n"

#define C_RMW \
    "addq.w #1,0(%%a0)\n  addq.w #1,2(%%a0)\n  addq.w #1,4(%%a0)\n  addq.w #1,6(%%a0)\n" \
    "addq.w #1,8(%%a0)\n  addq.w #1,10(%%a0)\n addq.w #1,12(%%a0)\n addq.w #1,14(%%a0)\n"

// Gap-probe chunks: 16 word reads with N nops (N*4 idle cycles) between them.
#define REP16(X) X X X X X X X X X X X X X X X X
#define REP8(X)  X X X X X X X X
#define A_RP1 "move.w 0(%%a0),%%d2\n nop\n"
#define A_RP2 "move.w 0(%%a0),%%d2\n nop\n nop\n"
#define A_RP4 "move.w 0(%%a0),%%d2\n nop\n nop\n nop\n nop\n"
#define A_RP8 "move.w 0(%%a0),%%d2\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
#define C_WORD_R_P1 REP16(A_RP1)
#define C_WORD_R_P2 REP16(A_RP2)
#define C_WORD_R_P4 REP16(A_RP4)
#define C_WORD_R_P8 REP16(A_RP8)

// Alternating separate read / write instructions (8 pairs = 16 accesses).
#define A_ALT "move.w 0(%%a0),%%d2\n move.w %%d3,2(%%a0)\n"
#define C_ALT_RW REP8(A_ALT)

#define P_NONE  ""
#define P_WVAL  "moveq #0,%%d3\n"
#define P_MOVEM \
    "moveq #0,%%d4\n moveq #0,%%d5\n moveq #0,%%d6\n moveq #0,%%d7\n" \
    "movea.l %%d4,%%a2\n movea.l %%d4,%%a3\n movea.l %%d4,%%a4\n movea.l %%d4,%%a5\n"

// Free-running loop: chunks while test_active.  Identical outer frame for all
// variants; base is a memory operand so register pressure stays uniform.
#define DEF_RUN_ALL(name, PRELOAD, CHUNK, ...)                                \
static u32 __attribute__((noinline)) run_##name##_all(volatile u8 *base)      \
{                                                                             \
    u32 count;                                                                \
    __asm__ volatile (                                                        \
        "       moveq   #0,%[cnt]        \n"                                  \
        "       move.l  %[base],%%a0     \n"                                  \
        PRELOAD                                                               \
        "1:     tst.w   %[active]        \n"                                  \
        "       beq     2f               \n"                                  \
        CHUNK                                                                 \
        "       addq.l  #1,%[cnt]        \n"                                  \
        "       jra     1b               \n"                                  \
        "2:                              \n"                                  \
        : [cnt] "=&d" (count)                                                 \
        : [base] "m" (base), [active] "m" (test_active)                       \
        : "a0", __VA_ARGS__, "cc", "memory");                                 \
    return count;                                                             \
}

// SCANLINE polls must stay BETWEEN chunks, never inside them.
#define DEF_RUN_RANGE(name, PRELOAD, CHUNK, ...)                              \
static u32 __attribute__((noinline)) run_##name##_range(                      \
    volatile u8 *base, u16 lo, u16 hi, u32 *spins)                            \
{                                                                             \
    u32 count, sp;                                                            \
    __asm__ volatile (                                                        \
        "       moveq   #0,%[cnt]        \n"                                  \
        "       moveq   #0,%[sp]         \n"                                  \
        "       move.l  %[base],%%a0     \n"                                  \
        "       lea     0xb07000,%%a1    \n"                                  \
        PRELOAD                                                               \
        "1:     tst.w   %[active]        \n"                                  \
        "       beq     4f               \n"                                  \
        "       move.w  (%%a1),%%d1      \n"                                  \
        "       cmp.w   %[lo],%%d1       \n"                                  \
        "       bcs     3f               \n"                                  \
        "       cmp.w   %[hi],%%d1       \n"                                  \
        "       bcc     3f               \n"                                  \
        CHUNK                                                                 \
        "       addq.l  #1,%[cnt]        \n"                                  \
        "       jra     1b               \n"                                  \
        "3:     addq.l  #1,%[sp]         \n"                                  \
        "       jra     1b               \n"                                  \
        "4:                              \n"                                  \
        : [cnt] "=&d" (count), [sp] "=&d" (sp)                                \
        : [base] "m" (base), [lo] "m" (lo), [hi] "m" (hi),                    \
          [active] "m" (test_active)                                          \
        : "a0", "a1", "d1", __VA_ARGS__, "cc", "memory");                     \
    *spins = sp;                                                              \
    return count;                                                             \
}

#define DEF_RUNNERS(name, PRELOAD, CHUNK, ...)         \
    DEF_RUN_ALL(name, PRELOAD, CHUNK, __VA_ARGS__)     \
    DEF_RUN_RANGE(name, PRELOAD, CHUNK, __VA_ARGS__)

DEF_RUNNERS(nomem,     P_NONE,  C_NOMEM,     "d2")
DEF_RUNNERS(word_r,    P_NONE,  C_WORD_R,    "d2")
DEF_RUNNERS(word_w,    P_WVAL,  C_WORD_W,    "d3")
DEF_RUNNERS(long_r,    P_NONE,  C_LONG_R,    "d2")
DEF_RUNNERS(long_w,    P_WVAL,  C_LONG_W,    "d3")
DEF_RUNNERS(byte_r_hi, P_NONE,  C_BYTE_R_HI, "d2")
DEF_RUNNERS(byte_r_lo, P_NONE,  C_BYTE_R_LO, "d2")
DEF_RUNNERS(byte_w_hi, P_WVAL,  C_BYTE_W_HI, "d3")
DEF_RUNNERS(byte_w_lo, P_WVAL,  C_BYTE_W_LO, "d3")
DEF_RUNNERS(movem_r,   P_NONE,  C_MOVEM_R,   "d4", "d5", "d6", "d7", "a2", "a3", "a4", "a5")
DEF_RUNNERS(movem_w,   P_MOVEM, C_MOVEM_W,   "d4", "d5", "d6", "d7", "a2", "a3", "a4", "a5")
DEF_RUNNERS(rmw,       P_NONE,  C_RMW,       "d2")
DEF_RUNNERS(word_r_p1, P_NONE,  C_WORD_R_P1, "d2")
DEF_RUNNERS(word_r_p2, P_NONE,  C_WORD_R_P2, "d2")
DEF_RUNNERS(word_r_p4, P_NONE,  C_WORD_R_P4, "d2")
DEF_RUNNERS(word_r_p8, P_NONE,  C_WORD_R_P8, "d2")
DEF_RUNNERS(alt_rw,    P_WVAL,  C_ALT_RW,    "d2", "d3")

typedef u32 (*RunAllFn)(volatile u8 *base);
typedef u32 (*RunRangeFn)(volatile u8 *base, u16 lo, u16 hi, u32 *spins);

static const RunAllFn run_all_tab[VRAM_BENCH_NUM_VARIANTS] = {
    run_nomem_all,   run_word_r_all,    run_word_w_all,    run_long_r_all,
    run_long_w_all,  run_byte_r_hi_all, run_byte_r_lo_all, run_byte_w_hi_all,
    run_byte_w_lo_all, run_movem_r_all, run_movem_w_all,   run_rmw_all,
    run_word_r_p1_all, run_word_r_p2_all, run_word_r_p4_all, run_word_r_p8_all,
    run_alt_rw_all,
};

static const RunRangeFn run_range_tab[VRAM_BENCH_NUM_VARIANTS] = {
    run_nomem_range,   run_word_r_range,    run_word_w_range,    run_long_r_range,
    run_long_w_range,  run_byte_r_hi_range, run_byte_r_lo_range, run_byte_w_hi_range,
    run_byte_w_lo_range, run_movem_r_range, run_movem_w_range,   run_rmw_range,
    run_word_r_p1_range, run_word_r_p2_range, run_word_r_p4_range, run_word_r_p8_range,
    run_alt_rw_range,
};

// ---- results ----

typedef struct
{
    u8  variant, target, window, flags;
    u16 frames, win_start, win_end, accesses_per_chunk;
    u32 chunks, poll_spins, vbl_start, vbl_end;
} BenchResult;

static BenchResult last_result;
static bool have_result;

// ---- protocol helpers (ics_remote.c pattern) ----

static u16 get_be16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

static u32 get_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void send_response(u8 seq, u8 status, const u8 *payload, u8 payload_len)
{
    u8 tx[TX_BUF_SIZE];
    u16 total = VRAM_BENCH_HEADER_SIZE + payload_len;
    if (total > TX_BUF_SIZE)
    {
        payload_len = TX_BUF_SIZE - VRAM_BENCH_HEADER_SIZE;
        total = TX_BUF_SIZE;
    }

    tx[0] = VRAM_BENCH_RSP_MAGIC0;
    tx[1] = VRAM_BENCH_RSP_MAGIC1;
    tx[2] = VRAM_BENCH_VERSION;
    tx[3] = seq;
    tx[4] = status;
    tx[5] = payload_len;
    if (payload_len && payload)
        memcpy(tx + VRAM_BENCH_HEADER_SIZE, payload, payload_len);

    debug_link_write(tx, total);
    last_status_code = status;
    if (status == VRAM_BENCH_STATUS_OK)
        command_count++;
    else
        error_count++;
}

// ---- command handlers ----

static void handle_ping(u8 seq)
{
    u8 out[7];
    put_be16(out + 0, VRAM_BENCH_PING_MAGIC);
    out[2] = VRAM_BENCH_VERSION;
    put_be32(out + 3, igs023_get_vblank_count());
    send_response(seq, VRAM_BENCH_STATUS_OK, out, sizeof(out));
}

static void handle_info(u8 seq)
{
    u8 out[7];
    put_be32(out + 0, 20000000);  // 68k clock
    out[4] = VRAM_BENCH_NUM_VARIANTS;
    out[5] = VRAM_BENCH_NUM_TARGETS;
    out[6] = VRAM_BENCH_NUM_WINDOWS;
    send_response(seq, VRAM_BENCH_STATUS_OK, out, sizeof(out));
}

static void pack_result(u8 *out, const BenchResult *r)
{
    out[0] = r->variant;
    out[1] = r->target;
    out[2] = r->window;
    out[3] = r->flags;
    put_be16(out + 4, r->frames);
    put_be16(out + 6, r->win_start);
    put_be16(out + 8, r->win_end);
    put_be16(out + 10, r->accesses_per_chunk);
    put_be32(out + 12, r->chunks);
    put_be32(out + 16, r->poll_spins);
    put_be32(out + 20, r->vbl_start);
    put_be32(out + 24, r->vbl_end);
}

static void apply_flags(u8 flags)
{
    u16 set = 0;
    if (flags & VRAM_BENCH_FLAG_BUS_MASTER) set |= IGS023_CTRL_BUS_MASTER;
    if (flags & VRAM_BENCH_FLAG_SPRITE_DMA) set |= IGS023_CTRL_DMA;
    if (flags & VRAM_BENCH_FLAG_LAYERS_OFF) set |= IGS023_CTRL_DISABLE_FG | IGS023_CTRL_DISABLE_BG;
    if (set)
        IGS023_CTRL_OR(set);
}

static void restore_flags(void)
{
    IGS023_CTRL_AND((u16)~(IGS023_CTRL_BUS_MASTER | IGS023_CTRL_DMA |
                           IGS023_CTRL_DISABLE_FG | IGS023_CTRL_DISABLE_BG));
}

static void handle_run_test(u8 seq, const u8 *payload, u8 len)
{
    u8 out[VRAM_BENCH_RESULT_SIZE];
    BenchResult r;

    if (len != 10)
    {
        send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0);
        return;
    }

    r.variant   = payload[0];
    r.target    = payload[1];
    r.window    = payload[2];
    r.flags     = payload[3];
    r.frames    = get_be16(payload + 4);
    r.win_start = get_be16(payload + 6);
    r.win_end   = get_be16(payload + 8);
    r.accesses_per_chunk = VRAM_BENCH_ACCESSES_PER_CHUNK;
    r.poll_spins = 0;

    if (r.variant >= VRAM_BENCH_NUM_VARIANTS || r.target >= VRAM_BENCH_NUM_TARGETS ||
        r.window >= VRAM_BENCH_NUM_WINDOWS || r.frames == 0 ||
        (r.window != VRAM_BENCH_WIN_ALL &&
         (r.window == VRAM_BENCH_WIN_RANGE ? r.win_end <= r.win_start : false)))
    {
        send_response(seq, VRAM_BENCH_STATUS_BAD_PARAM, NULL, 0);
        return;
    }

    if (r.window == VRAM_BENCH_WIN_LINE)
        r.win_end = r.win_start + 1;

    volatile u8 *base = target_ptr(r.target);

    apply_flags(r.flags);
    r.vbl_start = igs023_get_vblank_count();
    start_test(r.frames);
    if (r.window == VRAM_BENCH_WIN_ALL)
        r.chunks = run_all_tab[r.variant](base);
    else
    {
        u32 spins = 0;
        r.chunks = run_range_tab[r.variant](base, r.win_start, r.win_end, &spins);
        r.poll_spins = spins;
    }
    r.vbl_end = igs023_get_vblank_count();
    restore_flags();

    last_result = r;
    have_result = true;

    pack_result(out, &r);
    send_response(seq, VRAM_BENCH_STATUS_OK, out, VRAM_BENCH_RESULT_SIZE);
}

static void handle_get_result(u8 seq)
{
    u8 out[VRAM_BENCH_RESULT_SIZE];
    if (!have_result)
    {
        send_response(seq, VRAM_BENCH_STATUS_NO_RESULT, NULL, 0);
        return;
    }
    pack_result(out, &last_result);
    send_response(seq, VRAM_BENCH_STATUS_OK, out, VRAM_BENCH_RESULT_SIZE);
}

static void handle_cal_scanline(u8 seq, const u8 *payload, u8 len)
{
    u8 out[16];
    u16 frames;
    u16 vmin = 0xffff, vmax = 0, after_wrap = 0, prev;
    u32 samples = 0, wraps = 0;

    if (len != 2)
    {
        send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0);
        return;
    }
    frames = get_be16(payload);
    if (frames == 0)
    {
        send_response(seq, VRAM_BENCH_STATUS_BAD_PARAM, NULL, 0);
        return;
    }

    start_test(frames);
    prev = IGS023_SCANLINE_RAW();
    while (test_active)
    {
        u16 v = IGS023_SCANLINE_RAW();
        samples++;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        if (v < prev)
        {
            wraps++;
            after_wrap = v;
        }
        prev = v;
    }

    put_be16(out + 0, vmin);
    put_be16(out + 2, vmax);
    put_be16(out + 4, cal_at_irq6);
    put_be16(out + 6, after_wrap);
    put_be32(out + 8, samples);
    put_be32(out + 12, wraps);
    send_response(seq, VRAM_BENCH_STATUS_OK, out, sizeof(out));
}

static void handle_request(const u8 *req)
{
    u8 seq = req[3];
    u8 cmd = req[4];
    u8 len = req[5];
    const u8 *payload = req + VRAM_BENCH_HEADER_SIZE;

    last_cmd = cmd;

    if (req[2] != VRAM_BENCH_VERSION)
    {
        send_response(seq, VRAM_BENCH_STATUS_BAD_VERSION, NULL, 0);
        return;
    }

    switch (cmd)
    {
    case VRAM_BENCH_CMD_PING:
        if (len == 0) handle_ping(seq); else send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0);
        break;
    case VRAM_BENCH_CMD_INFO:
        if (len == 0) handle_info(seq); else send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0);
        break;
    case VRAM_BENCH_CMD_RUN_TEST:
        handle_run_test(seq, payload, len);
        break;
    case VRAM_BENCH_CMD_GET_RESULT:
        if (len == 0) handle_get_result(seq); else send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0);
        break;
    case VRAM_BENCH_CMD_CAL_SCANLINE:
        handle_cal_scanline(seq, payload, len);
        break;
    case VRAM_BENCH_CMD_PEEK16:
    {
        u8 out[2];
        if (len != 4) { send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0); break; }
        put_be16(out, *(volatile u16 *)get_be32(payload));
        send_response(seq, VRAM_BENCH_STATUS_OK, out, sizeof(out));
        break;
    }
    case VRAM_BENCH_CMD_POKE16:
        if (len != 6) { send_response(seq, VRAM_BENCH_STATUS_BAD_LENGTH, NULL, 0); break; }
        *(volatile u16 *)get_be32(payload) = get_be16(payload + 4);
        send_response(seq, VRAM_BENCH_STATUS_OK, NULL, 0);
        break;
    default:
        send_response(seq, VRAM_BENCH_STATUS_BAD_CMD, NULL, 0);
        break;
    }
}

static void drop_rx(u16 count)
{
    if (count >= rx_len)
    {
        rx_len = 0;
        return;
    }
    for (u16 i = 0; i < rx_len - count; i++)
        rx_buf[i] = rx_buf[i + count];
    rx_len -= count;
}

static void process_rx(void)
{
    while (rx_len >= VRAM_BENCH_HEADER_SIZE)
    {
        if (rx_buf[0] != VRAM_BENCH_REQ_MAGIC0 || rx_buf[1] != VRAM_BENCH_REQ_MAGIC1)
        {
            drop_rx(1);
            error_count++;
            continue;
        }

        u16 total = VRAM_BENCH_HEADER_SIZE + rx_buf[5];
        if (total > RX_BUF_SIZE)
        {
            drop_rx(1);
            error_count++;
            continue;
        }
        if (rx_len < total)
            return;

        handle_request(rx_buf);
        drop_rx(total);
    }
}

static void poll_debug_link(void)
{
    if (!debug_link_check_active())
    {
        rx_len = 0;
        return;
    }

    if (rx_len < RX_BUF_SIZE)
    {
        int got = debug_link_read(rx_buf + rx_len, RX_BUF_SIZE - rx_len);
        if (got > 0)
            rx_len += (u16)got;
    }
    process_rx();
}

// ---- page ----

static void init(void)
{
    igs023_init();
    text_reset();
    set_default_palette();

    // Deterministic layer state: BG cleared (tile 0), layers enabled so the
    // fetchers behave exactly as in a game.  IRQ4 off, bus_master off, DMA off.
    memset(VRAM->bg, 0, sizeof(VRAM->bg));
    IGS023_CTRL_AND((u16)~(IGS023_CTRL_BUS_MASTER | IGS023_CTRL_DMA | IGS023_CTRL_IRQ4_EN));

    rx_len = 0;
    command_count = 0;
    error_count = 0;
    last_cmd = 0;
    last_status_code = 0;
    have_result = false;
    test_state = TS_DONE;
    test_active = 0;
}

static void update(void)
{
    igs023_wait_vblank();
    poll_debug_link();

    text_color(1);
    text_cursor(2, 2);
    text("VRAM BENCH\n");
    textf("LINK %s RX %04X\n", debug_link_check_active() ? "ACTIVE" : "INACTIVE", rx_len);
    textf("CMD %02X STAT %02X OK %04X ERR %04X\n", last_cmd, last_status_code, command_count, error_count);
    if (have_result)
    {
        textf("VAR %02X TGT %02X WIN %02X FLG %02X\n",
              last_result.variant, last_result.target, last_result.window, last_result.flags);
        textf("CHUNKS %08X SPINS %08X\n", last_result.chunks, last_result.poll_spins);
    }
    textf("VBL %05X SL %03X\n", (u16)igs023_get_vblank_count(), IGS023_SCANLINE_RAW());
}

PAGE_REGISTER_IRQ(vram_bench, init, update, NULL, NULL, vblank_handler);
