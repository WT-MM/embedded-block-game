#include "chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "voxel_gpu.h"   /* QUAD_ALPHA_50 */

#define GLYPH_W        5
#define GLYPH_H        7
#define CELL_W         (GLYPH_W * CHAT_TEXT_SCALE + CHAT_TEXT_SCALE)
#define CELL_H         (GLYPH_H * CHAT_TEXT_SCALE + CHAT_TEXT_SCALE)

#define PAL_TEXT       5   /* bright white in default palette */
#define PAL_BG         14  /* medium grey (0x5c5c5c) — reads like Minecraft's bg */

#define CHAT_MARGIN_X  4.0f
#define CHAT_PAD_Y     3.0f

#define CHAT_FADE_SECONDS 8.0f   /* closed-chat history stays visible this long */

typedef struct {
    char code;
    const char *pixels; /* 7 rows × 5 cols row-major, '#' = on */
} GlyphSource;

static const GlyphSource FONT_SOURCES[] = {
    {' ', "....."
          "....."
          "....."
          "....."
          "....."
          "....."
          "....."},
    {'0', ".###."
          "#..##"
          "#.#.#"
          "#.#.#"
          "#.#.#"
          "##..#"
          ".###."},
    {'1', "..#.."
          ".##.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          ".###."},
    {'2', ".###."
          "#...#"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#####"},
    {'3', ".###."
          "#...#"
          "....#"
          "..##."
          "....#"
          "#...#"
          ".###."},
    {'4', "...#."
          "..##."
          ".#.#."
          "#..#."
          "#####"
          "...#."
          "...#."},
    {'5', "#####"
          "#...."
          "####."
          "....#"
          "....#"
          "#...#"
          ".###."},
    {'6', "..##."
          ".#..."
          "#...."
          "####."
          "#...#"
          "#...#"
          ".###."},
    {'7', "#####"
          "....#"
          "...#."
          "..#.."
          ".#..."
          ".#..."
          ".#..."},
    {'8', ".###."
          "#...#"
          "#...#"
          ".###."
          "#...#"
          "#...#"
          ".###."},
    {'9', ".###."
          "#...#"
          "#...#"
          ".####"
          "....#"
          "...#."
          ".##.."},
    {'A', "..#.."
          ".#.#."
          "#...#"
          "#...#"
          "#####"
          "#...#"
          "#...#"},
    {'B', "####."
          "#...#"
          "#...#"
          "####."
          "#...#"
          "#...#"
          "####."},
    {'C', ".####"
          "#...."
          "#...."
          "#...."
          "#...."
          "#...."
          ".####"},
    {'D', "####."
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "####."},
    {'E', "#####"
          "#...."
          "#...."
          "####."
          "#...."
          "#...."
          "#####"},
    {'F', "#####"
          "#...."
          "#...."
          "####."
          "#...."
          "#...."
          "#...."},
    {'G', ".####"
          "#...."
          "#...."
          "#..##"
          "#...#"
          "#...#"
          ".####"},
    {'H', "#...#"
          "#...#"
          "#...#"
          "#####"
          "#...#"
          "#...#"
          "#...#"},
    {'I', ".###."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          ".###."},
    {'J', "..###"
          "...#."
          "...#."
          "...#."
          "...#."
          "#..#."
          ".##.."},
    {'K', "#...#"
          "#..#."
          "#.#.."
          "##..."
          "#.#.."
          "#..#."
          "#...#"},
    {'L', "#...."
          "#...."
          "#...."
          "#...."
          "#...."
          "#...."
          "#####"},
    {'M', "#...#"
          "##.##"
          "#.#.#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"},
    {'N', "#...#"
          "##..#"
          "#.#.#"
          "#..##"
          "#...#"
          "#...#"
          "#...#"},
    {'O', ".###."
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".###."},
    {'P', "####."
          "#...#"
          "#...#"
          "####."
          "#...."
          "#...."
          "#...."},
    {'Q', ".###."
          "#...#"
          "#...#"
          "#...#"
          "#.#.#"
          "#..#."
          ".##.#"},
    {'R', "####."
          "#...#"
          "#...#"
          "####."
          "#.#.."
          "#..#."
          "#...#"},
    {'S', ".####"
          "#...."
          "#...."
          ".###."
          "....#"
          "....#"
          "####."},
    {'T', "#####"
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."},
    {'U', "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".###."},
    {'V', "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".#.#."
          "..#.."},
    {'W', "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#.#.#"
          "##.##"
          "#...#"},
    {'X', "#...#"
          "#...#"
          ".#.#."
          "..#.."
          ".#.#."
          "#...#"
          "#...#"},
    {'Y', "#...#"
          "#...#"
          ".#.#."
          "..#.."
          "..#.."
          "..#.."
          "..#.."},
    {'Z', "#####"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#...."
          "#####"},
    {'.', "....."
          "....."
          "....."
          "....."
          "....."
          ".##.."
          ".##.."},
    {',', "....."
          "....."
          "....."
          "....."
          "....."
          ".##.."
          ".#..."},
    {'!', "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "....."
          "..#.."},
    {'?', ".###."
          "#...#"
          "....#"
          "...#."
          "..#.."
          "....."
          "..#.."},
    {':', "....."
          ".##.."
          ".##.."
          "....."
          ".##.."
          ".##.."
          "....."},
    {';', "....."
          ".##.."
          ".##.."
          "....."
          ".##.."
          ".##.."
          ".#..."},
    {'-', "....."
          "....."
          "....."
          ".###."
          "....."
          "....."
          "....."},
    {'_', "....."
          "....."
          "....."
          "....."
          "....."
          "....."
          "#####"},
    {'\'', "..#.."
           "..#.."
           "..#.."
           "....."
           "....."
           "....."
           "....."},
    {'"', ".#.#."
          ".#.#."
          ".#.#."
          "....."
          "....."
          "....."
          "....."},
    {'/', "....#"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#...."
          "#...."},
    {'(', "..##."
          ".#..."
          ".#..."
          ".#..."
          ".#..."
          ".#..."
          "..##."},
    {')', ".##.."
          "...#."
          "...#."
          "...#."
          "...#."
          "...#."
          ".##.."},
    {'[', ".###."
          ".#..."
          ".#..."
          ".#..."
          ".#..."
          ".#..."
          ".###."},
    {']', ".###."
          "...#."
          "...#."
          "...#."
          "...#."
          "...#."
          ".###."},
    {'=', "....."
          "....."
          "#####"
          "....."
          "#####"
          "....."
          "....."},
    {'+', "....."
          "..#.."
          "..#.."
          "#####"
          "..#.."
          "..#.."
          "....."},
    {'>', "#...."
          ".#..."
          "..#.."
          "...#."
          "..#.."
          ".#..."
          "#...."},
    {'<', "....#"
          "...#."
          "..#.."
          ".#..."
          "..#.."
          "...#."
          "....#"},
    {'*', "....."
          "#...#"
          ".###."
          "#####"
          ".###."
          "#...#"
          "....."},
};

#define NUM_GLYPH_SOURCES ((int)(sizeof(FONT_SOURCES) / sizeof(FONT_SOURCES[0])))

static uint8_t g_glyphs[128][GLYPH_H];
static bool    g_glyphs_present[128];
static bool    g_glyphs_ready = false;

static void build_glyph_tables(void)
{
    if (g_glyphs_ready) return;

    memset(g_glyphs, 0, sizeof(g_glyphs));
    memset(g_glyphs_present, 0, sizeof(g_glyphs_present));

    for (int i = 0; i < NUM_GLYPH_SOURCES; i++) {
        const GlyphSource *src = &FONT_SOURCES[i];
        uint8_t code = (uint8_t)src->code;
        if (code >= 128) continue;
        for (int row = 0; row < GLYPH_H; row++) {
            uint8_t bits = 0;
            for (int col = 0; col < GLYPH_W; col++) {
                if (src->pixels[row * GLYPH_W + col] == '#')
                    bits |= (uint8_t)(1u << (GLYPH_W - 1 - col));
            }
            g_glyphs[code][row] = bits;
        }
        g_glyphs_present[code] = true;
    }

    g_glyphs_ready = true;
}

static uint8_t resolve_glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    uint8_t c = (uint8_t)ch;
    if (c < 128 && g_glyphs_present[c])
        return c;
    return (uint8_t)'?';
}

void chat_init(Chat *chat)
{
    memset(chat, 0, sizeof(*chat));
    build_glyph_tables();
}

void chat_toggle(Chat *chat)
{
    chat->open = !chat->open;
    chat->input_len = 0;
    chat->input[0] = '\0';
    /* Opening chat always brings history back into view, regardless of how
     * long ago the last log was. */
    if (chat->open)
        chat->age_since_activity = 0.0f;
}

bool chat_is_open(const Chat *chat)
{
    return chat->open;
}

void chat_tick(Chat *chat, float dt)
{
    if (!chat) return;
    if (chat->open) {
        /* History is always visible while the chat is open. */
        chat->age_since_activity = 0.0f;
        return;
    }
    chat->age_since_activity += dt;
    if (chat->age_since_activity > CHAT_FADE_SECONDS * 2.0f)
        chat->age_since_activity = CHAT_FADE_SECONDS * 2.0f; /* saturate */
}

static void push_history_line(Chat *chat, const char *line, int len)
{
    if (len < 0) len = 0;
    if (len > CHAT_LINE_MAX) len = CHAT_LINE_MAX;

    int slot = chat->history_head;
    memcpy(chat->history[slot], line, (size_t)len);
    chat->history[slot][len] = '\0';
    chat->history_len[slot] = len;
    chat->history_head = (chat->history_head + 1) % CHAT_HISTORY_LINES;
    if (chat->history_count < CHAT_HISTORY_LINES)
        chat->history_count++;

    /* Any new line re-shows the history and restarts the fade timer. */
    chat->age_since_activity = 0.0f;
}

void chat_log(Chat *chat, const char *fmt, ...)
{
    if (!chat || !fmt) return;

    char buf[CHAT_LINE_MAX * 4 + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    int start = 0;
    for (int i = 0; i <= n; i++) {
        bool newline = (buf[i] == '\n');
        bool terminator = (i == n);
        int chunk_len = i - start;
        if (newline || terminator || chunk_len >= CHAT_LINE_MAX) {
            push_history_line(chat, &buf[start], chunk_len);
            start = newline ? i + 1 : i;
            if (terminator) break;
        }
    }
}

void chat_handle_char(Chat *chat, char ch)
{
    if (!chat->open) return;
    if (chat->input_len >= CHAT_LINE_MAX) return;
    if ((unsigned char)ch < 32 || (unsigned char)ch > 126) return;
    chat->input[chat->input_len++] = ch;
    chat->input[chat->input_len] = '\0';
}

void chat_handle_backspace(Chat *chat)
{
    if (!chat->open) return;
    if (chat->input_len <= 0) return;
    chat->input_len--;
    chat->input[chat->input_len] = '\0';
}

void chat_handle_enter(Chat *chat)
{
    if (!chat->open) return;
    if (chat->input_len > 0) {
        char line[CHAT_LINE_MAX + 4];
        int n = snprintf(line, sizeof(line), "> %s", chat->input);
        if (n < 0) n = 0;
        if (n > CHAT_LINE_MAX) n = CHAT_LINE_MAX;
        push_history_line(chat, line, n);
    }
    chat->input_len = 0;
    chat->input[0] = '\0';
}

static void draw_glyph(RenderContext *ctx, char ch, float x, float y, uint8_t palette)
{
    uint8_t code = resolve_glyph(ch);
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g_glyphs[code][row];
        if (!bits) continue;
        for (int col = 0; col < GLYPH_W; col++) {
            if (!(bits & (1u << (GLYPH_W - 1 - col)))) continue;
            float px = x + (float)(col * CHAT_TEXT_SCALE);
            float py = y + (float)(row * CHAT_TEXT_SCALE);
            renderer_fill_rect(ctx,
                               px, py,
                               px + (float)CHAT_TEXT_SCALE,
                               py + (float)CHAT_TEXT_SCALE,
                               palette, 0);
        }
    }
}

static void draw_line(RenderContext *ctx, const char *s, int len,
                      float x, float y, uint8_t palette)
{
    for (int i = 0; i < len; i++) {
        if (x + (float)CELL_W > SCREEN_WIDTH)
            break;
        if (s[i] != ' ')
            draw_glyph(ctx, s[i], x, y, palette);
        x += (float)CELL_W;
    }
}

void chat_draw(const Chat *chat, RenderContext *ctx)
{
    if (!ctx) return;

    int visible_history = chat->history_count;
    if (visible_history > CHAT_HISTORY_LINES) visible_history = CHAT_HISTORY_LINES;

    /* When chat is closed, history stays visible only for the fade window. */
    bool show_history = chat->open || (chat->age_since_activity < CHAT_FADE_SECONDS);
    if (!chat->open && !show_history)
        return;
    if (!chat->open && visible_history == 0)
        return;

    int rendered_history = show_history ? visible_history : 0;
    int total_lines = rendered_history + (chat->open ? 1 : 0);
    if (total_lines <= 0) return;

    float content_h = (float)(total_lines * CELL_H) + 2.0f * CHAT_PAD_Y;
    float y_bot     = SCREEN_HEIGHT;
    float y_top     = y_bot - content_h;

    /* Background only while the chat is actively open — fading history
     * shows as bare text, matching Minecraft's floating message style. */
    if (chat->open) {
        renderer_fill_rect(ctx, 0.0f, y_top, SCREEN_WIDTH, y_bot,
                           PAL_BG, QUAD_ALPHA_50);
    }

    float input_y = y_bot - (float)CELL_H - CHAT_PAD_Y;

    for (int row = 0; row < rendered_history; row++) {
        int idx = (chat->history_head - rendered_history + row + CHAT_HISTORY_LINES * 2) %
                  CHAT_HISTORY_LINES;
        float y = input_y - (float)((rendered_history - row) * CELL_H);
        draw_line(ctx,
                  chat->history[idx], chat->history_len[idx],
                  CHAT_MARGIN_X, y, PAL_TEXT);
    }

    if (chat->open) {
        char line[CHAT_LINE_MAX + 4];
        int n = snprintf(line, sizeof(line), "> %s_", chat->input);
        if (n < 0) n = 0;
        draw_line(ctx, line, n, CHAT_MARGIN_X, input_y, PAL_TEXT);
    }
}
