#include "game_home.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "block_types.h"
#include "chat.h"

#define DEFAULT_WORLD_SAVE_DIR "../worlds/default"
#define DEFAULT_WORLDS_DIR "../worlds"
#define HOME_MAX_WORLDS 10
#define STONE_SEED 0x48403421u
#define STONE_TRIES_PER_CHUNK 24
#define DIRT_BG_TILE_PX 64.0f

typedef struct {
    char name[GAME_HOME_NAME_MAX];
    char path[WORLD_SAVE_PATH_MAX];
    uint32_t seed;
    int stone_tries_per_chunk;
} HomeWorldEntry;

typedef struct {
    HomeWorldEntry worlds[HOME_MAX_WORLDS];
    int world_count;
    int selected;
    bool prev_up;
    bool prev_down;
} HomeMenuState;

static long home_ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

static const char *read_world_save_dir(void)
{
    const char *value = getenv("VOXEL_WORLD_DIR");

    if (!value || value[0] == '\0')
        return DEFAULT_WORLD_SAVE_DIR;

    return value;
}

static const char *read_worlds_dir(void)
{
    const char *value = getenv("VOXEL_WORLDS_DIR");

    if (!value || value[0] == '\0')
        return DEFAULT_WORLDS_DIR;

    return value;
}

static bool path_is_directory(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool join_path(char *out, size_t out_size,
                      const char *root, const char *name)
{
    if (!out || out_size == 0 || !root || !name)
        return false;

    return snprintf(out, out_size, "%s/%s", root, name) < (int)out_size;
}

static int compare_world_entries(const void *a, const void *b)
{
    const HomeWorldEntry *wa = (const HomeWorldEntry *)a;
    const HomeWorldEntry *wb = (const HomeWorldEntry *)b;

    return strcmp(wa->name, wb->name);
}

static int scan_saved_worlds(const char *worlds_root,
                             HomeWorldEntry *entries,
                             int max_entries)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    if (!worlds_root || !entries || max_entries <= 0)
        return 0;

    dir = opendir(worlds_root);
    if (!dir)
        return 0;

    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        char path[WORLD_SAVE_PATH_MAX];
        uint32_t seed = 0;
        int stone_tries = STONE_TRIES_PER_CHUNK;

        if (entry->d_name[0] == '.')
            continue;
        if (!join_path(path, sizeof(path), worlds_root, entry->d_name))
            continue;
        if (!path_is_directory(path))
            continue;
        if (!world_read_save_metadata(path, &seed, &stone_tries))
            continue;

        snprintf(entries[count].name, sizeof(entries[count].name), "%s",
                 entry->d_name);
        snprintf(entries[count].path, sizeof(entries[count].path), "%s", path);
        entries[count].seed = seed;
        entries[count].stone_tries_per_chunk = stone_tries;
        count++;
    }

    closedir(dir);
    qsort(entries, (size_t)count, sizeof(entries[0]), compare_world_entries);
    return count;
}

static uint32_t random_world_seed(void)
{
    uint32_t seed = 0;
    FILE *urandom = fopen("/dev/urandom", "rb");

    if (urandom) {
        if (fread(&seed, sizeof(seed), 1, urandom) == 1 && seed != 0) {
            fclose(urandom);
            return seed;
        }
        fclose(urandom);
    }

    seed = (uint32_t)time(NULL);
    seed ^= (uint32_t)getpid() * 0x9e3779b9u;
    seed ^= (uint32_t)clock() * 0x85ebca6bu;
    return seed ? seed : STONE_SEED;
}

static bool choose_new_world_path(const char *worlds_root,
                                  SelectedWorld *selection)
{
    if (!worlds_root || !selection)
        return false;

    for (int attempt = 0; attempt < 32; attempt++) {
        uint32_t seed = random_world_seed();
        char name[GAME_HOME_NAME_MAX];
        char path[WORLD_SAVE_PATH_MAX];

        snprintf(name, sizeof(name), "world_%08x", seed);
        if (!join_path(path, sizeof(path), worlds_root, name))
            return false;
        if (path_is_directory(path))
            continue;

        snprintf(selection->name, sizeof(selection->name), "%s", name);
        snprintf(selection->path, sizeof(selection->path), "%s", path);
        selection->seed = seed;
        selection->stone_tries_per_chunk = STONE_TRIES_PER_CHUNK;
        return true;
    }

    return false;
}

static void draw_centered_text(RenderContext *ctx, const char *text,
                               float y, uint8_t palette_index)
{
    int len = text ? (int)strlen(text) : 0;
    float width = (float)(len * chat_font_cell_w());
    float x = floorf((SCREEN_WIDTH - width) * 0.5f);

    if (x < 0.0f)
        x = 0.0f;
    chat_draw_text(ctx, text, len, x, y, palette_index);
}

