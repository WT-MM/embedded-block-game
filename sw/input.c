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

int input_init(InputState *inp)
{
    memset(inp, 0, sizeof(*inp));
    inp->_kbd_fd   = -1;
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
            switch (ev.code) {
            case KEY_W:                      inp->forward = down; break;
            case KEY_S:                      inp->back    = down; break;
            case KEY_A:                      inp->left    = down; break;
            case KEY_D:                      inp->right   = down; break;
            case KEY_SPACE:
                if (ev.value == 1)
                    inp->jump_pressed = true;
                inp->up = down;
                break;
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT:             inp->down    = down; break;
            case KEY_LEFT:                       inp->look_left  = down; break;
            case KEY_RIGHT:                      inp->look_right = down; break;
            case KEY_UP:                         inp->look_up    = down; break;
            case KEY_DOWN:                       inp->look_down  = down; break;
            case KEY_ESC:   if (down) inp->quit = true;   break;
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
