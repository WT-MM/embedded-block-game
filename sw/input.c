#include "input.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

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

static int find_kbd(void)
{
    char path[64];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (test_evbit(fd, EV_KEY) && test_keybit(fd, KEY_W))
            return fd;
        close(fd);
    }
    return -1;
}

static int find_mouse(void)
{
    char path[64];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (test_evbit(fd, EV_REL) && test_relbit(fd, REL_X))
            return fd;
        close(fd);
    }
    return -1;
}

int input_init(InputState *inp)
{
    memset(inp, 0, sizeof(*inp));
    inp->_kbd_fd   = -1;
    inp->_mouse_fd = -1;

    inp->_kbd_fd = find_kbd();
    if (inp->_kbd_fd < 0)
        fprintf(stderr, "input: no keyboard found in /dev/input/event0-15\n");
    else
        fprintf(stderr, "input: keyboard ready\n");

    inp->_mouse_fd = find_mouse();
    if (inp->_mouse_fd < 0)
        fprintf(stderr, "input: no mouse found in /dev/input/event0-15\n");
    else
        fprintf(stderr, "input: mouse ready\n");

    return (inp->_kbd_fd >= 0) ? 0 : -1;
}

static void drain_fd(InputState *inp, int fd, bool is_mouse)
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
            case KEY_SPACE:                  inp->up      = down; break;
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT:             inp->down    = down; break;
            case KEY_ESC:   if (down) inp->quit = true;   break;
            case KEY_Q:     if (down) inp->quit = true;   break;
            default: break;
            }
        } else if (is_mouse && ev.type == EV_REL) {
            if (ev.code == REL_X) inp->mouse_dx += (float)ev.value;
            if (ev.code == REL_Y) inp->mouse_dy += (float)ev.value;
        }
    }
}

void input_update(InputState *inp)
{
    if (inp->_kbd_fd   >= 0) drain_fd(inp, inp->_kbd_fd,   false);
    if (inp->_mouse_fd >= 0) drain_fd(inp, inp->_mouse_fd, true);
}

void input_clear_mouse(InputState *inp)
{
    inp->mouse_dx = 0.0f;
    inp->mouse_dy = 0.0f;
}

void input_shutdown(InputState *inp)
{
    if (inp->_kbd_fd   >= 0) { close(inp->_kbd_fd);   inp->_kbd_fd   = -1; }
    if (inp->_mouse_fd >= 0) { close(inp->_mouse_fd); inp->_mouse_fd = -1; }
}