static void draw_dirt_tiled_bg(RenderContext *ctx)
{
    float u_span = (SCREEN_WIDTH / DIRT_BG_TILE_PX) * 16.0f;
    float v_span = (SCREEN_HEIGHT / DIRT_BG_TILE_PX) * 16.0f;
    RenderQuad q = {0};

    q.texture_id = (uint8_t)(TEX_TILE_DIRT | QUAD_TEX_REPEAT_UV);
    q.flags = QUAD_FLAG_TEX;
    q.vertices[0] = (Vertex2D){ 0.0f,         0.0f,          0.0f, 0.0f,   0.0f,   1.0f };
    q.vertices[1] = (Vertex2D){ SCREEN_WIDTH, 0.0f,          0.0f, u_span, 0.0f,   1.0f };
    q.vertices[2] = (Vertex2D){ SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, u_span, v_span, 1.0f };
    q.vertices[3] = (Vertex2D){ 0.0f,         SCREEN_HEIGHT, 0.0f, 0.0f,   v_span, 1.0f };
    renderer_push_quad(ctx, &q);
}

static void draw_centered_text_scaled(RenderContext *ctx, const char *text,
                                      float y, uint8_t palette_index, int scale)
{
    int len = text ? (int)strlen(text) : 0;
    int cell_w = (5 + 1) * scale;
    float width = (float)(len * cell_w);
    float x = floorf((SCREEN_WIDTH - width) * 0.5f);

    if (x < 0.0f)
        x = 0.0f;
    chat_draw_text_scaled(ctx, text, len, x, y, palette_index, scale);
}

static void draw_menu_button(RenderContext *ctx, float x, float y,
                             float w, float h, const char *label, bool selected)
{
    uint8_t panel_pal = selected ? 24 : 14;
    int len = label ? (int)strlen(label) : 0;
    int cell_w = chat_font_cell_w();
    int cell_h = chat_font_cell_h();
    float text_w = (float)(len * cell_w);
    float tx = floorf(x + (w - text_w) * 0.5f);
    float ty = floorf(y + (h - (float)cell_h) * 0.5f);
    uint8_t text_pal = selected ? 8 : 5;

    renderer_fill_rect(ctx, x, y, x + w, y + h, panel_pal, QUAD_ALPHA_50);
    if (!selected)
        renderer_fill_rect(ctx, x, y, x + w, y + h, panel_pal, QUAD_ALPHA_50);

    renderer_fill_rect(ctx, x,         y,         x + w, y + 1.0f, 0, 0);
    renderer_fill_rect(ctx, x,         y + h - 1, x + w, y + h,    0, 0);
    renderer_fill_rect(ctx, x,         y,         x + 1, y + h,    0, 0);
    renderer_fill_rect(ctx, x + w - 1, y,         x + w, y + h,    0, 0);
    if (selected)
        renderer_fill_rect(ctx, x + 1, y + 1, x + w - 1, y + 2, 5, 0);

    chat_draw_text(ctx, label, len, tx, ty, text_pal);
}

static void draw_home_menu(RenderContext *ctx, const HomeMenuState *menu,
                           const char *worlds_root)
{
    char line[96];
    const char *title = "EMBEDDED BLOCK GAME";
    int title_len = (int)strlen(title);
    int title_cell = (5 + 1) * 4;
    float title_w = (float)(title_len * title_cell);
    float title_x = floorf((SCREEN_WIDTH - title_w) * 0.5f);
    float title_y = 36.0f;
    int option_count = (menu->world_count > 0 ? menu->world_count : 1) + 1;
    float btn_w = 460.0f;
    float btn_h = 22.0f;
    float btn_step = 28.0f;
    float total_h = (float)option_count * btn_step;
    float btn_x = floorf((SCREEN_WIDTH - btn_w) * 0.5f);
    float btn_y = 124.0f;
    float footer_y = SCREEN_HEIGHT - 30.0f;

    draw_dirt_tiled_bg(ctx);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);

    chat_draw_text_scaled(ctx, title, title_len,
                          title_x + 2.0f, title_y + 2.0f, 0, 4);
    chat_draw_text_scaled(ctx, title, title_len, title_x, title_y, 5, 4);
    draw_centered_text_scaled(ctx, "SELECT WORLD", title_y + 36.0f, 8, 2);

    if (btn_y + total_h > footer_y - 6.0f)
        btn_y = footer_y - 6.0f - total_h;
    if (btn_y < 110.0f)
        btn_y = 110.0f;

    draw_menu_button(ctx, btn_x, btn_y, btn_w, btn_h,
                     "NEW RANDOM WORLD", menu->selected == 0);

    if (menu->world_count == 0) {
        float y = btn_y + btn_step;
        draw_menu_button(ctx, btn_x, y, btn_w, btn_h,
                         "(NO SAVED WORLDS)", false);
    } else {
        for (int i = 0; i < menu->world_count; i++) {
            const HomeWorldEntry *world = &menu->worlds[i];
            float y = btn_y + (float)(i + 1) * btn_step;

            snprintf(line, sizeof(line), "LOAD  %-24s  SEED %08x",
                     world->name, world->seed);
            draw_menu_button(ctx, btn_x, y, btn_w, btn_h,
                             line, menu->selected == i + 1);
        }
    }

    snprintf(line, sizeof(line), "W/S SELECT   ENTER/SPACE START   Q QUIT");
    draw_centered_text(ctx, line, footer_y, 5);

    if (worlds_root && worlds_root[0]) {
        snprintf(line, sizeof(line), "WORLDS: %s", worlds_root);
        draw_centered_text(ctx, line, footer_y - 14.0f, 14);
    }
}

