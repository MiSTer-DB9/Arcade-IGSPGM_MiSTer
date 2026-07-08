#include <stddef.h>

#include "../system.h"
#include "../memory_map.h"
#include "../page.h"
#include "../tilemap.h"
#include "../igs023.h"
#include "../color.h"
#include "../debug_link.h"
#include "../sdr_stress_protocol.h"
#include "../util.h"

// SDR ROM-cache stress test: 68k loop throughput under ROM-cache misses
// while random sprites generate A/B-ROM SDRAM load.

#define RX_BUF_SIZE 128
#define TX_BUF_SIZE 64

static u8 rx_buf[RX_BUF_SIZE];
static u16 rx_len;
static u16 command_count;
static u16 error_count;
static u8 last_cmd;
static u8 last_status_code;

// must stay bigger than the rom_cache or every pass becomes a hit
__asm__(
    "        .text\n"
    "        .align 2\n"
    "        .global sdr_code_blob\n"
    "sdr_code_blob:\n"
    "        .rept 16384\n"
    "        nop\n"
    "        .endr\n"
    "        rts\n");
extern void sdr_code_blob(void);

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

static void vblank_handler()
{
    // pages with an IRQ6 handler MUST ack it themselves or the system
    // livelocks in an IRQ6 storm
    igs023_ack_irq6();

    switch (test_state)
    {
        case TS_DONE:
            break;

        case TS_START:
            test_state = TS_RUN;
            test_active = 0xffff;
            break;

        case TS_RUN:
            if (--test_count == 0)
            {
                test_active = 0x0000;
                test_state = TS_DONE;
            }
            break;
    }
}

static void start_test(u16 count)
{
    test_count = count;
    test_active = 0x0000;
    test_state = TS_START;
    while (test_active == 0) {};   // wait for the next vblank to arm the window
}

// ---- measurement loops -------------------------------------------------
// Identical outer frame; only the chunk differs.  base is the ROM read
// pointer for the data variants.

