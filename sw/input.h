#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

typedef struct {
    bool forward, back, left, right;
    bool up, down;
    bool look_left, look_right, look_up, look_down;
    bool quit;

    float mouse_dx;
    float mouse_dy;

    int _kbd_fd;
    int _mouse_fd;
} InputState;

int  input_init(InputState *inp);
void input_update(InputState *inp);
void input_clear_mouse(InputState *inp);
void input_shutdown(InputState *inp);

#endif
