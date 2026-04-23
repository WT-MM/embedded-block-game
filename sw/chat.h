#ifndef CHAT_H
#define CHAT_H

#include <stdbool.h>
#include <stdint.h>

#include "renderer.h"

#define CHAT_HISTORY_LINES 6
#define CHAT_LINE_MAX      48
#define CHAT_TEXT_SCALE    1 /* 5x7 glyph pixels, 1 = native 5x7 on screen */

typedef struct {
    bool open;
    char history[CHAT_HISTORY_LINES][CHAT_LINE_MAX + 1];
    int  history_len[CHAT_HISTORY_LINES];
    int  history_head;   /* slot to write next; oldest = (head + 1) % N */
    int  history_count;  /* min(written, CHAT_HISTORY_LINES) */
    char input[CHAT_LINE_MAX + 1];
    int  input_len;
} Chat;

void chat_init(Chat *chat);
void chat_toggle(Chat *chat);
bool chat_is_open(const Chat *chat);

/* Printf-style append to the scrollback; visible whether chat is open or not. */
void chat_log(Chat *chat, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Keyboard input for the input line (called only while chat is open). */
void chat_handle_char(Chat *chat, char ch);
void chat_handle_backspace(Chat *chat);
void chat_handle_enter(Chat *chat);

void chat_draw(const Chat *chat, RenderContext *ctx);

#endif
