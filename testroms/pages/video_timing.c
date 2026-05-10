#include "../system.h"
#include "../memory_map.h"
#include "../page.h"

#include "../util.h"
#include "../tilemap.h"
#include "../igs023.h"
#include "../color.h"
#include "../gui.h"

static u32 frame_count;

static void init()
{
    igs023_init();
    text_reset();
    set_default_palette();

    frame_count = 0;
}

static void update()
{
    igs023_wait_vblank();

    text_color(2);
    text_cursor(3, 2);
    textf("COUNT: %08X\n", frame_count);

    frame_count++;

    gui_begin(3, 4);
    if (gui_button("RESET COUNT"))
    {
        frame_count = 0;
    }

    gui_end();
}

PAGE_REGISTER(video_timing, init, update, NULL);

