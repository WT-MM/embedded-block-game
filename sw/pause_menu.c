#include "pause_menu.h"

#include <stdio.h>
#include <string.h>

#include "chat.h"
#include "voxel_gpu.h"

#define PAUSE_DIM_PALETTE   14   /* medium grey (0x5c5c5c) */
#define PAUSE_TEXT_PALETTE  5    /* white */
#define PAUSE_SETTING_COUNT 5
#define PAUSE_OPTION_COUNT  7
#define PAUSE_EXIT_TO_MENU_INDEX 5
#define PAUSE_EXIT_GAME_INDEX    6
#define PAUSE_MAX_LINES 14
#define PAUSE_LINE_CHARS 72

void pause_menu_init(PauseMenu *pm)
{
    memset(pm, 0, sizeof(*pm));
}

void pause_menu_toggle(PauseMenu *pm)
{
    pm->open = !pm->open;
    pm->prev_up = false;
    pm->prev_down = false;
    pm->prev_left = false;
    pm->prev_right = false;
    pm->prev_select = false;
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

static bool adjust_int_setting(int *value, int min_value, int max_value,
                               int delta)
{
    int next;

    next = *value + delta;
    if (next < min_value)
        next = min_value;
    if (next > max_value)
        next = max_value;
    if (next == *value)
        return false;

    *value = next;
    return true;
}

bool pause_menu_update(PauseMenu *pm, const InputState *inp,
                       PauseMenuSettings *settings,
                       PauseMenuAction *action)
{
    bool up;
    bool down;
    bool left;
    bool right;
    bool changed = false;

    if (!pm->open) {
        pm->prev_up = false;
        pm->prev_down = false;
        pm->prev_left = false;
        pm->prev_right = false;
        pm->prev_select = false;
        return false;
    }

    *action = PAUSE_MENU_ACTION_NONE;

    up = inp->look_up || inp->forward;
    down = inp->look_down || inp->back;
    left = inp->look_left || inp->left;
    right = inp->look_right || inp->right;

    if (edge_pressed(up, &pm->prev_up)) {
        pm->selected_setting--;
        if (pm->selected_setting < 0)
            pm->selected_setting = PAUSE_OPTION_COUNT - 1;
    }
    if (edge_pressed(down, &pm->prev_down)) {
        pm->selected_setting++;
        if (pm->selected_setting >= PAUSE_OPTION_COUNT)
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
        } else if (pm->selected_setting == 3) {
            changed |= adjust_int_setting(
                &settings->mouse_sensitivity_percent,
                settings->mouse_sensitivity_percent_min,
                settings->mouse_sensitivity_percent_max,
                -settings->mouse_sensitivity_percent_step);
        } else if (pm->selected_setting == 4) {
            changed |= adjust_int_setting(
                &settings->fov_degrees_x10,
                settings->fov_degrees_x10_min,
                settings->fov_degrees_x10_max,
                -settings->fov_degrees_x10_step);
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
        } else if (pm->selected_setting == 3) {
            changed |= adjust_int_setting(
                &settings->mouse_sensitivity_percent,
                settings->mouse_sensitivity_percent_min,
                settings->mouse_sensitivity_percent_max,
                settings->mouse_sensitivity_percent_step);
        } else if (pm->selected_setting == 4) {
            changed |= adjust_int_setting(
                &settings->fov_degrees_x10,
                settings->fov_degrees_x10_min,
                settings->fov_degrees_x10_max,
                settings->fov_degrees_x10_step);
        }
    }
    if (edge_pressed(inp->menu_select_pressed, &pm->prev_select)) {
        if (pm->selected_setting == PAUSE_EXIT_TO_MENU_INDEX) {
            *action = PAUSE_MENU_ACTION_EXIT_TO_MENU;
        } else if (pm->selected_setting == PAUSE_EXIT_GAME_INDEX) {
            *action = PAUSE_MENU_ACTION_EXIT_GAME;
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

void pause_menu_draw(const PauseMenu *pm, RenderContext *ctx,
                     const PauseMenuSettings *settings)
{
    if (!pm->open)
        return;

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

    format_stream_value(stream_value, sizeof(stream_value),
                        settings->stream_chunks_per_frame);

    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "GAME MENU");
    lines[line_count++][0] = '\0';
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c STREAM CHUNKS/FRAME  %s",
             pm->selected_setting == 0 ? '>' : ' ', stream_value);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c NEAR MESH RADIUS     %d",
             pm->selected_setting == 1 ? '>' : ' ',
             settings->near_chunk_radius);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c RENDER DISTANCE       %d",
             pm->selected_setting == 2 ? '>' : ' ',
             settings->render_distance);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c MOUSE SENSITIVITY    %d%%",
             pm->selected_setting == 3 ? '>' : ' ',
             settings->mouse_sensitivity_percent);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c FOV                  %d.%d",
             pm->selected_setting == 4 ? '>' : ' ',
             settings->fov_degrees_x10 / 10,
             settings->fov_degrees_x10 % 10);
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c EXIT TO MENU",
             pm->selected_setting == PAUSE_EXIT_TO_MENU_INDEX ? '>' : ' ');
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "%c EXIT GAME",
             pm->selected_setting == PAUSE_EXIT_GAME_INDEX ? '>' : ' ');
    lines[line_count++][0] = '\0';
    snprintf(lines[line_count++], PAUSE_LINE_CHARS,
             "W/S SELECT   A/D ADJUST   ENTER USE");
    snprintf(lines[line_count++], PAUSE_LINE_CHARS, "ESC RESUME");
    lines[line_count++][0] = '\0';
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
