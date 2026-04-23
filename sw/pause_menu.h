#ifndef PAUSE_MENU_H
#define PAUSE_MENU_H

#include <stdbool.h>

#include "renderer.h"

typedef struct {
    bool open;
} PauseMenu;

void pause_menu_init(PauseMenu *pm);
void pause_menu_toggle(PauseMenu *pm);
bool pause_menu_is_open(const PauseMenu *pm);

/* Semi-transparent fullscreen dim + centered text listing the pause entries.
 * Game logic keeps running while the menu is open — this is purely visual. */
void pause_menu_draw(const PauseMenu *pm, RenderContext *ctx);

#endif
