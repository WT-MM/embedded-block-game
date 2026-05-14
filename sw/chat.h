#ifndef CHAT_H
#define CHAT_H

#include <stdbool.h>
#include <stdint.h>

#include "renderer.h"

#define CHAT_HISTORY_LINES 12
#define CHAT_INPUT_HISTORY_LINES 16
#define CHAT_LINE_MAX      48
#define CHAT_TEXT_SCALE    1 /* 5x7 glyph pixels, 1 = native 5x7 on screen */

typedef struct {
    bool open;
    char history[CHAT_HISTORY_LINES][CHAT_LINE_MAX + 1];
    int  history_len[CHAT_HISTORY_LINES];
    int  history_head;   /* slot to write next; oldest = (head + 1) % N */
    int  history_count;  /* min(written, CHAT_HISTORY_LINES) */
    char input_history[CHAT_INPUT_HISTORY_LINES][CHAT_LINE_MAX + 1];
    int  input_history_head;
    int  input_history_count;
    int  input_history_cursor;
    char input_history_draft[CHAT_LINE_MAX + 1];
    char input[CHAT_LINE_MAX + 1];
    int  input_len;
    bool completion_active;
    char completion_base[CHAT_LINE_MAX + 1];
    int  completion_index;
    /* Seconds since the last chat_log; drives the closed-chat fade-out. */
    float age_since_activity;
} Chat;

void chat_init(Chat *chat);
void chat_toggle(Chat *chat);
bool chat_is_open(const Chat *chat);

/* Advance fade timer. Call once per frame with the real-time dt. */
void chat_tick(Chat *chat, float dt);

/* Printf-style append to the scrollback; visible whether chat is open or not. */
void chat_log(Chat *chat, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Keyboard input for the input line (called only while chat is open). */
void chat_handle_char(Chat *chat, char ch);
void chat_handle_backspace(Chat *chat);
void chat_handle_enter(Chat *chat);
void chat_handle_history_prev(Chat *chat);
void chat_handle_history_next(Chat *chat);
void chat_set_input(Chat *chat, const char *line);
void chat_reset_completion(Chat *chat);

void chat_draw(const Chat *chat, RenderContext *ctx);

/* Cell dimensions of the chat's bitmap font at CHAT_TEXT_SCALE, exposed so
 * other overlays (pause menu, HUD) can lay out shared-font text. */
int  chat_font_cell_w(void);
int  chat_font_cell_h(void);

/* Draw a string at (x, y) in screen pixels using the chat bitmap font.
 * Clips at SCREEN_WIDTH; length is the number of chars to render. */
void chat_draw_text(RenderContext *ctx, const char *s, int len,
                    float x, float y, uint8_t palette_index);

/* Same as chat_draw_text but each 5x7 glyph pixel is rendered as a `scale`x
 * square block, giving big chunky title text. Returns the per-cell width
 * (glyph + 1px kerning) × scale, useful for centering. */
int chat_draw_text_scaled(RenderContext *ctx, const char *s, int len,
                          float x, float y, uint8_t palette_index, int scale);

#endif
