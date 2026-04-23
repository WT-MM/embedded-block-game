#include "input.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define INPUT_SCAN_LIMIT 32
#define ABS_MOUSE_SPAN_X 640.0f
#define ABS_MOUSE_SPAN_Y 480.0f
#define BITS_PER_LONG   (sizeof(long) * 8)
#define BIT_WORD(nr)    ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr)    (1UL << ((nr) % BITS_PER_LONG))

static bool test_evbit(int fd, int type)
{
    unsigned long bits[(EV_MAX / BITS_PER_LONG) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) < 0)
        return false;
    return !!(bits[BIT_WORD(type)] & BIT_MASK(type));
}

static bool test_keybit(int fd, int key)
{
    unsigned long bits[(KEY_MAX / BITS_PER_LONG) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return false;
    return !!(bits[BIT_WORD(key)] & BIT_MASK(key));
}

static bool test_relbit(int fd, int rel)
{
    unsigned long bits[(REL_MAX / BITS_PER_LONG) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(bits)), bits) < 0)
        return false;
    return !!(bits[BIT_WORD(rel)] & BIT_MASK(rel));
}

static bool test_absbit(int fd, int abs)
{
    unsigned long bits[(ABS_MAX / BITS_PER_LONG) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
        return false;
    return !!(bits[BIT_WORD(abs)] & BIT_MASK(abs));
}

static void describe_device(int fd, char *name, size_t name_size)
{
    if (name_size == 0)
        return;

    if (ioctl(fd, EVIOCGNAME((int)name_size), name) < 0 || name[0] == '\0')
        snprintf(name, name_size, "unknown");
}

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    if (!value || value[0] == '\0')
        return false;
    if (strcmp(value, "0") == 0)
        return false;
    if (strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0)
        return false;
    if (strcmp(value, "no") == 0 || strcmp(value, "NO") == 0)
        return false;

    return true;
}

static int find_kbd(void)
{
    char path[64];
    for (int i = 0; i < INPUT_SCAN_LIMIT; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (test_evbit(fd, EV_KEY) && test_keybit(fd, KEY_W))
            return fd;
        close(fd);
    }
    return -1;
}

static bool get_abs_range(int fd, unsigned int axis, int *min_out, int *max_out)
{
    struct input_absinfo info;

    if (ioctl(fd, EVIOCGABS(axis), &info) < 0)
        return false;

    *min_out = info.minimum;
    *max_out = info.maximum;
    return true;
}

static bool is_pointer_device(int fd)
{
    return test_evbit(fd, EV_KEY) &&
           (test_keybit(fd, BTN_MOUSE) ||
            test_keybit(fd, BTN_LEFT) ||
            test_keybit(fd, BTN_TOUCH));
}

static bool is_relative_pointer(int fd)
{
    if (!is_pointer_device(fd))
        return false;

    return test_evbit(fd, EV_REL) &&
           test_relbit(fd, REL_X) &&
           test_relbit(fd, REL_Y);
}

static bool is_absolute_pointer(int fd)
{
    if (!is_pointer_device(fd))
        return false;

    return test_evbit(fd, EV_ABS) &&
           test_absbit(fd, ABS_X) &&
           test_absbit(fd, ABS_Y);
}

static void add_pointer(InputState *inp, int fd, InputPointerMode mode)
{
    if (inp->_pointer_count >= INPUT_MAX_POINTERS) {
        close(fd);
        return;
    }

    InputPointer *pointer = &inp->_pointers[inp->_pointer_count++];
    char name[128] = {0};

    memset(pointer, 0, sizeof(*pointer));
    pointer->fd = fd;
    pointer->mode = mode;

    if (mode == INPUT_POINTER_ABS) {
        get_abs_range(fd, ABS_X, &pointer->abs_x_min, &pointer->abs_x_max);
        get_abs_range(fd, ABS_Y, &pointer->abs_y_min, &pointer->abs_y_max);
    }

    describe_device(fd, name, sizeof(name));
    fprintf(stderr, "input: pointer ready (%s, %s)\n",
            mode == INPUT_POINTER_REL ? "relative" : "absolute",
            name);
}

static void find_pointers(InputState *inp)
{
    char path[64];

    for (int i = 0; i < INPUT_SCAN_LIMIT; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (is_relative_pointer(fd)) {
            add_pointer(inp, fd, INPUT_POINTER_REL);
            continue;
        }
        if (is_absolute_pointer(fd)) {
            add_pointer(inp, fd, INPUT_POINTER_ABS);
            continue;
        }

        close(fd);
    }
}

