#ifndef PAUSE_MENU_H
#define PAUSE_MENU_H

#include <stdbool.h>

#include "input.h"
#include "renderer.h"

typedef struct {
    bool open;
    int selected_setting;
    bool prev_up;
    bool prev_down;
    bool prev_left;
    bool prev_right;
    bool prev_select;
} PauseMenu;

typedef struct {
    int stream_chunks_per_frame;
    int stream_chunks_per_frame_max;
    int near_chunk_radius;
    int near_chunk_radius_max;
    int render_distance;
    int render_distance_max;
} PauseMenuSettings;

void pause_menu_init(PauseMenu *pm);
void pause_menu_toggle(PauseMenu *pm);
bool pause_menu_is_open(const PauseMenu *pm);
bool pause_menu_update(PauseMenu *pm, const InputState *inp,
                       PauseMenuSettings *settings,
                       bool *exit_requested);

void pause_menu_draw(const PauseMenu *pm, RenderContext *ctx,
                     const PauseMenuSettings *settings);

#endif
