#include "pause_menu.h"

#include <string.h>

#include "chat.h"
#include "voxel_gpu.h"

#define PAUSE_DIM_PALETTE   14   /* medium grey (0x5c5c5c) */
#define PAUSE_TEXT_PALETTE  5    /* white */

static const char *PAUSE_LINES[] = {
    "GAME MENU",
    "",
    "ESC   RESUME GAME",
    "Q     QUIT TO DESKTOP",
    "",
    "MULTIPLAYER - COMING SOON",
    "CONNECT / DISCONNECT - COMING SOON",
};

#define PAUSE_LINE_COUNT ((int)(sizeof(PAUSE_LINES) / sizeof(PAUSE_LINES[0])))

void pause_menu_init(PauseMenu *pm)
{
    pm->open = false;
}

void pause_menu_toggle(PauseMenu *pm)
{
    pm->open = !pm->open;
}

bool pause_menu_is_open(const PauseMenu *pm)
{
    return pm->open;
}

void pause_menu_draw(const PauseMenu *pm, RenderContext *ctx)
{
    if (!pm || !pm->open || !ctx) return;

    /* Full-screen dim. Stack two 50% alpha rects to deepen the darken
     * without needing a 25%-or-less blend level the hardware lacks at
     * this palette. */
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       PAUSE_DIM_PALETTE, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       PAUSE_DIM_PALETTE, QUAD_ALPHA_50);

    int cell_w = chat_font_cell_w();
    int cell_h = chat_font_cell_h();
    int line_step = cell_h + 2;  /* a little breathing room between lines */

    int block_h = PAUSE_LINE_COUNT * line_step;
    float y = (SCREEN_HEIGHT - (float)block_h) * 0.5f;

    for (int i = 0; i < PAUSE_LINE_COUNT; i++) {
        const char *s = PAUSE_LINES[i];
        int len = (int)strlen(s);
        float text_w = (float)(len * cell_w);
        float x = (SCREEN_WIDTH - text_w) * 0.5f;
        if (x < 0.0f) x = 0.0f;
        chat_draw_text(ctx, s, len, x, y, PAUSE_TEXT_PALETTE);
        y += (float)line_step;
    }
}