static float scale_abs_delta(int delta, int min, int max, float span)
{
    int range = max - min;

    if (range <= 0)
        return (float)delta;

    return ((float)delta * span) / (float)range;
}

static const char SCANCODE_TO_CHAR[KEY_MAX];  /* forward decl; defined below */
static const char SCANCODE_TO_CHAR_SHIFT[KEY_MAX];

static char scancode_to_ascii(int code, bool shift)
{
    if (code < 0 || code >= KEY_MAX) return 0;
    char c = shift ? SCANCODE_TO_CHAR_SHIFT[code] : SCANCODE_TO_CHAR[code];
    return c;
}

static void text_queue_push(InputState *inp, char ch)
{
    if (inp->text_queue_len >= INPUT_TEXT_QUEUE_MAX) return;
    inp->text_queue[inp->text_queue_len++] = ch;
}

static int hotbar_slot_for_key(int code)
{
    switch (code) {
    case KEY_1:
    case KEY_KP1:
        return 0;
    case KEY_2:
    case KEY_KP2:
        return 1;
    case KEY_3:
    case KEY_KP3:
        return 2;
    case KEY_4:
    case KEY_KP4:
        return 3;
    case KEY_5:
    case KEY_KP5:
        return 4;
    default:
        return -1;
    }
}

int input_init(InputState *inp)
{
    memset(inp, 0, sizeof(*inp));
    inp->_kbd_fd   = -1;
    inp->hotbar_slot_pressed = -1;
    inp->_mouse_scale_x = env_flag_enabled("VOXEL_MOUSE_INVERT_X") ? -1.0f : 1.0f;
    inp->_mouse_scale_y = env_flag_enabled("VOXEL_MOUSE_INVERT_Y") ? -1.0f : 1.0f;
    for (int i = 0; i < INPUT_MAX_POINTERS; i++)
        inp->_pointers[i].fd = -1;

    inp->_kbd_fd = find_kbd();
    if (inp->_kbd_fd < 0)
        fprintf(stderr, "input: no keyboard found in /dev/input/event0-%d\n",
                INPUT_SCAN_LIMIT - 1);
    else
        fprintf(stderr, "input: keyboard ready\n");

    find_pointers(inp);
    if (inp->_pointer_count == 0)
        fprintf(stderr, "input: no pointer found in /dev/input/event0-%d\n",
                INPUT_SCAN_LIMIT - 1);
    fprintf(stderr, "input: mouse invert x=%s y=%s\n",
            inp->_mouse_scale_x < 0.0f ? "on" : "off",
            inp->_mouse_scale_y < 0.0f ? "on" : "off");

    return (inp->_kbd_fd >= 0) ? 0 : -1;
}

