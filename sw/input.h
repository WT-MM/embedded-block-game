#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define INPUT_MAX_POINTERS 8
#define INPUT_TEXT_QUEUE_MAX 64

/* Text event codes for the chat input queue. Printable chars go in as-is;
 * backspace/enter use the control bytes below so the game loop can dispatch
 * without a second enum. */
#define INPUT_TEXT_BACKSPACE '\b'
#define INPUT_TEXT_ENTER     '\n'

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
    bool grabbed;
} InputPointer;

typedef struct {
    bool forward, back, left, right;
    bool up, down;
    bool sprint;
    bool jump_pressed;
    bool mode_toggle_pressed;
    bool chat_toggle_pressed;
    bool break_pressed;
    bool break_down;
    bool place_pressed;
    bool inventory_toggle_pressed;
    bool pause_toggle_pressed;
    bool debug_hud_toggle_pressed;
    bool menu_select_pressed;
    bool look_left, look_right, look_up, look_down;
    bool quit;
    int hotbar_slot_pressed;
    bool hotbar_page_pressed;

    float mouse_dx;
    float mouse_dy;
    float cursor_dx;
    float cursor_dy;

    /* Bytes captured while text mode is on. Printable ASCII or one of
     * INPUT_TEXT_BACKSPACE / INPUT_TEXT_ENTER. */
    char text_queue[INPUT_TEXT_QUEUE_MAX];
    int  text_queue_len;

    bool _text_mode;
    bool _shift_down;
    bool _grab_pointers;
    bool _pointer_capture_wanted;
    bool _forward_tap_armed;
    uint64_t _last_forward_press_ns;
    uint64_t _last_pointer_capture_attempt_ns;
    uint64_t _last_relative_motion_ns;

    int _kbd_fd;
    int _pointer_count;
    float _mouse_scale_x;
    float _mouse_scale_y;
    float _cursor_scale_x;
    float _cursor_scale_y;
    InputPointer _pointers[INPUT_MAX_POINTERS];
} InputState;

int  input_init(InputState *inp);
void input_update(InputState *inp);
void input_clear_mouse(InputState *inp);
bool input_consume_jump(InputState *inp);
bool input_consume_mode_toggle(InputState *inp);
bool input_consume_chat_toggle(InputState *inp);
bool input_consume_break(InputState *inp);
bool input_consume_place(InputState *inp);
bool input_consume_inventory_toggle(InputState *inp);
bool input_consume_pause_toggle(InputState *inp);
bool input_consume_debug_hud_toggle(InputState *inp);
bool input_consume_menu_select(InputState *inp);
int input_consume_hotbar_slot(InputState *inp);
bool input_consume_hotbar_page(InputState *inp);
void input_set_pointer_capture(InputState *inp, bool on);
void input_set_text_mode(InputState *inp, bool on);
void input_clear_text_queue(InputState *inp);
void input_shutdown(InputState *inp);

#endif
