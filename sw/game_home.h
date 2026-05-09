#ifndef GAME_HOME_H
#define GAME_HOME_H

#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "renderer.h"
#include "world.h"

#define GAME_HOME_NAME_MAX 64

typedef struct {
    char name[GAME_HOME_NAME_MAX];
    char path[WORLD_SAVE_PATH_MAX];
    uint32_t seed;
    int stone_tries_per_chunk;
} SelectedWorld;

bool run_home_menu(RenderContext *ctx, InputState *inp,
                   int target_fps, SelectedWorld *selection);

#endif
