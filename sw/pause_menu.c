#include "pause_menu.h"

#include <stdio.h>
#include <string.h>

#include "chat.h"
#include "voxel_gpu.h"

#define PAUSE_DIM_PALETTE   14   /* medium grey (0x5c5c5c) */
#define PAUSE_TEXT_PALETTE  5    /* white */
#define PAUSE_SETTING_COUNT 3
#define PAUSE_MAX_LINES 12
#define PAUSE_LINE_CHARS 72

void pause_menu_init(PauseMenu *pm)
{
    memset(pm, 0, sizeof(*pm));
    pm->open = false;
}

void pause_menu_toggle(PauseMenu *pm)
{
    pm->open = !pm->open;
    pm->prev_up = false;
    pm->prev_down = false;
    pm->prev_left = false;
    pm->prev_right = false;
}

bool pause_menu_is_open(const PauseMenu *pm)
{
    return pm->open;
}

static bool edge_pressed(bool now, bool *prev)
{
    bool pressed = now && !*prev;
    *prev = now;
    return pressed;
}

bool pause_menu_update(PauseMenu *pm, const InputState *inp,
                       PauseMenuSettings *settings)
{
    bool up;
    bool down;
    bool left;
    bool right;
    bool changed = false;

    if (!pm || !inp || !settings)
        return false;
    if (!pm->open) {
        pm->prev_up = false;
        pm->prev_down = false;
        pm->prev_left = false;
        pm->prev_right = false;
        return false;
    }

    up = inp->look_up || inp->forward;
    down = inp->look_down || inp->back;
    left = inp->look_left || inp->left;
    right = inp->look_right || inp->right;

    if (edge_pressed(up, &pm->prev_up)) {
        pm->selected_setting--;
        if (pm->selected_setting < 0)
            pm->selected_setting = PAUSE_SETTING_COUNT - 1;
    }
    if (edge_pressed(down, &pm->prev_down)) {
        pm->selected_setting++;
        if (pm->selected_setting >= PAUSE_SETTING_COUNT)
            pm->selected_setting = 0;
    }

    if (edge_pressed(left, &pm->prev_left)) {
        if (pm->selected_setting == 0) {
            if (settings->stream_chunks_per_frame > 0) {
                settings->stream_chunks_per_frame--;
                changed = true;
            }
        } else if (pm->selected_setting == 1) {
            if (settings->near_chunk_radius > 0) {
                settings->near_chunk_radius--;
                changed = true;
            }
        } else if (pm->selected_setting == 2) {
            if (settings->render_distance > 1) {
                settings->render_distance--;
                changed = true;
            }
        }
    }
    if (edge_pressed(right, &pm->prev_right)) {
        if (pm->selected_setting == 0) {
            if (settings->stream_chunks_per_frame <
                settings->stream_chunks_per_frame_max) {
                settings->stream_chunks_per_frame++;
                changed = true;
            }
        } else if (pm->selected_setting == 1) {
            if (settings->near_chunk_radius <
                settings->near_chunk_radius_max) {
                settings->near_chunk_radius++;
                changed = true;
            }
        } else if (pm->selected_setting == 2) {
            if (settings->render_distance <
                settings->render_distance_max) {
                settings->render_distance++;
                changed = true;
            }
        }
    }

    return changed;
}

static void format_stream_value(char *buf, size_t buf_size, int value)
{
    if (value <= 0)
        snprintf(buf, buf_size, "UNLIMITED");
    else
        snprintf(buf, buf_size, "%d", value);
}

static void set_blank_line(char *line)
{
    if (line)
        line[0] = '\0';
}

void pause_menu_draw(const PauseMenu *pm, RenderContext *ctx,
                     const PauseMenuSettings *settings)
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
    char lines[PAUSE_MAX_LINES][PAUSE_LINE_CHARS];
    char stream_value[16];
    int line_count = 0;

    if (settings)
        format_stream_value(stream_value, sizeof(stream_value),
                            settings->stream_chunks_per_frame);
    else
        snprintf(stream_value, sizeof(stream_value), "N/A");

    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "GAME MENU");
    set_blank_line(lines[line_count++]);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c STREAM CHUNKS/FRAME  %s",
             pm->selected_setting == 0 ? '>' : ' ', stream_value);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c NEAR MESH RADIUS     %d",
             pm->selected_setting == 1 ? '>' : ' ',
             settings ? settings->near_chunk_radius : 0);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c RENDER DISTANCE       %d",
             pm->selected_setting == 2 ? '>' : ' ',
             settings ? settings->render_distance : 0);
    set_blank_line(lines[line_count++]);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "W/S SELECT   A/D ADJUST");
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "ESC RESUME   Q QUIT");
    set_blank_line(lines[line_count++]);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "PREGENERATED WORLDS - PLANNED");

    int block_h = line_count * line_step;
    float y = (SCREEN_HEIGHT - (float)block_h) * 0.5f;

    for (int i = 0; i < line_count; i++) {
        const char *s = lines[i];
        int len = (int)strlen(s);
        float text_w = (float)(len * cell_w);
        float x = (SCREEN_WIDTH - text_w) * 0.5f;
        if (x < 0.0f) x = 0.0f;
        chat_draw_text(ctx, s, len, x, y, PAUSE_TEXT_PALETTE);
        y += (float)line_step;
    }
}
