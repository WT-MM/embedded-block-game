#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

#define INPUT_MAX_POINTERS 8

typedef enum {
    INPUT_POINTER_NONE = 0,
    INPUT_POINTER_REL,
    INPUT_POINTER_ABS,
} InputPointerMode;

typedef struct {
    int fd;
    InputPointerMode mode;
    int abs_x_min;
    int abs_x_max;
    int abs_y_min;
    int abs_y_max;
    int last_abs_x;
    int last_abs_y;
    bool have_abs_x;
    bool have_abs_y;
} InputPointer;

typedef struct {
    bool forward, back, left, right;
    bool up, down;
    bool jump_pressed;
    bool look_left, look_right, look_up, look_down;
    bool quit;

    float mouse_dx;
    float mouse_dy;

    int _kbd_fd;
    int _pointer_count;
    float _mouse_scale_x;
    float _mouse_scale_y;
    InputPointer _pointers[INPUT_MAX_POINTERS];
} InputState;

int  input_init(InputState *inp);
void input_update(InputState *inp);
void input_clear_mouse(InputState *inp);
bool input_consume_jump(InputState *inp);
void input_shutdown(InputState *inp);

#endif