static void drain_fd(InputState *inp, int fd, InputPointer *pointer)
{
    struct input_event ev;

    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            bool down = (ev.value != 0); /* 0=up, 1=down, 2=repeat */
            bool press_edge  = (ev.value == 1);
            bool press_or_repeat = (ev.value == 1 || ev.value == 2);

            /* Shift tracking happens regardless of text mode so both game
             * actions and text entry see the correct modifier state. */
            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
                inp->_shift_down = down;

            /* Look keys are always live. T opens the chat only when it
             * is closed — once open, T is a regular letter and ESC or
             * Enter closes the chat. */
            switch (ev.code) {
            case KEY_T:
                if (press_edge && !inp->_text_mode)
                    inp->chat_toggle_pressed = true;
                break;
            case KEY_LEFT:   inp->look_left  = down; break;
            case KEY_RIGHT:  inp->look_right = down; break;
            case KEY_UP:     inp->look_up    = down; break;
            case KEY_DOWN:   inp->look_down  = down; break;
            default: break;
            }

            if (inp->_text_mode && ev.code == KEY_ESC && press_edge) {
                /* Treat ESC as "close chat" — signal the toggle. */
                inp->chat_toggle_pressed = true;
                continue;
            }

            if (inp->_text_mode) {
                /* Suppress movement/game actions; capture printable text
                 * and editing keys into the queue. */
                if (ev.code == KEY_BACKSPACE && press_or_repeat) {
                    text_queue_push(inp, INPUT_TEXT_BACKSPACE);
                } else if (ev.code == KEY_ENTER && press_edge) {
                    text_queue_push(inp, INPUT_TEXT_ENTER);
                } else if (press_or_repeat) {
                    char ch = scancode_to_ascii(ev.code, inp->_shift_down);
                    if (ch)
                        text_queue_push(inp, ch);
                }
                continue;
            }

            {
                int hotbar_slot = hotbar_slot_for_key(ev.code);

                if (press_edge && hotbar_slot >= 0)
                    inp->hotbar_slot_pressed = hotbar_slot;
            }

            switch (ev.code) {
            case KEY_W:                      inp->forward = down; break;
            case KEY_S:                      inp->back    = down; break;
            case KEY_A:                      inp->left    = down; break;
            case KEY_D:                      inp->right   = down; break;
            case KEY_SPACE:
                if (press_edge)
                    inp->jump_pressed = true;
                inp->up = down;
                break;
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT:             inp->down    = down; break;
            case KEY_G:
                if (press_edge)
                    inp->mode_toggle_pressed = true;
                break;
            case KEY_F:
            case BTN_LEFT:
                if (press_edge)
                    inp->break_pressed = true;
                break;
            case KEY_R:
            case BTN_RIGHT:
                if (press_edge)
                    inp->place_pressed = true;
                break;
            case KEY_LEFTCTRL:
            case KEY_RIGHTCTRL:
                inp->sprint = down;
                break;
            case KEY_ESC:
                if (press_edge)
                    inp->pause_toggle_pressed = true;
                break;
            case KEY_Q:     if (down) inp->quit = true;   break;
            default: break;
            }
        } else if (pointer && pointer->mode == INPUT_POINTER_REL && ev.type == EV_REL) {
            if (ev.code == REL_X)
                inp->mouse_dx += (float)ev.value * inp->_mouse_scale_x;
            if (ev.code == REL_Y)
                inp->mouse_dy += (float)ev.value * inp->_mouse_scale_y;
        } else if (pointer && pointer->mode == INPUT_POINTER_ABS && ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                if (pointer->have_abs_x) {
                    inp->mouse_dx += scale_abs_delta(ev.value - pointer->last_abs_x,
                                                     pointer->abs_x_min,
                                                     pointer->abs_x_max,
                                                     ABS_MOUSE_SPAN_X) *
                                     inp->_mouse_scale_x;
                }
                pointer->last_abs_x = ev.value;
                pointer->have_abs_x = true;
            }
            if (ev.code == ABS_Y) {
                if (pointer->have_abs_y) {
                    inp->mouse_dy += scale_abs_delta(ev.value - pointer->last_abs_y,
                                                     pointer->abs_y_min,
                                                     pointer->abs_y_max,
                                                     ABS_MOUSE_SPAN_Y) *
                                     inp->_mouse_scale_y;
                }
                pointer->last_abs_y = ev.value;
                pointer->have_abs_y = true;
            }
        }
    }
}

void input_update(InputState *inp)
{
    if (inp->_kbd_fd >= 0)
        drain_fd(inp, inp->_kbd_fd, NULL);

    for (int i = 0; i < inp->_pointer_count; i++) {
        if (inp->_pointers[i].fd >= 0)
            drain_fd(inp, inp->_pointers[i].fd, &inp->_pointers[i]);
    }
}

void input_clear_mouse(InputState *inp)
{
    inp->mouse_dx = 0.0f;
    inp->mouse_dy = 0.0f;
}

bool input_consume_jump(InputState *inp)
{
    bool pressed = inp->jump_pressed;
    inp->jump_pressed = false;
    return pressed;
}

bool input_consume_mode_toggle(InputState *inp)
{
    bool pressed = inp->mode_toggle_pressed;
    inp->mode_toggle_pressed = false;
    return pressed;
}

bool input_consume_chat_toggle(InputState *inp)
{
    bool pressed = inp->chat_toggle_pressed;
    inp->chat_toggle_pressed = false;
    return pressed;
}

bool input_consume_break(InputState *inp)
{
    bool pressed = inp->break_pressed;
    inp->break_pressed = false;
    return pressed;
}

bool input_consume_place(InputState *inp)
{
    bool pressed = inp->place_pressed;
    inp->place_pressed = false;
    return pressed;
}