#define DEF_RUN(name, CHUNK, ...)                                             \
static u32 __attribute__((noinline)) run_##name(volatile u8 *base)            \
{                                                                             \
    u32 count;                                                                \
    __asm__ volatile (                                                        \
        "       moveq   #0,%[cnt]        \n"                                  \
        "       move.l  %[base],%%a0     \n"                                  \
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

#define C_NOMEM \
    "moveq #0,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n" \
    "addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n" \
    "addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n" \
    "addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n addq.l #1,%%d4\n"

#define C_ROM_HOT \
    "move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n" \
    "move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n" \
    "move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n" \
    "move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n move.l (%%a0),%%d4\n"

// stride must equal the rom_cache size for the index-alias conflict miss
#define C_ROM_MISS \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n" \
    "move.l (0,%%a0),%%d4\n move.l (16384,%%a0),%%d4\n"

DEF_RUN(nomem,    C_NOMEM,    "d4")
DEF_RUN(rom_hot,  C_ROM_HOT,  "d4")
DEF_RUN(rom_miss, C_ROM_MISS, "d4")

static u32 __attribute__((noinline)) run_rom_code(volatile u8 *base)
{
    (void)base;
    u32 count = 0;
    while (test_active)
    {
        sdr_code_blob();
        count++;
    }
    return count;
}

typedef u32 (*RunFn)(volatile u8 *);
static const RunFn run_tab[SDR_STRESS_NUM_VARIANTS] = {
    run_nomem, run_rom_hot, run_rom_miss, run_rom_code,
};
static const u16 apc_tab[SDR_STRESS_NUM_VARIANTS] = { 16, 16, 16, 4096 };

// ---- sprite load ---------------------------------------------------------

static u16 lfsr_state;

static u16 rnd16(void)
{
    u16 l = lfsr_state;
    l = (u16)((l << 1) ^ ((l & 0x8000) ? 0x1021 : 0));
    if (l == 0) l = 0xACE1;
    lfsr_state = l;
    return l;
}

static void build_sprites(u16 count, u16 seed)
{
    lfsr_state = seed ? seed : 0xACE1;

    for (u16 i = 0; i < count; i++)
    {
        IGS023Sprite *spr = &SPRITE_BUFFER[i];
        memset(spr, 0, sizeof(IGS023Sprite));

        u32 addr = ((u32)rnd16() << 8) & 0x00FFFF00;  // anywhere in 16MB A-ROM
        u16 r = rnd16();

        spr->height = 64 + (r & 63);                  // 64-127 lines tall
        spr->width = 4 + ((r >> 6) & 3);              // 64-112 px wide
        spr->address_lo = (u16)(addr >> 1);
        spr->address_hi = (u16)(addr >> 17) & 0x7f;
        spr->xpos = (s16)((rnd16() % 480) - 16);
        spr->ypos = (s16)(rnd16() % 224);
        spr->color = r & 31;
        spr->prio = 0;
    }

    // end marker
    IGS023Sprite *end = &SPRITE_BUFFER[count];
    end->unk2 = 0;
    end->width = 0;
    end->height = 0;
}

// ---- result ----------------------------------------------------------------

typedef struct
{
    u8 variant, flags;
    u16 frames, sprite_count, accesses_per_chunk;
    u32 chunks, vbl_start, vbl_end;
} StressResult;

static StressResult last_result;
static bool have_result;

static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u16 get_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

static void pack_result(u8 *out, const StressResult *r)
{
    out[0] = r->variant;
    out[1] = r->flags;
    put_be16(out + 2, r->frames);
    put_be16(out + 4, r->sprite_count);
    put_be16(out + 6, r->accesses_per_chunk);
    put_be32(out + 8, r->chunks);
    put_be32(out + 12, r->vbl_start);
    put_be32(out + 16, r->vbl_end);
}

static u32 run_one(u8 variant, u16 frames, u16 sprite_count, u16 seed,
                   StressResult *r)
{
    r->variant = variant;
    r->flags = 0;
    r->frames = frames;
    r->sprite_count = sprite_count;
    r->accesses_per_chunk = apc_tab[variant];

    build_sprites(sprite_count, seed);
    IGS023_CTRL_OR(IGS023_CTRL_DMA);

    volatile u8 *base = (volatile u8 *)0x100;   // low vectors: plain ROM data

    r->vbl_start = igs023_get_vblank_count();
    start_test(frames);
    r->chunks = run_tab[variant](base);
    r->vbl_end = igs023_get_vblank_count();

    IGS023_CTRL_AND((u16)~IGS023_CTRL_DMA);
    build_sprites(0, 0);

    return r->chunks;
}

// ---- debug link servicing (ics_remote.c / vram_bench.c pattern) -----------

static void send_response(u8 seq, u8 status, const u8 *payload, u8 len)
{
    u8 tx[TX_BUF_SIZE];
    tx[0] = SDR_STRESS_RSP_MAGIC0;
    tx[1] = SDR_STRESS_RSP_MAGIC1;
    tx[2] = SDR_STRESS_VERSION;
    tx[3] = seq;
    tx[4] = status;
    tx[5] = len;
    for (u8 i = 0; i < len; i++) tx[6 + i] = payload[i];
    debug_link_write(tx, 6 + len);
    last_status_code = status;
    if (status == SDR_STRESS_STATUS_OK) command_count++;
    else error_count++;
}

static void handle_ping(u8 seq)
{
    u8 out[7];
    put_be16(out, SDR_STRESS_PING_MAGIC);
    out[2] = SDR_STRESS_VERSION;
    put_be32(out + 3, igs023_get_vblank_count());
    send_response(seq, SDR_STRESS_STATUS_OK, out, 7);
}

static void handle_run_test(u8 seq, const u8 *payload, u8 len)
{
    u8 out[SDR_STRESS_RESULT_SIZE];
    StressResult r;

    if (len != 8)
    {
        send_response(seq, SDR_STRESS_STATUS_BAD_LENGTH, NULL, 0);
        return;
    }

    u8 variant = payload[0];
    u16 frames = get_be16(payload + 2);
    u16 sprite_count = get_be16(payload + 4);
    u16 seed = get_be16(payload + 6);

    if (variant >= SDR_STRESS_NUM_VARIANTS || frames == 0 ||
        sprite_count > SDR_STRESS_MAX_SPRITES)
    {
        send_response(seq, SDR_STRESS_STATUS_BAD_PARAM, NULL, 0);
        return;
    }

    run_one(variant, frames, sprite_count, seed, &r);

    last_result = r;
    have_result = true;

    pack_result(out, &r);
    send_response(seq, SDR_STRESS_STATUS_OK, out, SDR_STRESS_RESULT_SIZE);
}

static void handle_get_result(u8 seq)
{
    u8 out[SDR_STRESS_RESULT_SIZE];
    if (!have_result)
    {
        send_response(seq, SDR_STRESS_STATUS_NO_RESULT, NULL, 0);
        return;
    }
    pack_result(out, &last_result);
    send_response(seq, SDR_STRESS_STATUS_OK, out, SDR_STRESS_RESULT_SIZE);
}

static void handle_request(void)
{
    u8 ver = rx_buf[2];
    u8 seq = rx_buf[3];
    u8 cmd = rx_buf[4];
    u8 len = rx_buf[5];
    last_cmd = cmd;

    if (ver != SDR_STRESS_VERSION)
    {
        send_response(seq, SDR_STRESS_STATUS_BAD_VERSION, NULL, 0);
        return;
    }

    switch (cmd)
    {
        case SDR_STRESS_CMD_PING:       handle_ping(seq); break;
        case SDR_STRESS_CMD_RUN_TEST:   handle_run_test(seq, rx_buf + 6, len); break;
        case SDR_STRESS_CMD_GET_RESULT: handle_get_result(seq); break;
        default: send_response(seq, SDR_STRESS_STATUS_BAD_CMD, NULL, 0); break;
    }
}

static void process_rx(void)
{
    while (rx_len >= 2)
    {
        if (rx_buf[0] != SDR_STRESS_REQ_MAGIC0 || rx_buf[1] != SDR_STRESS_REQ_MAGIC1)
        {
            // resync: drop one byte
            rx_len--;
            for (u16 i = 0; i < rx_len; i++) rx_buf[i] = rx_buf[i + 1];
            continue;
        }
        if (rx_len < 6) return;
        u8 len = rx_buf[5];
        if (rx_len < (u16)(6 + len)) return;

        handle_request();

        u16 total = (u16)(6 + len);
        rx_len -= total;
        for (u16 i = 0; i < rx_len; i++) rx_buf[i] = rx_buf[i + total];
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

// ---- auto-cycle for linkless (MiSTer) operation ----------------------------

static const u16 auto_counts[4] = { 0, 64, 128, 224 };
static u32 auto_chunks[4];
static u8 auto_idx;
static u8 auto_valid;
static u16 idle_frames;

#define AUTO_FRAMES 120

// ---- page ------------------------------------------------------------------

static void init(void)
{
    igs023_init();
    text_reset();
    set_default_palette();

    IGS023_CTRL_AND((u16)~(IGS023_CTRL_BUS_MASTER | IGS023_CTRL_DMA |
                           IGS023_CTRL_IRQ4_EN));
    IGS023_CTRL_OR(IGS023_CTRL_IRQ6_EN);

    build_sprites(0, 0);

    rx_len = 0;
    command_count = 0;
    error_count = 0;
    last_cmd = 0;
    last_status_code = 0;
    have_result = false;
    test_state = TS_DONE;
    test_active = 0;
    auto_idx = 0;
    auto_valid = 0;
    idle_frames = 0;
}

static void update(void)
{
    igs023_wait_vblank();
    poll_debug_link();

    bool link = debug_link_check_active();

    // idle threshold must stay well above the sim host's boot-ping window;
    // an auto test makes the page deaf to the debug link for its duration
    if (!link && ++idle_frames > 3600)
    {
        StressResult r;
        run_one(SDR_STRESS_VAR_ROM_CODE, AUTO_FRAMES,
                auto_counts[auto_idx], 0x1234, &r);
        auto_chunks[auto_idx] = r.chunks;
        if (auto_valid < 4) auto_valid++;
        auto_idx = (u8)((auto_idx + 1) & 3);
        idle_frames = 0;
    }

    text_color(1);
    text_cursor(2, 2);
    text("SDR STRESS\n");
    textf("LINK %s CMD %02X ST %02X OK %04X ER %04X\n",
          link ? "ACTIVE" : "AUTO", last_cmd, last_status_code,
          command_count, error_count);
    if (have_result)
    {
        textf("VAR %02X SPR %03X FRM %04X\n",
              last_result.variant, last_result.sprite_count, last_result.frames);
        textf("CHUNKS %08X\n", last_result.chunks);
    }
    if (auto_valid)
    {
        text("CODE X120F CHUNKS BY SPRITES\n");
        for (u8 i = 0; i < auto_valid && i < 4; i++)
            textf(" S%03X: %08X\n", auto_counts[i], auto_chunks[i]);
    }
    textf("VBL %05X\n", (u16)igs023_get_vblank_count());
}

PAGE_REGISTER_IRQ(sdr_stress, init, update, NULL, NULL, vblank_handler);