static bool home_edge_pressed(bool now, bool *prev)
{
    bool pressed = now && !*prev;

    *prev = now;
    return pressed;
}

static void clear_home_menu_input(InputState *inp)
{
    if (!inp)
        return;

    inp->forward = inp->back = inp->left = inp->right = false;
    inp->up = inp->down = false;
    inp->sprint = false;
    inp->look_left = inp->look_right = inp->look_up = inp->look_down = false;
    inp->jump_pressed = false;
    inp->menu_select_pressed = false;
    input_clear_mouse(inp);
}

static bool select_world_direct(SelectedWorld *selection)
{
    const char *world_save_dir = read_world_save_dir();
    uint32_t seed = STONE_SEED;
    int stone_tries = STONE_TRIES_PER_CHUNK;

    if (!selection)
        return false;

    (void)world_read_save_metadata(world_save_dir, &seed, &stone_tries);

    snprintf(selection->name, sizeof(selection->name), "%s", world_save_dir);
    snprintf(selection->path, sizeof(selection->path), "%s", world_save_dir);
    selection->seed = seed;
    selection->stone_tries_per_chunk = stone_tries;
    return true;
}

bool run_home_menu(RenderContext *ctx, InputState *inp,
                   int target_fps, SelectedWorld *selection)
{
    const char *direct_world = getenv("VOXEL_WORLD_DIR");
    const char *worlds_root = read_worlds_dir();
    HomeMenuState menu = {0};
    long frame_ns = 1000000000L / target_fps;

    if (!selection)
        return false;
    if (direct_world && direct_world[0] != '\0')
        return select_world_direct(selection);

    menu.world_count = scan_saved_worlds(worlds_root, menu.worlds,
                                         HOME_MAX_WORLDS);
    input_set_pointer_capture(inp, false);

    while (!inp->quit) {
        struct timespec frame_start;
        struct timespec frame_end;
        int option_count = menu.world_count + 1;

        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        input_update(inp);

        if (home_edge_pressed(inp->look_up || inp->forward, &menu.prev_up)) {
            menu.selected--;
            if (menu.selected < 0)
                menu.selected = option_count - 1;
        }
        if (home_edge_pressed(inp->look_down || inp->back, &menu.prev_down)) {
            menu.selected++;
            if (menu.selected >= option_count)
                menu.selected = 0;
        }

        if (input_consume_menu_select(inp) || input_consume_jump(inp)) {
            if (menu.selected == 0) {
                if (!choose_new_world_path(worlds_root, selection))
                    return false;
            } else {
                HomeWorldEntry *world = &menu.worlds[menu.selected - 1];

                snprintf(selection->name, sizeof(selection->name), "%s",
                         world->name);
                snprintf(selection->path, sizeof(selection->path), "%s",
                         world->path);
                selection->seed = world->seed;
                selection->stone_tries_per_chunk =
                    world->stone_tries_per_chunk;
            }
            clear_home_menu_input(inp);
            input_set_pointer_capture(inp, true);
            return true;
        }

        renderer_begin_frame(ctx);
        draw_home_menu(ctx, &menu, worlds_root);
        renderer_end_frame(ctx);

        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long used = home_ns_diff(&frame_end, &frame_start);
        if (used < frame_ns) {
            struct timespec ts = { 0, frame_ns - used };

            nanosleep(&ts, NULL);
        }
    }

    return false;
}