bool input_consume_pause_toggle(InputState *inp)
{
    bool pressed = inp->pause_toggle_pressed;
    inp->pause_toggle_pressed = false;
    return pressed;
}

int input_consume_hotbar_slot(InputState *inp)
{
    int slot = inp->hotbar_slot_pressed;
    inp->hotbar_slot_pressed = -1;
    return slot;
}

void input_set_text_mode(InputState *inp, bool on)
{
    if (on && !inp->_text_mode) {
        /* Freeze all movement inputs so a held WASD doesn't keep walking
         * while the user is typing. */
        inp->forward = inp->back = inp->left = inp->right = false;
        inp->up = inp->down = false;
        inp->jump_pressed = false;
        inp->mode_toggle_pressed = false;
        inp->break_pressed = false;
        inp->place_pressed = false;
        inp->hotbar_slot_pressed = -1;
    }
    inp->_text_mode = on;
    if (!on)
        inp->text_queue_len = 0;
}

void input_clear_text_queue(InputState *inp)
{
    inp->text_queue_len = 0;
}

void input_shutdown(InputState *inp)
{
    if (inp->_kbd_fd >= 0) {
        close(inp->_kbd_fd);
        inp->_kbd_fd = -1;
    }

    for (int i = 0; i < inp->_pointer_count; i++) {
        if (inp->_pointers[i].fd >= 0) {
            close(inp->_pointers[i].fd);
            inp->_pointers[i].fd = -1;
        }
    }
    inp->_pointer_count = 0;
}

/* US-layout scancode → ASCII. Entries left zero are ignored. The tables are
 * KEY_MAX-sized so the lookup is a simple bounded index — any unmapped key
 * just yields 0. */
static const char SCANCODE_TO_CHAR[KEY_MAX] = {
    [KEY_A] = 'a', [KEY_B] = 'b', [KEY_C] = 'c', [KEY_D] = 'd',
    [KEY_E] = 'e', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
    [KEY_I] = 'i', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
    [KEY_M] = 'm', [KEY_N] = 'n', [KEY_O] = 'o', [KEY_P] = 'p',
    [KEY_Q] = 'q', [KEY_R] = 'r', [KEY_S] = 's', [KEY_T] = 't',
    [KEY_U] = 'u', [KEY_V] = 'v', [KEY_W] = 'w', [KEY_X] = 'x',
    [KEY_Y] = 'y', [KEY_Z] = 'z',
    [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
    [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
    [KEY_9] = '9', [KEY_0] = '0',
    [KEY_SPACE]      = ' ',
    [KEY_MINUS]      = '-',
    [KEY_EQUAL]      = '=',
    [KEY_LEFTBRACE]  = '[',
    [KEY_RIGHTBRACE] = ']',
    [KEY_SEMICOLON]  = ';',
    [KEY_APOSTROPHE] = '\'',
    [KEY_COMMA]      = ',',
    [KEY_DOT]        = '.',
    [KEY_SLASH]      = '/',
    [KEY_BACKSLASH]  = '\\',
    [KEY_GRAVE]      = '`',
};

static const char SCANCODE_TO_CHAR_SHIFT[KEY_MAX] = {
    [KEY_A] = 'A', [KEY_B] = 'B', [KEY_C] = 'C', [KEY_D] = 'D',
    [KEY_E] = 'E', [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H',
    [KEY_I] = 'I', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
    [KEY_M] = 'M', [KEY_N] = 'N', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_Q] = 'Q', [KEY_R] = 'R', [KEY_S] = 'S', [KEY_T] = 'T',
    [KEY_U] = 'U', [KEY_V] = 'V', [KEY_W] = 'W', [KEY_X] = 'X',
    [KEY_Y] = 'Y', [KEY_Z] = 'Z',
    [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$',
    [KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*',
    [KEY_9] = '(', [KEY_0] = ')',
    [KEY_SPACE]      = ' ',
    [KEY_MINUS]      = '_',
    [KEY_EQUAL]      = '+',
    [KEY_LEFTBRACE]  = '{',
    [KEY_RIGHTBRACE] = '}',
    [KEY_SEMICOLON]  = ':',
    [KEY_APOSTROPHE] = '"',
    [KEY_COMMA]      = '<',
    [KEY_DOT]        = '>',
    [KEY_SLASH]      = '?',
    [KEY_BACKSLASH]  = '|',
    [KEY_GRAVE]      = '~',
};
