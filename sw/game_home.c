#include "game_home.h"

#include <dirent.h>
#include <errno.h>
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
#define HOME_CURSOR_SPEED 1.0f
#define HOME_ROW_H 22.0f
#define HOME_ROW_STEP 28.0f
#define HOME_LOAD_W 456.0f
#define HOME_DELETE_GAP 10.0f
#define HOME_DELETE_W 82.0f

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
    float cursor_x;
    float cursor_y;
    int delete_confirm_index;
    char status[96];
} HomeMenuState;

typedef struct {
    float list_x;
    float list_y;
    float list_w;
    float load_w;
    float delete_x;
    float delete_w;
    float row_h;
    float row_step;
    float footer_y;
} HomeMenuLayout;

typedef enum {
    HOME_HIT_NONE = 0,
    HOME_HIT_NEW,
    HOME_HIT_WORLD,
    HOME_HIT_DELETE,
} HomeHitKind;

typedef struct {
    HomeHitKind kind;
    int world_index;
    int option_index;
} HomeHit;

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

static bool remove_path_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;

    if (!path || path[0] == '\0')
        return false;
    if (lstat(path, &st) < 0)
        return false;

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return unlink(path) == 0;

    dir = opendir(path);
    if (!dir)
        return false;

    while ((entry = readdir(dir)) != NULL) {
        char child[WORLD_SAVE_PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (!join_path(child, sizeof(child), path, entry->d_name)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return false;
        }
        if (!remove_path_tree(child)) {
            int saved_errno = errno;

            closedir(dir);
            errno = saved_errno;
            return false;
        }
    }

    if (closedir(dir) < 0)
        return false;
    return rmdir(path) == 0;
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

static float clamp_home_cursor(float value, float min_value, float max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static bool point_in_rect(float x, float y,
                          float x0, float y0,
                          float x1, float y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static void home_menu_layout(const HomeMenuState *menu, HomeMenuLayout *layout)
{
    int option_rows = (menu && menu->world_count > 0 ? menu->world_count : 1) + 1;
    float total_h = (float)option_rows * HOME_ROW_STEP;
    float list_w = HOME_LOAD_W + HOME_DELETE_GAP + HOME_DELETE_W;
    float list_y = 124.0f;
    float footer_y = SCREEN_HEIGHT - 30.0f;

    if (!layout)
        return;

    if (list_y + total_h > footer_y - 6.0f)
        list_y = footer_y - 6.0f - total_h;
    if (list_y < 110.0f)
        list_y = 110.0f;

    layout->list_w = list_w;
    layout->load_w = HOME_LOAD_W;
    layout->delete_w = HOME_DELETE_W;
    layout->list_x = floorf((SCREEN_WIDTH - list_w) * 0.5f);
    layout->list_y = list_y;
    layout->delete_x = layout->list_x + HOME_LOAD_W + HOME_DELETE_GAP;
    layout->row_h = HOME_ROW_H;
    layout->row_step = HOME_ROW_STEP;
    layout->footer_y = footer_y;
}

static HomeHit home_menu_hit_test(const HomeMenuState *menu,
                                  const HomeMenuLayout *layout,
                                  float x, float y)
{
    HomeHit hit = { HOME_HIT_NONE, -1, -1 };

    if (!menu || !layout)
        return hit;

    if (point_in_rect(x, y,
                      layout->list_x, layout->list_y,
                      layout->list_x + layout->list_w,
                      layout->list_y + layout->row_h)) {
        hit.kind = HOME_HIT_NEW;
        hit.option_index = 0;
        return hit;
    }

    for (int i = 0; i < menu->world_count; i++) {
        float row_y = layout->list_y + (float)(i + 1) * layout->row_step;

        if (point_in_rect(x, y,
                          layout->delete_x, row_y,
                          layout->delete_x + layout->delete_w,
                          row_y + layout->row_h)) {
            hit.kind = HOME_HIT_DELETE;
            hit.world_index = i;
            hit.option_index = i + 1;
            return hit;
        }
        if (point_in_rect(x, y,
                          layout->list_x, row_y,
                          layout->list_x + layout->load_w,
                          row_y + layout->row_h)) {
            hit.kind = HOME_HIT_WORLD;
            hit.world_index = i;
            hit.option_index = i + 1;
            return hit;
        }
    }

    return hit;
}

static void home_set_selected(HomeMenuState *menu, int selected)
{
    if (!menu)
        return;
    if (selected < 0)
        selected = 0;
    if (selected > menu->world_count)
        selected = menu->world_count;
    if (menu->selected != selected) {
        menu->selected = selected;
        menu->delete_confirm_index = -1;
    }
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

static void draw_menu_button_styled(RenderContext *ctx, float x, float y,
                                    float w, float h, const char *label,
                                    bool selected, uint8_t panel_pal,
                                    uint8_t text_pal)
{
    int len = label ? (int)strlen(label) : 0;
    int cell_w = chat_font_cell_w();
    int cell_h = chat_font_cell_h();
    float text_w = (float)(len * cell_w);
    float tx = floorf(x + (w - text_w) * 0.5f);
    float ty = floorf(y + (h - (float)cell_h) * 0.5f);

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

static void draw_menu_button(RenderContext *ctx, float x, float y,
                             float w, float h, const char *label, bool selected)
{
    draw_menu_button_styled(ctx, x, y, w, h, label, selected,
                            selected ? 24 : 14,
                            selected ? 8 : 5);
}

static void draw_home_cursor(RenderContext *ctx, float cursor_x, float cursor_y)
{
    const float s = HUD_SCALE;

    renderer_fill_rect(ctx, cursor_x + 1.0f, cursor_y + 1.0f,
                       cursor_x + 7.0f * s + 1.0f,
                       cursor_y + 1.0f * s + 1.0f,
                       0, 0);
    renderer_fill_rect(ctx, cursor_x + 1.0f, cursor_y + 1.0f,
                       cursor_x + 1.0f * s + 1.0f,
                       cursor_y + 9.0f * s + 1.0f,
                       0, 0);
    renderer_fill_rect(ctx, cursor_x, cursor_y,
                       cursor_x + 7.0f * s, cursor_y + 1.0f * s,
                       5, 0);
    renderer_fill_rect(ctx, cursor_x, cursor_y,
                       cursor_x + 1.0f * s, cursor_y + 9.0f * s,
                       5, 0);
}

static void draw_home_menu(RenderContext *ctx, const HomeMenuState *menu,
                           const char *worlds_root)
{
    char line[128];
    const char *title = "EMBEDDED BLOCK GAME";
    int title_len = (int)strlen(title);
    int title_cell = (5 + 1) * 4;
    float title_w = (float)(title_len * title_cell);
    float title_x = floorf((SCREEN_WIDTH - title_w) * 0.5f);
    float title_y = 36.0f;
    HomeMenuLayout layout;

    home_menu_layout(menu, &layout);

    draw_dirt_tiled_bg(ctx);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);

    chat_draw_text_scaled(ctx, title, title_len,
                          title_x + 2.0f, title_y + 2.0f, 0, 4);
    chat_draw_text_scaled(ctx, title, title_len, title_x, title_y, 5, 4);
    draw_centered_text_scaled(ctx, "SELECT WORLD", title_y + 36.0f, 8, 2);

    draw_menu_button(ctx, layout.list_x, layout.list_y,
                     layout.list_w, layout.row_h,
                     "NEW RANDOM WORLD", menu->selected == 0);

    if (menu->world_count == 0) {
        float y = layout.list_y + layout.row_step;
        draw_menu_button(ctx, layout.list_x, y,
                         layout.list_w, layout.row_h,
                         "(NO SAVED WORLDS)", false);
    } else {
        for (int i = 0; i < menu->world_count; i++) {
            const HomeWorldEntry *world = &menu->worlds[i];
            bool row_selected = menu->selected == i + 1;
            bool confirm = menu->delete_confirm_index == i;
            float y = layout.list_y + (float)(i + 1) * layout.row_step;

            snprintf(line, sizeof(line), "LOAD  %-27.27s  SEED %08x",
                     world->name, world->seed);
            draw_menu_button(ctx, layout.list_x, y,
                             layout.load_w, layout.row_h,
                             line, row_selected);
            draw_menu_button_styled(ctx, layout.delete_x, y,
                                    layout.delete_w, layout.row_h,
                                    confirm ? "CONFIRM" : "DELETE",
                                    row_selected || confirm,
                                    confirm ? 6 : (row_selected ? 23 : 14),
                                    confirm ? 5 : (row_selected ? 8 : 5));
        }
    }

    snprintf(line, sizeof(line),
             "W/S OR CURSOR SELECT   ENTER/SPACE/CLICK START   DEL/BKSP DELETE   ESC QUIT");
    draw_centered_text(ctx, line, layout.footer_y, 5);

    if (menu->delete_confirm_index >= 0 &&
        menu->delete_confirm_index < menu->world_count) {
        snprintf(line, sizeof(line), "DELETE %s?  DEL/BKSP OR CLICK CONFIRM",
                 menu->worlds[menu->delete_confirm_index].name);
        draw_centered_text(ctx, line, layout.footer_y - 15.0f, 6);
    } else if (menu->status[0] != '\0') {
        snprintf(line, sizeof(line), "%s", menu->status);
        draw_centered_text(ctx, line, layout.footer_y - 15.0f, 8);
    }

    if (worlds_root && worlds_root[0]) {
        snprintf(line, sizeof(line), "WORLDS: %s", worlds_root);
        draw_centered_text(ctx, line, layout.footer_y - 30.0f, 14);
    }

    draw_home_cursor(ctx, menu->cursor_x, menu->cursor_y);
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
    inp->menu_delete_pressed = false;
    inp->break_pressed = false;
    inp->break_down = false;
    inp->place_pressed = false;
    inp->item_drop_pressed = false;
    inp->pause_toggle_pressed = false;
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

static bool home_select_current(HomeMenuState *menu, const char *worlds_root,
                                SelectedWorld *selection)
{
    if (!menu || !selection)
        return false;

    if (menu->selected == 0) {
        if (!choose_new_world_path(worlds_root, selection))
            return false;
    } else if (menu->selected > 0 && menu->selected <= menu->world_count) {
        HomeWorldEntry *world = &menu->worlds[menu->selected - 1];

        snprintf(selection->name, sizeof(selection->name), "%s",
                 world->name);
        snprintf(selection->path, sizeof(selection->path), "%s",
                 world->path);
        selection->seed = world->seed;
        selection->stone_tries_per_chunk =
            world->stone_tries_per_chunk;
    } else {
        return false;
    }

    return true;
}

static void home_delete_selected(HomeMenuState *menu, const char *worlds_root)
{
    int world_index;
    char deleted_name[GAME_HOME_NAME_MAX];
    char deleted_path[WORLD_SAVE_PATH_MAX];

    if (!menu || menu->selected <= 0 || menu->selected > menu->world_count)
        return;

    world_index = menu->selected - 1;
    if (menu->delete_confirm_index != world_index) {
        menu->delete_confirm_index = world_index;
        snprintf(menu->status, sizeof(menu->status), "DELETE ARMED");
        return;
    }

    snprintf(deleted_name, sizeof(deleted_name), "%s",
             menu->worlds[world_index].name);
    snprintf(deleted_path, sizeof(deleted_path), "%s",
             menu->worlds[world_index].path);

    if (!remove_path_tree(deleted_path)) {
        snprintf(menu->status, sizeof(menu->status),
                 "DELETE FAILED: %s", deleted_name);
        fprintf(stderr, "home: failed to delete world '%s' at %s: %s\n",
                deleted_name, deleted_path, strerror(errno));
        return;
    }

    snprintf(menu->status, sizeof(menu->status), "DELETED: %s", deleted_name);
    menu->world_count = scan_saved_worlds(worlds_root, menu->worlds,
                                          HOME_MAX_WORLDS);
    if (menu->selected > menu->world_count)
        menu->selected = menu->world_count;
    if (menu->selected < 0)
        menu->selected = 0;
    menu->delete_confirm_index = -1;
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
    menu.cursor_x = SCREEN_WIDTH * 0.5f;
    menu.cursor_y = SCREEN_HEIGHT * 0.5f;
    menu.delete_confirm_index = -1;
    input_set_pointer_capture(inp, false);

    while (!inp->quit) {
        struct timespec frame_start;
        struct timespec frame_end;
        int option_count = menu.world_count + 1;
        HomeMenuLayout layout;
        HomeHit cursor_hit;
        bool cursor_moved;
        bool clicked;
        bool delete_pressed;
        bool select_pressed;
        bool jump_pressed;

        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        input_update(inp);
        if (input_consume_pause_toggle(inp)) {
            inp->quit = true;
            break;
        }

        home_menu_layout(&menu, &layout);
        cursor_moved = inp->cursor_dx != 0.0f || inp->cursor_dy != 0.0f;
        menu.cursor_x += inp->cursor_dx * HOME_CURSOR_SPEED;
        menu.cursor_y += inp->cursor_dy * HOME_CURSOR_SPEED;
        menu.cursor_x = clamp_home_cursor(menu.cursor_x, 0.0f,
                                          SCREEN_WIDTH - 1.0f);
        menu.cursor_y = clamp_home_cursor(menu.cursor_y, 0.0f,
                                          SCREEN_HEIGHT - 1.0f);
        cursor_hit = home_menu_hit_test(&menu, &layout,
                                        menu.cursor_x, menu.cursor_y);
        if (cursor_moved && cursor_hit.option_index >= 0)
            home_set_selected(&menu, cursor_hit.option_index);

        if (home_edge_pressed(inp->look_up || inp->forward, &menu.prev_up)) {
            int selected = menu.selected - 1;

            if (selected < 0)
                selected = option_count - 1;
            home_set_selected(&menu, selected);
        }
        if (home_edge_pressed(inp->look_down || inp->back, &menu.prev_down)) {
            int selected = menu.selected + 1;

            if (selected >= option_count)
                selected = 0;
            home_set_selected(&menu, selected);
        }

        clicked = input_consume_break(inp);
        delete_pressed = input_consume_menu_delete(inp);
        select_pressed = input_consume_menu_select(inp);
        jump_pressed = input_consume_jump(inp);
        if (clicked && cursor_hit.option_index >= 0)
            home_set_selected(&menu, cursor_hit.option_index);

        if ((clicked && cursor_hit.kind == HOME_HIT_DELETE) ||
            delete_pressed) {
            home_delete_selected(&menu, worlds_root);
        } else if ((clicked &&
                    (cursor_hit.kind == HOME_HIT_NEW ||
                     cursor_hit.kind == HOME_HIT_WORLD)) ||
                   select_pressed ||
                   jump_pressed) {
            if (home_select_current(&menu, worlds_root, selection)) {
                clear_home_menu_input(inp);
                input_set_pointer_capture(inp, true);
                return true;
            }
        }
        input_clear_mouse(inp);

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
