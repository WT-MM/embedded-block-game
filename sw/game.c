#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "renderer.h"
#include "input.h"
#include "block_types.h"
#include "world.h"
#include "mesh_worker.h"
#include "gen_worker.h"
#include "thread_affinity.h"
#include "player_physics.h"
#include "chat.h"
#include "command_parser.h"
#include "pause_menu.h"
#include "inventory.h"

#define DEFAULT_MOUSE_SENS 0.003f /* radians per pixel */
#define LOOK_SPEED   1.8f         /* radians per second (arrow keys) */
#define PITCH_LIMIT  1.48f        /* ~85 degrees, avoids gimbal flip */
#define DEFAULT_TARGET_FPS 30
#define MIN_TARGET_FPS 15
#define MAX_TARGET_FPS 120
#define PHYSICS_HZ   60
#define PHYSICS_DT   (1.0f / (float)PHYSICS_HZ)
#define MAX_FRAME_DT 0.25f
#define DEFAULT_MAX_PHYSICS_STEPS_PER_FRAME 4
#define MAX_PHYSICS_STEPS_PER_FRAME 16
#define PERF_LOG_NS  1000000000L
#define DEFAULT_WORLD_RENDER_DISTANCE_CHUNKS 3
#define MAX_WORLD_RENDER_DISTANCE_CHUNKS 9
#define DEFAULT_STREAM_CHUNKS_PER_FRAME 0
#define MAX_STREAM_CHUNKS_PER_FRAME 64
#define DEFERRED_LIGHTING_MAX_STREAM_BODY_NS 1000000ULL
#define DEFERRED_LIGHTING_MAX_SPEED_SQ 0.25f
#define STONE_SEED   0x48403421u
#define STONE_TRIES_PER_CHUNK 24
#define HOTBAR_SLOT_COUNT 9
#define HOTBAR_PAGE_COUNT 4
#define BLOCK_REACH_DISTANCE 6.0f
#define BLOCK_TRACE_STEP 0.05f
#define HAND_SWING_SECONDS 0.26f
#define CREATIVE_BLOCK_BREAK_REPEAT_SECONDS 0.12f
#define PLAYER_MAX_HEALTH_UNITS 20
#define PLAYER_MAX_AIR_SECONDS 15.0f
#define PLAYER_AIR_REFILL_PER_SECOND 5.0f
#define DROWN_DAMAGE_INTERVAL_SECONDS 1.0f
#define DROWN_DAMAGE_UNITS 2
#define AIR_BUBBLE_COUNT 10
#define AIR_BUBBLE_POP_WINDOW_SECONDS 0.10f
#define DAMAGE_FLASH_SECONDS 0.50f
#define FALL_DAMAGE_SAFE_DISTANCE 3.0f
#define FALL_DAMAGE_EPSILON 0.01f
#define FALL_DAMAGE_MULTIPLIER 1.0f
#define CREATIVE_FLIGHT_DOUBLE_TAP_SECONDS 0.35f
#define PLAYER_SPAWN_X 0.0f
#define PLAYER_SPAWN_Y ((float)(WORLD_CHUNK_HEIGHT - 2))
#define PLAYER_SPAWN_Z -1.5f
#define COMMAND_TIME_DAY_SECONDS   0.0f
#define COMMAND_TIME_NIGHT_SECONDS 90.0f
#define DEFAULT_WORLD_SAVE_DIR "../worlds/default"
#define DEFAULT_WORLDS_DIR "../worlds"
#define HOME_MAX_WORLDS 10
#define HOME_NAME_MAX 64
#define HOTBAR_SLOT_PIXELS (17.0f * HUD_SCALE)
#define HOTBAR_GAP_PIXELS (2.0f * HUD_SCALE)
#define HOTBAR_BOTTOM_MARGIN_PIXELS (4.0f * HUD_SCALE + 1.0f)
#define ITEM_ENTITY_MAX 128
#define ITEM_ENTITY_SIZE_WORLD 0.42f
#define ITEM_ENTITY_PICKUP_DELAY_SECONDS 0.35f
#define ITEM_ENTITY_LIFETIME_SECONDS 300.0f
#define ITEM_ENTITY_PICKUP_RADIUS 1.35f
#define ITEM_ENTITY_GRAVITY 18.0f
#define INVENTORY_CURSOR_MIN_X 0.0f
#define INVENTORY_CURSOR_MIN_Y 0.0f
#define INVENTORY_CURSOR_SPEED 1.0f

typedef struct {
    char name[HOME_NAME_MAX];
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

typedef struct {
    char name[HOME_NAME_MAX];
    char path[WORLD_SAVE_PATH_MAX];
    uint32_t seed;
    int stone_tries_per_chunk;
} SelectedWorld;

typedef struct {
    bool hit;
    bool place_valid;
    int hit_x;
    int hit_y;
    int hit_z;
    int place_x;
    int place_y;
    int place_z;
} BlockTarget;

typedef struct {
    bool active;
    ItemStack stack;
    Vec3 position;
    Vec3 velocity;
    float age_seconds;
    float pickup_delay_seconds;
    float bob_seed;
} ItemEntity;

typedef struct {
    ItemEntity items[ITEM_ENTITY_MAX];
    uint32_t spawn_counter;
} ItemEntityPool;

typedef struct {
    InventorySlotArea area;
    int index;
} InventoryHit;

typedef struct {
    float panel_x;
    float panel_y;
    float panel_w;
    float panel_h;
    float slot;
    float gap;
    float craft_x;
    float craft_y;
    float output_x;
    float output_y;
    float main_x;
    float main_y;
    float hotbar_x;
    float hotbar_y;
} SurvivalInventoryLayout;

static const BlockID HOTBAR_BLOCKS[HOTBAR_PAGE_COUNT][HOTBAR_SLOT_COUNT] = {
    {
        BLOCK_GRASS,
        BLOCK_DIRT,
        BLOCK_STONE,
        BLOCK_WOOD,
        BLOCK_PLANKS,
        BLOCK_GLASS,
        BLOCK_LAMP,
        BLOCK_LEAVES,
        BLOCK_WATER,
    },
    {
        BLOCK_SAND,
        BLOCK_GRAVEL,
        BLOCK_COBBLESTONE,
        BLOCK_BRICKS,
        BLOCK_OBSIDIAN,
        BLOCK_SANDSTONE,
        BLOCK_CLAY,
        BLOCK_REDSTONE_BLOCK,
        BLOCK_LAVA,
    },
    {
        BLOCK_COAL_ORE,
        BLOCK_IRON_ORE,
        BLOCK_GOLD_ORE,
        BLOCK_DIAMOND_ORE,
        BLOCK_REDSTONE_ORE,
        BLOCK_GOLD_BLOCK,
        BLOCK_DIAMOND_BLOCK,
        BLOCK_RED_FLOWER,
        BLOCK_YELLOW_FLOWER,
    },
    {
        BLOCK_CRAFTING_TABLE,
        BLOCK_DOOR,
        BLOCK_RED_FLOWER,
        BLOCK_YELLOW_FLOWER,
        BLOCK_AIR,
        BLOCK_AIR,
        BLOCK_AIR,
        BLOCK_AIR,
        BLOCK_AIR,
    },
};

static BlockID hotbar_block_at(int page, int slot)
{
    if (page < 0 || page >= HOTBAR_PAGE_COUNT ||
        slot < 0 || slot >= HOTBAR_SLOT_COUNT)
        return BLOCK_AIR;

    return HOTBAR_BLOCKS[page][slot];
}

static const uint8_t HOTBAR_DIGITS[HOTBAR_SLOT_COUNT][5] = {
    { 0x2, 0x6, 0x2, 0x2, 0x7 }, /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 }, /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 }, /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 }, /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 }, /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 }, /* 6 */
    { 0x7, 0x1, 0x1, 0x1, 0x1 }, /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 }, /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 }, /* 9 */
};

static long ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

static double ns_to_ms(double ns)
{
    return ns / 1000000.0;
}

static bool can_rebuild_deferred_lighting(const VoxelWorld *world,
                                          const Player *player)
{
    float horizontal_speed_sq;

    if (!world || !player)
        return false;
    if (world->chunks_generated_last_stream > 0)
        return false;
    if (world->last_stream_body_ns > DEFERRED_LIGHTING_MAX_STREAM_BODY_NS)
        return false;

    horizontal_speed_sq = player->vx * player->vx + player->vz * player->vz;
    return horizontal_speed_sq <= DEFERRED_LIGHTING_MAX_SPEED_SQ;
}

static float read_mouse_sensitivity(void)
{
    const char *value = getenv("VOXEL_MOUSE_SENS");

    if (!value || value[0] == '\0')
        return DEFAULT_MOUSE_SENS;

    char *end = NULL;
    float parsed = strtof(value, &end);

    if (end == value || (end && *end != '\0') || parsed <= 0.0f)
        return DEFAULT_MOUSE_SENS;

    return parsed;
}

static int read_debug_enabled(void)
{
    const char *value = getenv("DEBUG");

    if (!value || value[0] == '\0')
        return 0;
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "no") == 0)
        return 0;
    return 1;
}

static int read_status_log_enabled(int debug_enabled)
{
    const char *value = getenv("VOXEL_STATUS_LOG");

    if (!value || value[0] == '\0' || strcmp(value, "auto") == 0)
        return debug_enabled;
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "no") == 0)
        return 0;

    return 1;
}

static int read_debug_hud_enabled(void)
{
    const char *value = getenv("VOXEL_DEBUG_HUD");

    if (!value || value[0] == '\0')
        return 0;
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "no") == 0)
        return 0;
    return 1;
}

static int read_target_fps(void)
{
    const char *value = getenv("VOXEL_TARGET_FPS");
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0')
        return DEFAULT_TARGET_FPS;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') ||
        parsed < MIN_TARGET_FPS || parsed > MAX_TARGET_FPS)
        return DEFAULT_TARGET_FPS;

    return (int)parsed;
}

static int read_render_distance_chunks(void)
{
    const char *value = getenv("VOXEL_RENDER_DISTANCE");

    if (!value || value[0] == '\0')
        return DEFAULT_WORLD_RENDER_DISTANCE_CHUNKS;

    char *end = NULL;
    long parsed = strtol(value, &end, 10);

    if (end == value || (end && *end != '\0') ||
        parsed < 1 || parsed > MAX_WORLD_RENDER_DISTANCE_CHUNKS)
        return DEFAULT_WORLD_RENDER_DISTANCE_CHUNKS;

    return (int)parsed;
}

static int read_stream_chunks_per_frame(void)
{
    const char *value = getenv("VOXEL_CHUNKS_PER_FRAME");
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0')
        value = getenv("VOXEL_CHUNK_PER_FRAME");
    if (!value || value[0] == '\0')
        return DEFAULT_STREAM_CHUNKS_PER_FRAME;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') ||
        parsed < 0 || parsed > MAX_STREAM_CHUNKS_PER_FRAME)
        return DEFAULT_STREAM_CHUNKS_PER_FRAME;

    return (int)parsed;
}

static int read_max_physics_steps_per_frame(void)
{
    const char *value = getenv("VOXEL_MAX_PHYSICS_STEPS_PER_FRAME");
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0')
        return DEFAULT_MAX_PHYSICS_STEPS_PER_FRAME;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed < 0)
        return DEFAULT_MAX_PHYSICS_STEPS_PER_FRAME;
    if (parsed > MAX_PHYSICS_STEPS_PER_FRAME)
        return MAX_PHYSICS_STEPS_PER_FRAME;
    return (int)parsed;
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
        char name[HOME_NAME_MAX];
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

/*
 * Tiled dirt background for the home menu, drawn as a single textured quad
 * with hardware UV-wrap so we don't pay 80+ small quads per frame. UV span =
 * (pixels / DIRT_BG_TILE_PX) * 16 maps each on-screen tile to one full atlas
 * tile of TEX_TILE_DIRT.
 */
#define DIRT_BG_TILE_PX 64.0f

static void draw_dirt_tiled_bg(RenderContext *ctx)
{
    float u_span = (SCREEN_WIDTH  / DIRT_BG_TILE_PX) * 16.0f;
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
    int cell_w = (5 + 1) * scale;   /* GLYPH_W + 1 kerning, matches chat.c */
    float width = (float)(len * cell_w);
    float x = floorf((SCREEN_WIDTH - width) * 0.5f);

    if (x < 0.0f)
        x = 0.0f;
    chat_draw_text_scaled(ctx, text, len, x, y, palette_index, scale);
}

static void draw_menu_button(RenderContext *ctx, float x, float y,
                             float w, float h, const char *label, bool selected)
{
    /* Stack two 50% panels so the underlying dirt darkens without needing
     * a 25%-or-less hardware blend level we don't have. Selected variant
     * uses a single 50% panel for a brighter, more obvious focus. */
    uint8_t panel_pal = selected ? 24 : 14;   /* stone-light vs medium grey */

    renderer_fill_rect(ctx, x, y, x + w, y + h, panel_pal, QUAD_ALPHA_50);
    if (!selected)
        renderer_fill_rect(ctx, x, y, x + w, y + h, panel_pal, QUAD_ALPHA_50);

    /* 1px outer border in near-black to give the button a chiseled look. */
    renderer_fill_rect(ctx, x,         y,         x + w, y + 1.0f, 0, 0);
    renderer_fill_rect(ctx, x,         y + h - 1, x + w, y + h,    0, 0);
    renderer_fill_rect(ctx, x,         y,         x + 1, y + h,    0, 0);
    renderer_fill_rect(ctx, x + w - 1, y,         x + w, y + h,    0, 0);

    /* Top inner highlight line: 1px lighter strip just inside the top
     * border, classic Minecraft "raised" button look. */
    if (selected)
        renderer_fill_rect(ctx, x + 1, y + 1, x + w - 1, y + 2, 5, 0);

    int len = label ? (int)strlen(label) : 0;
    int cell_w = chat_font_cell_w();
    int cell_h = chat_font_cell_h();
    float text_w = (float)(len * cell_w);
    float tx = floorf(x + (w - text_w) * 0.5f);
    float ty = floorf(y + (h - (float)cell_h) * 0.5f);
    uint8_t text_pal = selected ? 8 : 5;   /* yellow when selected, white otherwise */

    chat_draw_text(ctx, label, len, tx, ty, text_pal);
}

static void draw_home_menu(RenderContext *ctx, const HomeMenuState *menu,
                           const char *worlds_root)
{
    char line[96];

    draw_dirt_tiled_bg(ctx);
    /* Two stacked 50% dim panels darken the dirt enough that the title and
     * buttons read clearly without washing the texture out completely. */
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, QUAD_ALPHA_50);

    /* Title with a 2px drop shadow for the chunky Minecraft look. */
    const char *title = "EMBEDDED BLOCK GAME";
    int title_len = (int)strlen(title);
    int title_cell = (5 + 1) * 4;   /* scale 4 */
    float title_w = (float)(title_len * title_cell);
    float title_x = floorf((SCREEN_WIDTH - title_w) * 0.5f);
    float title_y = 36.0f;

    chat_draw_text_scaled(ctx, title, title_len,
                          title_x + 2.0f, title_y + 2.0f, 0, 4);
    chat_draw_text_scaled(ctx, title, title_len, title_x, title_y, 5, 4);

    draw_centered_text_scaled(ctx, "SELECT WORLD", title_y + 36.0f, 8, 2);

    int option_count = (menu->world_count > 0 ? menu->world_count : 1) + 1;
    float btn_w = 460.0f;
    float btn_h = 22.0f;
    float btn_step = 28.0f;
    float total_h = (float)option_count * btn_step;
    float btn_x = floorf((SCREEN_WIDTH - btn_w) * 0.5f);
    float btn_y = 124.0f;

    /* Push everything up if the world list would otherwise crowd the footer. */
    float footer_y = SCREEN_HEIGHT - 30.0f;
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

    /* Footer: small instruction line and worlds-root path so users can find
     * their saves on disk. */
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

    if (world_read_save_metadata(world_save_dir, &seed, &stone_tries)) {
        /* Existing save: honor its stored procedural parameters. */
    }

    snprintf(selection->name, sizeof(selection->name), "%s", world_save_dir);
    snprintf(selection->path, sizeof(selection->path), "%s", world_save_dir);
    selection->seed = seed;
    selection->stone_tries_per_chunk = stone_tries;
    return true;
}

static bool run_home_menu(RenderContext *ctx, InputState *inp,
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
        long used = ns_diff(&frame_end, &frame_start);
        if (used < frame_ns) {
            struct timespec ts = { 0, frame_ns - used };

            nanosleep(&ts, NULL);
        }
    }

    return false;
}

/* Camera focal length is stored in screen pixels, so the FOV scales with the
 * render resolution unless we compensate. The pre-SDRAM 320x240 build used
 * focal=170 → horizontal FOV ≈ 86.5°. This computes a focal that preserves
 * that FOV at any resolution, with VOXEL_FOV_DEG to override. */
static float compute_camera_focal_px(void)
{
    const float default_fov_deg = 86.5f;
    const char *value = getenv("VOXEL_FOV_DEG");
    float fov_deg = default_fov_deg;

    if (value && value[0] != '\0') {
        char *end = NULL;
        float parsed = strtof(value, &end);
        if (end != value && (!end || *end == '\0') &&
            parsed >= 30.0f && parsed <= 150.0f) {
            fov_deg = parsed;
        }
    }

    float fov_rad = fov_deg * (float)M_PI / 180.0f;
    return (SCREEN_WIDTH * 0.5f) / tanf(fov_rad * 0.5f);
}

static Vec3 camera_forward(const Camera *cam)
{
    float cos_pitch = cosf(cam->pitch);

    return (Vec3){
        sinf(cam->yaw) * cos_pitch,
        -sinf(cam->pitch),
        cosf(cam->yaw) * cos_pitch,
    };
}

static void respawn_player(Player *player, Camera *cam)
{
    PlayerMode mode;
    float yaw = 0.0f;
    float pitch = -0.3f;
    float depth = compute_camera_focal_px();

    if (!player)
        return;

    mode = player->mode;
    if (cam) {
        yaw = cam->yaw;
        pitch = cam->pitch;
        depth = cam->depth;
    }

    player_init(player, PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z);
    player_set_mode(player, mode);

    if (cam) {
        cam->position.x = player->x;
        cam->position.y = player_get_eye_height(player);
        cam->position.z = player->z;
        cam->yaw = yaw;
        cam->pitch = pitch;
        cam->depth = depth;
    }
}

static bool apply_survival_damage(int *health_units, int damage_units,
                                  float *damage_flash_timer)
{
    if (!health_units || damage_units <= 0)
        return false;

    *health_units -= damage_units;
    if (*health_units < 0)
        *health_units = 0;
    if (damage_flash_timer)
        *damage_flash_timer = DAMAGE_FLASH_SECONDS;
    return *health_units <= 0;
}

static int game_floor_div_i(int value, int divisor)
{
    int q = value / divisor;
    int r = value % divisor;

    if (r < 0)
        q--;
    return q;
}

static int game_positive_mod_i(int value, int divisor)
{
    int r = value % divisor;

    if (r < 0)
        r += divisor;
    return r;
}

static bool game_block_is_water(BlockID id)
{
    return id == BLOCK_WATER || id == BLOCK_WATER_FLOW;
}

static bool game_block_is_trace_passable(BlockID id)
{
    return id == BLOCK_AIR || id == BLOCK_WATER ||
           id == BLOCK_WATER_FLOW || id == BLOCK_LAVA ||
           id == BLOCK_LAVA_FLOW;
}

static bool camera_is_underwater(const VoxelWorld *world, const Camera *cam)
{
    int wx = (int)floorf(cam->position.x);
    int wy = (int)floorf(cam->position.y);
    int wz = (int)floorf(cam->position.z);
    BlockID id;

    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    id = world_get_block(world, wx, wy, wz);
    if (id == BLOCK_WATER)
        return true;
    if (id != BLOCK_WATER_FLOW)
        return false;

    if (wy + 1 < WORLD_CHUNK_HEIGHT &&
        game_block_is_water(world_get_block(world, wx, wy + 1, wz)))
        return true;

    int chunk_x = game_floor_div_i(wx, WORLD_CHUNK_SIZE);
    int chunk_z = game_floor_div_i(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    int lx = game_positive_mod_i(wx, WORLD_CHUNK_SIZE);
    int lz = game_positive_mod_i(wz, WORLD_CHUNK_SIZE);
    int level = chunk->water_level[wy][lz][lx];
    if (level < 1)
        level = 1;
    if (level > 7)
        level = 7;

    float water_top = (float)wy + (float)(8 - level) * 0.125f;
    return cam->position.y < water_top - 0.02f;
}

static void draw_underwater_overlay(RenderContext *ctx)
{
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       36, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       7, QUAD_ALPHA_25);
}

static void draw_damage_overlay(RenderContext *ctx, float damage_flash_timer)
{
    if (damage_flash_timer <= 0.0f)
        return;

    uint8_t alpha = (damage_flash_timer > DAMAGE_FLASH_SECONDS * 0.55f) ?
        QUAD_ALPHA_50 : QUAD_ALPHA_25;
    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       6, alpha);
}

static bool trace_target_block(const VoxelWorld *world, const Camera *cam,
                               float max_distance, BlockTarget *out)
{
    Vec3 dir = camera_forward(cam);
    bool have_last_air = false;
    bool have_prev_cell = false;
    int last_air_x = 0;
    int last_air_y = 0;
    int last_air_z = 0;
    int prev_x = 0;
    int prev_y = 0;
    int prev_z = 0;

    if (out)
        memset(out, 0, sizeof(*out));

    for (float distance = 0.0f;
         distance <= max_distance;
         distance += BLOCK_TRACE_STEP) {
        float sample_x = cam->position.x + dir.x * distance;
        float sample_y = cam->position.y + dir.y * distance;
        float sample_z = cam->position.z + dir.z * distance;
        int block_x = (int)floorf(sample_x);
        int block_y = (int)floorf(sample_y);
        int block_z = (int)floorf(sample_z);
        BlockID block;

        if (have_prev_cell &&
            block_x == prev_x &&
            block_y == prev_y &&
            block_z == prev_z)
            continue;

        prev_x = block_x;
        prev_y = block_y;
        prev_z = block_z;
        have_prev_cell = true;

        block = world_get_block(world, block_x, block_y, block_z);
        /* Treat liquids as see-through for targeting, but let passable
         * placeables like flowers still be hit and broken. The last
         * trace-passable cell becomes the placement position. */
        if (!game_block_is_trace_passable(block)) {
            if (!out)
                return true;

            out->hit = true;
            out->place_valid = have_last_air;
            out->hit_x = block_x;
            out->hit_y = block_y;
            out->hit_z = block_z;
            out->place_x = last_air_x;
            out->place_y = last_air_y;
            out->place_z = last_air_z;
            return true;
        }

        have_last_air = true;
        last_air_x = block_x;
        last_air_y = block_y;
        last_air_z = block_z;
    }

    return false;
}

static bool player_intersects_block(const Player *player, int wx, int wy, int wz)
{
    float player_min_x = player->x - PLAYER_WIDTH * 0.5f;
    float player_max_x = player->x + PLAYER_WIDTH * 0.5f;
    float player_min_y = player->y;
    float player_max_y = player->y + PLAYER_HEIGHT;
    float player_min_z = player->z - PLAYER_DEPTH * 0.5f;
    float player_max_z = player->z + PLAYER_DEPTH * 0.5f;
    float block_min_x = (float)wx;
    float block_max_x = block_min_x + 1.0f;
    float block_min_y = (float)wy;
    float block_max_y = block_min_y + 1.0f;
    float block_min_z = (float)wz;
    float block_max_z = block_min_z + 1.0f;

    return player_max_x > block_min_x && player_min_x < block_max_x &&
           player_max_y > block_min_y && player_min_y < block_max_y &&
           player_max_z > block_min_z && player_min_z < block_max_z;
}

static bool break_block_target(VoxelWorld *world, const BlockTarget *target,
                               BlockID *broken_block_out)
{
    BlockID broken;

    if (!target || !target->hit)
        return false;
    broken = world_get_block(world, target->hit_x, target->hit_y, target->hit_z);
    if (broken == BLOCK_AIR)
        return false;

    if (!world_set_block(world,
                         target->hit_x, target->hit_y, target->hit_z,
                         BLOCK_AIR))
        return false;

    if (broken_block_out)
        *broken_block_out = broken;

    /* Route the edited chunk through the mesh worker's priority lane so
     * the broken-block visual lands on the next frame even if the main
     * queue carries a backlog. */
    world_mark_chunk_mesh_edit_priority(world, target->hit_x, target->hit_z);
    return true;
}

static bool try_break_targeted_block(VoxelWorld *world, const Camera *cam)
{
    BlockTarget target = {0};

    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;

    if (!break_block_target(world, &target, NULL))
        return false;

    return true;
}

typedef enum {
    PLACE_OK = 0,
    PLACE_FAIL_BAD_TYPE,
    PLACE_FAIL_NO_TRACE,
    PLACE_FAIL_NO_AIR_NEAR_HIT,
    PLACE_FAIL_TARGET_OCCUPIED,
    PLACE_FAIL_PLAYER_BLOCKED,
    PLACE_FAIL_WORLD_REJECTED,
} PlaceResult;

static PlaceResult try_place_targeted_block(VoxelWorld *world, const Camera *cam,
                                            const Player *player, BlockID type)
{
    BlockTarget target = {0};

    if (type <= BLOCK_AIR || type >= NUM_BLOCK_TYPES)
        return PLACE_FAIL_BAD_TYPE;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return PLACE_FAIL_NO_TRACE;
    if (!target.place_valid)
        return PLACE_FAIL_NO_AIR_NEAR_HIT;
    /* Allow overwriting passable blocks (water) so players can fill water
     * with solids or replace water-flow with a fresh source. */
    BlockID at_place = world_get_block(world, target.place_x, target.place_y, target.place_z);
    if (!block_is_passable(at_place))
        return PLACE_FAIL_TARGET_OCCUPIED;
    if (!block_is_passable(type) && player_intersects_block(player, target.place_x, target.place_y, target.place_z))
        return PLACE_FAIL_PLAYER_BLOCKED;

    if (!world_set_block(world,
                         target.place_x, target.place_y, target.place_z,
                         type))
        return PLACE_FAIL_WORLD_REJECTED;

    world_mark_chunk_mesh_edit_priority(world, target.place_x, target.place_z);
    return PLACE_OK;
}

static const char *place_result_name(PlaceResult r)
{
    switch (r) {
    case PLACE_OK:                    return "ok";
    case PLACE_FAIL_BAD_TYPE:         return "bad-type";
    case PLACE_FAIL_NO_TRACE:         return "no-trace";
    case PLACE_FAIL_NO_AIR_NEAR_HIT:  return "no-air-near-hit";
    case PLACE_FAIL_TARGET_OCCUPIED:  return "target-occupied";
    case PLACE_FAIL_PLAYER_BLOCKED:   return "player-blocked";
    case PLACE_FAIL_WORLD_REJECTED:   return "world-rejected";
    default:                          return "unknown";
    }
}

static void item_entities_init(ItemEntityPool *pool)
{
    if (!pool)
        return;

    memset(pool, 0, sizeof(*pool));
}

static ItemEntity *item_entity_find_spawn_slot(ItemEntityPool *pool)
{
    int oldest_index = 0;
    float oldest_age = -1.0f;

    if (!pool)
        return NULL;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        if (!pool->items[i].active)
            return &pool->items[i];
        if (pool->items[i].age_seconds > oldest_age) {
            oldest_age = pool->items[i].age_seconds;
            oldest_index = i;
        }
    }

    return &pool->items[oldest_index];
}

static void item_entity_spawn_stack(ItemEntityPool *pool,
                                   ItemID item,
                                   int count,
                                   Vec3 position,
                                   Vec3 velocity,
                                   float pickup_delay)
{
    while (pool && count > 0 && item != ITEM_NONE && item < NUM_ITEM_TYPES) {
        ItemEntity *entity = item_entity_find_spawn_slot(pool);
        int stack_count = count > ITEM_STACK_MAX ? ITEM_STACK_MAX : count;

        if (!entity)
            return;

        memset(entity, 0, sizeof(*entity));
        entity->active = true;
        entity->stack.item = item;
        entity->stack.count = (uint8_t)stack_count;
        entity->position = position;
        entity->velocity = velocity;
        entity->age_seconds = 0.0f;
        entity->pickup_delay_seconds = pickup_delay;
        entity->bob_seed = (float)(pool->spawn_counter++ & 31u) * 0.37f;
        count -= stack_count;
    }
}

static void item_entity_spawn_block_drop(ItemEntityPool *pool,
                                        BlockID broken_block,
                                        int wx,
                                        int wy,
                                        int wz,
                                        const Camera *cam)
{
    ItemID drop = survival_drop_for_block(broken_block);
    Vec3 dir = cam ? camera_forward(cam) : (Vec3){ 0.0f, 0.0f, 0.0f };
    Vec3 pos = {
        (float)wx + 0.5f,
        (float)wy + 0.55f,
        (float)wz + 0.5f,
    };
    Vec3 vel = {
        dir.x * 1.2f,
        2.2f,
        dir.z * 1.2f,
    };

    if (drop == ITEM_NONE)
        return;

    item_entity_spawn_stack(pool, drop, 1, pos, vel,
                            ITEM_ENTITY_PICKUP_DELAY_SECONDS);
}

static void item_entity_spawn_near_player(ItemEntityPool *pool,
                                          const Player *player,
                                          const ItemStack *stack)
{
    if (!pool || !player || item_stack_is_empty(stack))
        return;

    Vec3 pos = {
        player->x,
        player->y + PLAYER_HEIGHT * 0.55f,
        player->z,
    };
    Vec3 vel = { 0.0f, 2.0f, 0.0f };

    item_entity_spawn_stack(pool, stack->item, stack->count, pos, vel, 0.0f);
}

static void return_stack_to_inventory_or_drop(SurvivalInventory *inv,
                                              ItemEntityPool *drops,
                                              const Player *player,
                                              ItemStack *stack)
{
    int leftover;

    if (!inv || item_stack_is_empty(stack))
        return;

    leftover = survival_inventory_add_item(inv, stack->item, stack->count);
    if (leftover > 0) {
        ItemStack drop = { stack->item, (uint8_t)leftover };
        item_entity_spawn_near_player(drops, player, &drop);
    }
    item_stack_clear(stack);
}

static void close_survival_inventory(SurvivalInventory *inv,
                                     ItemEntityPool *drops,
                                     const Player *player,
                                     bool *inventory_open)
{
    if (inventory_open)
        *inventory_open = false;
    if (!inv)
        return;

    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        return_stack_to_inventory_or_drop(inv, drops, player, &inv->craft[i]);
    return_stack_to_inventory_or_drop(inv, drops, player, &inv->cursor);
    survival_inventory_refresh_craft_output(inv);
}

static bool item_entity_floor_collision(const VoxelWorld *world,
                                        const ItemEntity *entity,
                                        float *floor_y_out)
{
    int wx;
    int wy;
    int wz;
    BlockID block;

    if (!world || !entity)
        return false;

    wx = (int)floorf(entity->position.x);
    wy = (int)floorf(entity->position.y - 0.24f);
    wz = (int)floorf(entity->position.z);
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    block = world_get_block(world, wx, wy, wz);
    if (block_is_passable(block))
        return false;

    if (floor_y_out)
        *floor_y_out = (float)wy + 1.18f;
    return true;
}

static void item_entities_update(ItemEntityPool *pool,
                                 const VoxelWorld *world,
                                 SurvivalInventory *inventory,
                                 const Player *player,
                                 float dt)
{
    if (!pool || !inventory || !player)
        return;

    if (dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        ItemEntity *entity = &pool->items[i];
        float floor_y;

        if (!entity->active)
            continue;

        entity->age_seconds += dt;
        if (entity->age_seconds >= ITEM_ENTITY_LIFETIME_SECONDS ||
            item_stack_is_empty(&entity->stack)) {
            entity->active = false;
            continue;
        }

        entity->velocity.y -= ITEM_ENTITY_GRAVITY * dt;
        entity->position.x += entity->velocity.x * dt;
        entity->position.y += entity->velocity.y * dt;
        entity->position.z += entity->velocity.z * dt;
        entity->velocity.x *= 0.98f;
        entity->velocity.z *= 0.98f;

        if (item_entity_floor_collision(world, entity, &floor_y) &&
            entity->position.y < floor_y) {
            entity->position.y = floor_y;
            if (entity->velocity.y < 0.0f)
                entity->velocity.y = 0.0f;
            entity->velocity.x *= 0.72f;
            entity->velocity.z *= 0.72f;
        }

        if (entity->age_seconds >= entity->pickup_delay_seconds) {
            float dx = entity->position.x - player->x;
            float dy = entity->position.y - (player->y + PLAYER_HEIGHT * 0.5f);
            float dz = entity->position.z - player->z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            float pickup_sq = ITEM_ENTITY_PICKUP_RADIUS * ITEM_ENTITY_PICKUP_RADIUS;

            if (dist_sq <= pickup_sq) {
                int leftover = survival_inventory_add_item(inventory,
                                                           entity->stack.item,
                                                           entity->stack.count);
                if (leftover <= 0) {
                    entity->active = false;
                } else {
                    entity->stack.count = (uint8_t)leftover;
                    entity->pickup_delay_seconds = entity->age_seconds + 0.4f;
                }
            }
        }
    }
}

static void item_entities_draw(RenderContext *ctx,
                               const ItemEntityPool *pool,
                               float world_time)
{
    if (!ctx || !pool)
        return;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        const ItemEntity *entity = &pool->items[i];
        Vec3 pos;

        if (!entity->active || item_stack_is_empty(&entity->stack))
            continue;

        pos = entity->position;
        pos.y += sinf(world_time * 4.0f + entity->bob_seed) * 0.05f;
        renderer_draw_world_billboard_tile(
            ctx, pos, ITEM_ENTITY_SIZE_WORLD,
            item_texture_id(entity->stack.item),
            QUAD_FLAG_ALPHA_KEY | QUAD_FLAG_ZTEST);
    }
}

static bool command_gamemode_to_player(GameCommandGameModeValue value,
                                       PlayerMode *out)
{
    if (!out)
        return false;

    switch (value) {
    case GAME_COMMAND_GAMEMODE_SURVIVAL:
        *out = PLAYER_MODE_SURVIVAL;
        return true;
    case GAME_COMMAND_GAMEMODE_CREATIVE:
        *out = PLAYER_MODE_CREATIVE;
        return true;
    case GAME_COMMAND_GAMEMODE_SPECTATOR:
        *out = PLAYER_MODE_SPECTATOR;
        return true;
    default:
        return false;
    }
}

static float command_time_seconds(GameCommandTimeValue value)
{
    switch (value) {
    case GAME_COMMAND_TIME_DAY:
        return COMMAND_TIME_DAY_SECONDS;
    case GAME_COMMAND_TIME_NIGHT:
        return COMMAND_TIME_NIGHT_SECONDS;
    default:
        return COMMAND_TIME_DAY_SECONDS;
    }
}

static bool execute_chat_command(Chat *chat, Player *player,
                                 float *world_time, bool *kill_requested,
                                 const char *line)
{
    GameCommandParseResult parsed;
    GameCommandParseStatus status = game_command_parse(line, &parsed);

    if (status == GAME_COMMAND_PARSE_NOT_COMMAND)
        return false;
    if (status != GAME_COMMAND_PARSE_OK) {
        chat_log(chat, "%s", parsed.error[0] ? parsed.error :
                 game_command_parse_status_name(status));
        return true;
    }

    switch (parsed.ast.kind) {
    case GAME_COMMAND_KIND_TIME:
        if (parsed.ast.action == GAME_COMMAND_ACTION_SET && world_time) {
            *world_time = command_time_seconds(parsed.ast.value.time);
            chat_log(chat, "time: %s",
                     game_command_time_value_name(parsed.ast.value.time));
        }
        return true;
    case GAME_COMMAND_KIND_GAMEMODE: {
        PlayerMode mode;

        if (parsed.ast.action == GAME_COMMAND_ACTION_SET &&
            command_gamemode_to_player(parsed.ast.value.gamemode, &mode)) {
            player_set_mode(player, mode);
            chat_log(chat, "mode: %s", player_mode_name(player->mode));
        }
        return true;
    }
    case GAME_COMMAND_KIND_KILL:
        if (kill_requested)
            *kill_requested = true;
        return true;
    case GAME_COMMAND_KIND_HELP:
        chat_log(chat, "/time set day|night");
        chat_log(chat, "/gamemode set survival|creative|spectator");
        chat_log(chat, "/kill");
        return true;
    default:
        chat_log(chat, "unknown command");
        return true;
    }
}

static void draw_hotbar_digit(RenderContext *ctx, int digit,
                              float x, float y, uint8_t palette_index)
{
    if (digit < 1 || digit > HOTBAR_SLOT_COUNT)
        return;

    const uint8_t *rows = HOTBAR_DIGITS[digit - 1];
    const float px = HUD_SCALE;

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (rows[row] & (1u << (2 - col))) {
                renderer_fill_rect(ctx,
                                   x + (float)col * px,
                                   y + (float)row * px,
                                   x + (float)(col + 1) * px,
                                   y + (float)(row + 1) * px,
                                   palette_index,
                                   0);
            }
        }
    }
}

static void survival_inventory_layout(SurvivalInventoryLayout *layout)
{
    const float slot = HOTBAR_SLOT_PIXELS;
    const float gap = HOTBAR_GAP_PIXELS;
    const float storage_w =
        SURVIVAL_HOTBAR_SLOT_COUNT * slot +
        (SURVIVAL_HOTBAR_SLOT_COUNT - 1) * gap;
    const float panel_w = storage_w + 24.0f;
    const float panel_h = 272.0f;
    const float panel_x = floorf((SCREEN_WIDTH - panel_w) * 0.5f);
    const float panel_y = floorf((SCREEN_HEIGHT - panel_h) * 0.5f) - 10.0f;

    if (!layout)
        return;

    layout->panel_x = panel_x;
    layout->panel_y = panel_y;
    layout->panel_w = panel_w;
    layout->panel_h = panel_h;
    layout->slot = slot;
    layout->gap = gap;
    layout->craft_x = panel_x + 28.0f;
    layout->craft_y = panel_y + 24.0f;
    layout->output_x = layout->craft_x + 2.0f * (slot + gap) + 36.0f;
    layout->output_y = layout->craft_y + (slot + gap) * 0.5f;
    layout->main_x = panel_x + 12.0f;
    layout->main_y = panel_y + 102.0f;
    layout->hotbar_x = layout->main_x;
    layout->hotbar_y = layout->main_y + 3.0f * (slot + gap) + 16.0f;
}

static bool point_in_rect(float x, float y,
                          float x0, float y0,
                          float x1, float y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static InventoryHit survival_inventory_hit_test(float cursor_x, float cursor_y)
{
    SurvivalInventoryLayout layout;
    InventoryHit hit = { INVENTORY_SLOT_NONE, -1 };

    survival_inventory_layout(&layout);

    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            float x0 = layout.craft_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.craft_y + (layout.slot + layout.gap) * (float)row;

            if (point_in_rect(cursor_x, cursor_y,
                              x0, y0, x0 + layout.slot, y0 + layout.slot)) {
                hit.area = INVENTORY_SLOT_CRAFT;
                hit.index = row * 2 + col;
                return hit;
            }
        }
    }

    if (point_in_rect(cursor_x, cursor_y,
                      layout.output_x, layout.output_y,
                      layout.output_x + layout.slot,
                      layout.output_y + layout.slot)) {
        hit.area = INVENTORY_SLOT_OUTPUT;
        hit.index = 0;
        return hit;
    }

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
            float x0 = layout.main_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.main_y + (layout.slot + layout.gap) * (float)row;

            if (point_in_rect(cursor_x, cursor_y,
                              x0, y0, x0 + layout.slot, y0 + layout.slot)) {
                hit.area = INVENTORY_SLOT_STORAGE;
                hit.index = SURVIVAL_HOTBAR_SLOT_COUNT +
                            row * SURVIVAL_HOTBAR_SLOT_COUNT + col;
                return hit;
            }
        }
    }

    for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
        float x0 = layout.hotbar_x + (layout.slot + layout.gap) * (float)col;
        float y0 = layout.hotbar_y;

        if (point_in_rect(cursor_x, cursor_y,
                          x0, y0, x0 + layout.slot, y0 + layout.slot)) {
            hit.area = INVENTORY_SLOT_STORAGE;
            hit.index = col;
            return hit;
        }
    }

    return hit;
}

static void draw_item_stack_icon(RenderContext *ctx,
                                 const ItemStack *stack,
                                 float x0,
                                 float y0,
                                 float x1,
                                 float y1)
{
    if (!ctx || item_stack_is_empty(stack))
        return;

    renderer_draw_screen_tile(ctx,
                              x0 + 4.0f, y0 + 4.0f,
                              x1 - 4.0f, y1 - 4.0f,
                              item_texture_id(stack->item),
                              item_is_placeable_block(stack->item) &&
                              block_render_model(item_place_block(stack->item)) ==
                                  BLOCK_RENDER_CROSS ?
                                  QUAD_FLAG_ALPHA_KEY : 0);
    if (stack->count > 1) {
        char count[4];
        int len;
        float tx;
        float ty;

        snprintf(count, sizeof(count), "%u", (unsigned)stack->count);
        len = (int)strlen(count);
        tx = x1 - 2.0f - (float)(len * chat_font_cell_w());
        ty = y1 - 2.0f - (float)chat_font_cell_h();
        chat_draw_text(ctx, count, len, tx + 1.0f, ty + 1.0f, 0);
        chat_draw_text(ctx, count, len, tx, ty, 5);
    }
}

static void draw_inventory_slot(RenderContext *ctx,
                                const ItemStack *stack,
                                float x0,
                                float y0,
                                bool selected,
                                bool hovered)
{
    const float slot = HOTBAR_SLOT_PIXELS;
    uint8_t border = selected ? 8 : (hovered ? 5 : 14);

    renderer_fill_rect(ctx, x0, y0, x0 + slot, y0 + slot, border, 0);
    renderer_fill_rect(ctx,
                       x0 + 1.0f, y0 + 1.0f,
                       x0 + slot - 1.0f, y0 + slot - 1.0f,
                       0, QUAD_ALPHA_25);
    draw_item_stack_icon(ctx, stack, x0, y0, x0 + slot, y0 + slot);
}

static void draw_inventory_cursor(RenderContext *ctx,
                                  const ItemStack *cursor,
                                  float cursor_x,
                                  float cursor_y)
{
    const float s = HUD_SCALE;

    if (!item_stack_is_empty(cursor)) {
        draw_item_stack_icon(ctx, cursor,
                             cursor_x - HOTBAR_SLOT_PIXELS * 0.5f,
                             cursor_y - HOTBAR_SLOT_PIXELS * 0.5f,
                             cursor_x + HOTBAR_SLOT_PIXELS * 0.5f,
                             cursor_y + HOTBAR_SLOT_PIXELS * 0.5f);
    }

    renderer_fill_rect(ctx, cursor_x, cursor_y,
                       cursor_x + 7.0f * s, cursor_y + 1.0f * s,
                       5, 0);
    renderer_fill_rect(ctx, cursor_x, cursor_y,
                       cursor_x + 1.0f * s, cursor_y + 9.0f * s,
                       5, 0);
    renderer_fill_rect(ctx, cursor_x + 1.0f, cursor_y + 1.0f,
                       cursor_x + 7.0f * s + 1.0f,
                       cursor_y + 1.0f * s + 1.0f,
                       0, 0);
    renderer_fill_rect(ctx, cursor_x + 1.0f, cursor_y + 1.0f,
                       cursor_x + 1.0f * s + 1.0f,
                       cursor_y + 9.0f * s + 1.0f,
                       0, 0);
}

static void draw_survival_inventory(RenderContext *ctx,
                                    const SurvivalInventory *inventory,
                                    int selected_slot,
                                    float cursor_x,
                                    float cursor_y)
{
    SurvivalInventoryLayout layout;
    InventoryHit hover;

    if (!ctx || !inventory)
        return;

    survival_inventory_layout(&layout);
    hover = survival_inventory_hit_test(cursor_x, cursor_y);

    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       0, QUAD_ALPHA_50);
    renderer_fill_rect(ctx,
                       layout.panel_x, layout.panel_y,
                       layout.panel_x + layout.panel_w,
                       layout.panel_y + layout.panel_h,
                       14, 0);
    renderer_fill_rect(ctx,
                       layout.panel_x + 2.0f, layout.panel_y + 2.0f,
                       layout.panel_x + layout.panel_w - 2.0f,
                       layout.panel_y + layout.panel_h - 2.0f,
                       0, QUAD_ALPHA_25);

    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            int index = row * 2 + col;
            float x0 = layout.craft_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.craft_y + (layout.slot + layout.gap) * (float)row;
            bool hovered = hover.area == INVENTORY_SLOT_CRAFT &&
                           hover.index == index;

            draw_inventory_slot(ctx, &inventory->craft[index],
                                x0, y0, false, hovered);
        }
    }

    renderer_fill_rect(ctx,
                       layout.output_x - 24.0f,
                       layout.output_y + layout.slot * 0.5f - 2.0f,
                       layout.output_x - 8.0f,
                       layout.output_y + layout.slot * 0.5f + 2.0f,
                       5, 0);
    renderer_fill_rect(ctx,
                       layout.output_x - 10.0f,
                       layout.output_y + layout.slot * 0.5f - 6.0f,
                       layout.output_x - 6.0f,
                       layout.output_y + layout.slot * 0.5f + 6.0f,
                       5, 0);
    draw_inventory_slot(ctx, &inventory->craft_output,
                        layout.output_x, layout.output_y,
                        false,
                        hover.area == INVENTORY_SLOT_OUTPUT);

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
            int index = SURVIVAL_HOTBAR_SLOT_COUNT +
                        row * SURVIVAL_HOTBAR_SLOT_COUNT + col;
            float x0 = layout.main_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.main_y + (layout.slot + layout.gap) * (float)row;
            bool hovered = hover.area == INVENTORY_SLOT_STORAGE &&
                           hover.index == index;

            draw_inventory_slot(ctx, &inventory->storage[index],
                                x0, y0, false, hovered);
        }
    }

    for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
        float x0 = layout.hotbar_x + (layout.slot + layout.gap) * (float)col;
        bool hovered = hover.area == INVENTORY_SLOT_STORAGE &&
                       hover.index == col;

        draw_inventory_slot(ctx, &inventory->storage[col],
                            x0, layout.hotbar_y,
                            col == selected_slot, hovered);
        draw_hotbar_digit(ctx, col + 1, x0 + 2.0f, layout.hotbar_y + 2.0f,
                          col == selected_slot ? 8 : 5);
    }

    draw_inventory_cursor(ctx, &inventory->cursor, cursor_x, cursor_y);
}

static bool draw_screen_solid_quad(RenderContext *ctx,
                                   float x0, float y0,
                                   float x1, float y1,
                                   float x2, float y2,
                                   float x3, float y3,
                                   uint8_t palette_index)
{
    RenderQuad q = {0};

    q.color_tint = palette_index;
    q.vertices[0] = (Vertex2D){ x0, y0, 0.0f, 0.0f, 0.0f, 1.0f };
    q.vertices[1] = (Vertex2D){ x1, y1, 0.0f, 0.0f, 0.0f, 1.0f };
    q.vertices[2] = (Vertex2D){ x2, y2, 0.0f, 0.0f, 0.0f, 1.0f };
    q.vertices[3] = (Vertex2D){ x3, y3, 0.0f, 0.0f, 0.0f, 1.0f };
    return renderer_push_quad(ctx, &q);
}

static void hotbar_metrics(float *slot_left_out, float *slot_top_out,
                           float *slot_size_out, float *gap_out,
                           float *total_width_out)
{
    const float slot_size = HOTBAR_SLOT_PIXELS;
    const float gap = HOTBAR_GAP_PIXELS;
    const float total_width =
        HOTBAR_SLOT_COUNT * slot_size + (HOTBAR_SLOT_COUNT - 1) * gap;

    if (slot_left_out)
        *slot_left_out = floorf((SCREEN_WIDTH - total_width) * 0.5f);
    if (slot_top_out)
        *slot_top_out = SCREEN_HEIGHT - slot_size - HOTBAR_BOTTOM_MARGIN_PIXELS;
    if (slot_size_out)
        *slot_size_out = slot_size;
    if (gap_out)
        *gap_out = gap;
    if (total_width_out)
        *total_width_out = total_width;
}

static void draw_hotbar_page_pips(RenderContext *ctx, float slot_left,
                                  float slot_top, float total_width,
                                  int selected_page)
{
    const float s = HUD_SCALE;
    const float pip = 2.0f * s;
    const float gap = 2.0f * s;
    const float total = HOTBAR_PAGE_COUNT * pip + (HOTBAR_PAGE_COUNT - 1) * gap;
    float x = slot_left + (total_width - total) * 0.5f;
    float y = slot_top - 4.0f * s;

    for (int i = 0; i < HOTBAR_PAGE_COUNT; i++) {
        uint8_t color = (i == selected_page) ? 8 : 14;
        renderer_fill_rect(ctx, x, y, x + pip, y + pip, color, 0);
        x += pip + gap;
    }
}

static void draw_hotbar(RenderContext *ctx, int selected_slot,
                        int selected_page, PlayerMode mode,
                        const SurvivalInventory *survival_inventory)
{
    const float s = HUD_SCALE;
    float slot_left, slot_top, slot_size, gap, total_width;

    hotbar_metrics(&slot_left, &slot_top, &slot_size, &gap, &total_width);

    renderer_fill_rect(ctx,
                       slot_left - 2.0f * s, slot_top - 2.0f * s,
                       slot_left + total_width + 2.0f * s,
                       slot_top + slot_size + 2.0f * s,
                       14, 0); // Palette 14 is dark grey, good for background outline
    renderer_fill_rect(ctx,
                       slot_left - 1.0f * s, slot_top - 1.0f * s,
                       slot_left + total_width + 1.0f * s,
                       slot_top + slot_size + 1.0f * s,
                       0, 0); // Palette 0 is probably black or transparent bg

    for (int i = 0; i < HOTBAR_SLOT_COUNT; i++) {
        float x0 = slot_left + (slot_size + gap) * (float)i;
        float x1 = x0 + slot_size;
        float y0 = slot_top;
        float y1 = y0 + slot_size;
        uint8_t border = (i == selected_slot) ? 5 : 14; // 5 is white
        uint8_t number = (i == selected_slot) ? 8 : 5;

        renderer_fill_rect(ctx, x0, y0, x1, y1, border, 0);
        renderer_fill_rect(ctx, x0 + 1.0f, y0 + 1.0f,
                           x1 - 1.0f, y1 - 1.0f, 0, 0);

        if (mode == PLAYER_MODE_SURVIVAL && survival_inventory) {
            draw_item_stack_icon(ctx, &survival_inventory->storage[i],
                                 x0, y0, x1, y1);
        } else {
            BlockID block = hotbar_block_at(selected_page, i);

            if (block != BLOCK_AIR) {
                uint8_t flags =
                    block_render_model(block) == BLOCK_RENDER_CROSS ?
                    QUAD_FLAG_ALPHA_KEY : 0;

                renderer_draw_screen_tile(ctx,
                                          x0 + 3.0f, y0 + 3.0f,
                                          x1 - 3.0f, y1 - 3.0f,
                                          block_face_texture_id(block,
                                                               FACE_FRONT),
                                          flags);
            }
        }
        renderer_fill_rect(ctx, x0 + 1.0f, y0 + 1.0f,
                           x0 + 5.0f * s, y0 + 7.0f * s, 0, 0);
        draw_hotbar_digit(ctx, i + 1, x0 + 2.0f, y0 + 2.0f, number);

        if (i == selected_slot) {
            renderer_fill_rect(ctx,
                               x0 + 3.0f, y1 - 3.0f,
                               x1 - 3.0f, y1 - 1.0f,
                               8, 0);
        }
    }

    if (mode == PLAYER_MODE_CREATIVE)
        draw_hotbar_page_pips(ctx, slot_left, slot_top, total_width,
                              selected_page);
}

static void draw_healthbar(RenderContext *ctx, int health_units,
                           float damage_flash_timer)
{
    float slot_left, slot_top;
    const float icon = 8.0f * HUD_SCALE;
    const float step = 8.0f * HUD_SCALE;
    int full_hearts = health_units / 2;
    bool half_heart = (health_units & 1) != 0;
    bool blink = damage_flash_timer > 0.0f &&
        (((int)(damage_flash_timer * 16.0f)) & 1);

    hotbar_metrics(&slot_left, &slot_top, NULL, NULL, NULL);
    const float health_top = slot_top - icon - 7.0f;

    if (full_hearts < 0)
        full_hearts = 0;
    if (full_hearts > 10)
        full_hearts = 10;

    for (int i = 0; i < 10; i++) {
        float x0 = slot_left + (float)i * step;
        uint8_t tile;

        renderer_draw_screen_tile(ctx,
                                  x0, health_top,
                                  x0 + icon, health_top + icon,
                                  TEX_TILE_HEART_CONTAINER,
                                  QUAD_FLAG_ALPHA_KEY);

        if (i < full_hearts) {
            tile = blink ? TEX_TILE_HEART_BLINK : TEX_TILE_HEART;
        } else if (i == full_hearts && half_heart) {
            tile = blink ? TEX_TILE_HEART_HALF_BLINK : TEX_TILE_HEART_HALF;
        } else {
            continue;
        }

        renderer_draw_screen_tile(ctx,
                                  x0, health_top,
                                  x0 + icon, health_top + icon,
                                  tile, QUAD_FLAG_ALPHA_KEY);
    }
}

static void draw_hungerbar(RenderContext *ctx)
{
    float slot_left, slot_top, total_width;
    const float icon = 8.0f * HUD_SCALE;
    const float step = 8.0f * HUD_SCALE;

    hotbar_metrics(&slot_left, &slot_top, NULL, NULL, &total_width);
    const float health_top = slot_top - icon - 7.0f;
    const float hunger_right = slot_left + total_width;

    for (int i = 0; i < 10; i++) {
        float x0 = hunger_right - icon - (float)i * step;
        renderer_draw_screen_tile(ctx,
                                  x0, health_top,
                                  x0 + icon, health_top + icon,
                                  TEX_TILE_DRUMSTICK, QUAD_FLAG_ALPHA_KEY);
    }
}

static void draw_oxygenbar(RenderContext *ctx, float air_seconds)
{
    float slot_left, slot_top, total_width;
    const float icon = 8.0f * HUD_SCALE;
    const float step = 8.0f * HUD_SCALE;
    int full_bubbles;
    int shown_bubbles;

    if (air_seconds < 0.0f)
        air_seconds = 0.0f;
    if (air_seconds > PLAYER_MAX_AIR_SECONDS)
        air_seconds = PLAYER_MAX_AIR_SECONDS;

    full_bubbles = (int)ceilf(
        ((air_seconds - AIR_BUBBLE_POP_WINDOW_SECONDS) *
         (float)AIR_BUBBLE_COUNT) / PLAYER_MAX_AIR_SECONDS);
    shown_bubbles = (int)ceilf(
        (air_seconds * (float)AIR_BUBBLE_COUNT) / PLAYER_MAX_AIR_SECONDS);
    if (full_bubbles < 0)
        full_bubbles = 0;
    if (shown_bubbles < full_bubbles)
        shown_bubbles = full_bubbles;
    if (shown_bubbles > AIR_BUBBLE_COUNT)
        shown_bubbles = AIR_BUBBLE_COUNT;

    hotbar_metrics(&slot_left, &slot_top, NULL, NULL, &total_width);
    const float health_top = slot_top - 8.0f * HUD_SCALE - 7.0f;
    const float bubble_top = health_top - icon - 4.0f;
    const float right = slot_left + total_width;

    for (int i = 0; i < shown_bubbles; i++) {
        float x0 = right - icon - (float)i * step;
        uint8_t tile = (i < full_bubbles) ?
            TEX_TILE_AIR_BUBBLE : TEX_TILE_AIR_BUBBLE_POP;

        renderer_draw_screen_tile(ctx,
                                  x0, bubble_top,
                                  x0 + icon, bubble_top + icon,
                                  tile, QUAD_FLAG_ALPHA_KEY);
    }
}

/* Map a short input-driven timer to a single Minecraft-style swing curve.
 * Returns swing in [0,1] tracing 0 -> 1 -> 0 over HAND_SWING_SECONDS. */
static float hand_swing_phase(float swing_timer)
{
    float t = swing_timer / HAND_SWING_SECONDS;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 0.0f;
    return sinf(t * (float)M_PI);
}

/* Bare hand for survival: a stylized connected block-arm. The fist is chunky
 * and the forearm attaches under it, avoiding the long tapered stick look. */
static void draw_bare_hand(RenderContext *ctx, float swing_timer)
{
    const float s = HUD_SCALE;
    const uint8_t skin_light = 20;
    const uint8_t skin_mid = 15;
    const uint8_t skin_shadow = 19;

    float swing = hand_swing_phase(swing_timer);
    float tip_dx = -12.0f * s * swing;
    float tip_dy = 12.0f * s * swing;
    float base_dx = tip_dx * 0.35f;
    float base_dy = tip_dy * 0.30f;

    float fist_x = SCREEN_WIDTH - 78.0f * s + tip_dx;
    float fist_y = SCREEN_HEIGHT - 94.0f * s + tip_dy;

    float arm_tl_x = fist_x + 13.0f * s;
    float arm_tl_y = fist_y + 35.0f * s;
    float arm_tr_x = fist_x + 45.0f * s;
    float arm_tr_y = fist_y + 43.0f * s;
    float arm_br_x = SCREEN_WIDTH + 12.0f * s + base_dx;
    float arm_br_y = SCREEN_HEIGHT + 8.0f * s + base_dy;
    float arm_bl_x = SCREEN_WIDTH - 44.0f * s + base_dx;
    float arm_bl_y = SCREEN_HEIGHT + 8.0f * s + base_dy;

    /* Forearm behind the fist. */
    draw_screen_solid_quad(ctx,
        arm_tl_x, arm_tl_y,
        arm_tr_x, arm_tr_y,
        arm_br_x, arm_br_y,
        arm_bl_x, arm_bl_y,
        skin_mid);

    draw_screen_solid_quad(ctx,
        arm_tl_x, arm_tl_y,
        arm_tl_x + 7.0f * s, arm_tl_y + 1.5f * s,
        arm_bl_x + 8.0f * s, arm_bl_y,
        arm_bl_x, arm_bl_y,
        skin_light);

    draw_screen_solid_quad(ctx,
        arm_tr_x - 7.0f * s, arm_tr_y - 1.5f * s,
        arm_tr_x, arm_tr_y,
        arm_br_x, arm_br_y,
        arm_br_x - 13.0f * s, arm_br_y,
        skin_shadow);

    /* Fist front face. */
    draw_screen_solid_quad(ctx,
        fist_x, fist_y,
        fist_x + 37.0f * s, fist_y + 8.0f * s,
        fist_x + 40.0f * s, fist_y + 41.0f * s,
        fist_x + 1.0f * s, fist_y + 35.0f * s,
        skin_mid);

    /* Left highlight and right shadow on the fist keep it blocky but readable. */
    draw_screen_solid_quad(ctx,
        fist_x, fist_y,
        fist_x + 9.0f * s, fist_y + 2.0f * s,
        fist_x + 9.0f * s, fist_y + 34.0f * s,
        fist_x + 1.0f * s, fist_y + 35.0f * s,
        skin_light);

    draw_screen_solid_quad(ctx,
        fist_x + 31.0f * s, fist_y + 6.5f * s,
        fist_x + 37.0f * s, fist_y + 8.0f * s,
        fist_x + 40.0f * s, fist_y + 41.0f * s,
        fist_x + 32.0f * s, fist_y + 40.0f * s,
        skin_shadow);
}

/* Block-in-hand for creative: isometric cube tilted into the bottom-right.
 * The swing translates the cube down and adds a small clockwise tilt by
 * shifting the diamond's top apex, mimicking the wrist-rotation arc you see
 * in Minecraft when you punch with a held block. */
static void draw_block_in_hand(RenderContext *ctx, BlockID type, float swing_timer)
{
    if (type == BLOCK_AIR)
        return;

    const float s = HUD_SCALE;
    float swing = hand_swing_phase(swing_timer);

    if (block_render_model(type) == BLOCK_RENDER_CROSS) {
        float size = 22.0f * s;
        float x0 = SCREEN_WIDTH - 42.0f * s - swing * 7.0f * s;
        float y0 = SCREEN_HEIGHT - 58.0f * s + swing * 12.0f * s;

        renderer_draw_custom_screen_quad(ctx,
            x0,                 y0,
            x0 + size,          y0 + 3.0f * s,
            x0 + size * 0.85f,  y0 + size + 7.0f * s,
            x0 - size * 0.15f,  y0 + size + 4.0f * s,
            block_face_texture_id(type, FACE_FRONT),
            QUAD_FLAG_ALPHA_KEY);
        return;
    }

    /* Half-widths of the isometric cube. The diamond top spans (-w,+w) and
     * (-h,+h) around the center; h2 is the side-face vertical extent. */
    const float w  = 24.0f * s;
    const float h  = 12.0f * s;
    const float h2 = 24.0f * s;
    const float margin_right  = 14.0f * s;
    const float margin_bottom = 14.0f * s;

    /* Translate down and slightly forward (right). Tilt: a small clockwise
     * rotation of the top apex (left + down) gives the cube the "wrist
     * swinging through" silhouette without needing real 2D rotation. */
    float swing_y = swing * 16.0f * s;
    float swing_x = swing * 4.0f * s;
    float tilt    = swing * 6.0f * s;

    float cx = SCREEN_WIDTH  - w  - margin_right  + swing_x;
    float cy = SCREEN_HEIGHT - h2 - margin_bottom + swing_y;

    /* Top diamond. Vertex order top -> right -> bottom -> left with the
     * UV winding hardcoded in renderer.c rotates the texture 45 deg CW
     * into the diamond shape. The tilt biases the top and right apex. */
    renderer_draw_custom_screen_quad(ctx,
        cx - tilt,        cy - h + tilt * 0.3f,  /* top apex (rotated CW) */
        cx + w,           cy - tilt * 0.3f,      /* right apex             */
        cx + tilt,        cy + h - tilt * 0.3f,  /* bottom apex            */
        cx - w,           cy + tilt * 0.3f,      /* left apex              */
        block_face_texture_id(type, FACE_TOP), 0);

    /* Right side face, TL,TR,BR,BL to match UV(0,0)(16,0)(16,16)(0,16). */
    renderer_draw_custom_screen_quad(ctx,
        cx + tilt,        cy + h - tilt * 0.3f,  /* TL */
        cx + w,           cy - tilt * 0.3f,      /* TR */
        cx + w,           cy + h2,               /* BR */
        cx + tilt,        cy + h + h2,           /* BL */
        block_face_texture_id(type, FACE_RIGHT), QUAD_LIGHT_LEVEL(2));

    /* Left/front face, same TL,TR,BR,BL winding. */
    renderer_draw_custom_screen_quad(ctx,
        cx - w,           cy + tilt * 0.3f,      /* TL */
        cx + tilt,        cy + h - tilt * 0.3f,  /* TR */
        cx + tilt,        cy + h + h2,           /* BR */
        cx - w,           cy + h2,               /* BL */
        block_face_texture_id(type, FACE_FRONT), QUAD_LIGHT_LEVEL(1));
}

/* Debug HUD: player position, chunk coords, render distance, loaded chunks.
 * Toggled with F3 or VOXEL_DEBUG_HUD=1. Drawn top-left below the FPS counter
 * using the same drop-shadow text style. Also draws 3D chunk border lines. */
static void draw_debug_hud(RenderContext *ctx, const Player *player,
                           const Camera *cam, const VoxelWorld *world)
{
    char lines[9][56];
    int line_count = 0;
    int cell_h = chat_font_cell_h();
    int line_step = cell_h + 2;
    int wx = (int)floorf(player->x);
    int wz = (int)floorf(player->z);
    int chunk_x = (int)floorf(player->x / (float)WORLD_CHUNK_SIZE);
    int chunk_z = (int)floorf(player->z / (float)WORLD_CHUNK_SIZE);
    WorldBiome biome = world_biome_at(world, wx, wz);

    /* Block position within the current chunk (0..15 in each axis). */
    float local_x = player->x - (float)(chunk_x * WORLD_CHUNK_SIZE);
    float local_z = player->z - (float)(chunk_z * WORLD_CHUNK_SIZE);

    snprintf(lines[line_count++], sizeof(lines[0]),
             "XYZ: %.1f / %.1f / %.1f", player->x, player->y, player->z);
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Chunk: %d, %d  [%.1f, %.1f]", chunk_x, chunk_z,
             local_x, local_z);
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Biome: %s", world_biome_name(biome));
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Facing: yaw=%.1f pitch=%.1f",
             cam->yaw * 180.0f / (float)M_PI,
             cam->pitch * 180.0f / (float)M_PI);
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Render dist: %d  Near: %d",
             world_render_distance(world),
             world_near_chunk_radius(world));
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Loaded: %d / %d chunks",
             world_loaded_chunk_count(world),
             world_chunk_capacity(world));

    /* Position below the enlarged FPS counter (top-left, same x=12). */
    float y = 12.0f + (float)(chat_font_cell_h() * HUD_SCALE_I) + 4.0f;
    for (int i = 0; i < line_count; i++) {
        int len = (int)strlen(lines[i]);
        /* Drop shadow + foreground, left-aligned under FPS */
        chat_draw_text(ctx, lines[i], len, 13.0f, y + 1.0f, 0);
        chat_draw_text(ctx, lines[i], len, 12.0f, y, 5);
        y += (float)line_step;
    }

    /* Draw 3D chunk border lines in the world. */
    renderer_draw_chunk_borders(ctx, player->x, player->z,
                                world_render_distance(world));
}

int main(void)
{
    int debug_enabled = read_debug_enabled();
    thread_affinity_pin_current("main", "VOXEL_MAIN_CPU", 0);

    RenderContext *ctx = renderer_init();
    VoxelWorld world;
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    InputState inp;
    input_init(&inp);

    Chat chat;
    chat_init(&chat);

    PauseMenu pause;
    pause_menu_init(&pause);

    init_block_types();
    world_init(&world);
    float mouse_sens = read_mouse_sensitivity();
    int target_fps = read_target_fps();
    long frame_ns = 1000000000L / target_fps;
    int render_distance_chunks = read_render_distance_chunks();
    int stream_chunks_per_frame = read_stream_chunks_per_frame();
    int max_physics_steps_per_frame = read_max_physics_steps_per_frame();
    SelectedWorld selected_world = {0};
    int selected_hotbar_slot = 0;
    int selected_hotbar_page = 0;
    SurvivalInventory survival_inventory;
    ItemEntityPool item_drops;
    bool inventory_open = false;
    float inventory_cursor_x = SCREEN_WIDTH * 0.5f;
    float inventory_cursor_y = SCREEN_HEIGHT * 0.5f;
    PauseMenuSettings pause_settings = {0};

    survival_inventory_init(&survival_inventory);
    item_entities_init(&item_drops);

    if (!run_home_menu(ctx, &inp, target_fps, &selected_world)) {
        input_shutdown(&inp);
        world_free(&world);
        renderer_shutdown(ctx);
        return inp.quit ? 0 : 1;
    }

    /* Initialize Player */
    Player player;
    /* Spawn above the tallest possible heightmap surface + tree canopy so
     * the player drops cleanly onto whatever terrain happens to be at the
     * origin column - tall hills, beaches, or treetops. */
    player_init(&player, PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z);

    Camera cam = {
        .position = { player.x, player_get_eye_height(&player), player.z },
        .pitch    = -0.3f,  /* negative pitch looks down in renderer.c */
        .yaw      = 0.0f,
        .depth    = compute_camera_focal_px(),
    };

    if (!world_init_infinite_procedural(&world,
                                        selected_world.seed,
                                        selected_world.stone_tries_per_chunk,
                                        render_distance_chunks,
                                        player.x,
                                        player.z,
                                        selected_world.path)) {
        fprintf(stderr, "world generation failed\n");
        input_shutdown(&inp);
        world_free(&world);
        renderer_shutdown(ctx);
        return 1;
    }
    world_set_stream_chunks_per_frame(&world, stream_chunks_per_frame);

    pause_settings.stream_chunks_per_frame = world_stream_chunks_per_frame(&world);
    pause_settings.stream_chunks_per_frame_max = MAX_STREAM_CHUNKS_PER_FRAME;
    pause_settings.near_chunk_radius = world_near_chunk_radius(&world);
    pause_settings.near_chunk_radius_max = world_render_distance(&world);
    pause_settings.render_distance = world_render_distance(&world);
    pause_settings.render_distance_max = MAX_WORLD_RENDER_DISTANCE_CHUNKS;

    bool mesh_worker_running = mesh_worker_start(&world);
    bool gen_worker_running = gen_worker_start(&world);

    if (debug_enabled) {
        printf("Controls: WASD=move  double-tap W=sprint  Space=jump/fly-up  double-tap Space=creative flight  Shift=crouch/fly-down  1-9=hotbar  Tab=hotbar page  F/LMB=break  R/RMB=place  E=inventory  G=cycle mode  T=chat  Esc=pause/release mouse  Q=quit\n");
        printf("Mode: %s (survival=gravity+collision, creative=build+toggle-flight, spectator=fly+no-collision)\n",
               player_mode_name(player.mode));
        printf("World: %s, %dx%dx%d chunks, seed 0x%08x\n",
               selected_world.name,
               WORLD_CHUNK_SIZE, WORLD_CHUNK_HEIGHT, WORLD_CHUNK_SIZE,
               selected_world.seed);
        printf("World save dir: %s\n", selected_world.path);
        printf("Loaded window: %dx%d chunks around player (%d-chunk render radius + 1 border, capacity=%d)\n",
               world.chunks_x, world.chunks_z, world_render_distance(&world),
               world_chunk_capacity(&world));
        printf("Cached loaded world: chunks=%d blocks=%d exposed_faces=%d generated=%d mesh_rebuilds=%d\n",
               world_loaded_chunk_count(&world),
               world_total_blocks(&world), world_total_faces(&world),
               world.chunks_generated_last_stream,
               world.meshes_rebuilt_last_stream);
        printf("Mouse sensitivity: %.4f rad/input (set VOXEL_MOUSE_SENS to override)\n",
               mouse_sens);
        printf("Frame cap: %d FPS (set VOXEL_TARGET_FPS=%d-%d to override)\n",
               target_fps, MIN_TARGET_FPS, MAX_TARGET_FPS);
        printf("Streaming: chunks_per_frame=%d near_mesh_radius=%d\n",
               world_stream_chunks_per_frame(&world),
               world_near_chunk_radius(&world));
        printf("Physics: max_steps_per_frame=%d (0=unlimited, env VOXEL_MAX_PHYSICS_STEPS_PER_FRAME)\n",
               max_physics_steps_per_frame);
        printf("Mesh worker: %s\n", mesh_worker_running ? "on" : "off");
        printf("Gen worker: %s\n", gen_worker_running ? "on" : "off");
    }

    struct timespec prev, now, frame_end;
    struct timespec perf_window_start;
    float world_time = 0.0f;
    float physics_accumulator = 0.0f;
    float environment_tick_accumulator = 0.0f;
    float break_timer = 0.0f;
    float break_duration = 0.0f;
    float hand_swing_timer = HAND_SWING_SECONDS;
    int player_health_units = PLAYER_MAX_HEALTH_UNITS;
    float player_air_seconds = PLAYER_MAX_AIR_SECONDS;
    float drown_timer = 0.0f;
    float damage_flash_timer = 0.0f;
    bool creative_flight_enabled = false;
    float creative_jump_tap_timer = 0.0f;
    PlayerMode last_player_mode = player.mode;
    bool fall_tracking = false;
    bool fall_damage_armed = false;
    float fall_start_y = player.y;
    BlockTarget break_target = {0};
    bool break_target_valid = false;
#define ENVIRONMENT_TICK_INTERVAL 0.75f  /* One fluid/gravity step every 750 ms. */
#define RESET_PLAYER_AFTER_DEATH(message) do { \
        respawn_player(&player, &cam); \
        player_health_units = PLAYER_MAX_HEALTH_UNITS; \
        player_air_seconds = PLAYER_MAX_AIR_SECONDS; \
        drown_timer = 0.0f; \
        damage_flash_timer = 0.0f; \
        physics_accumulator = 0.0f; \
        break_timer = 0.0f; \
        break_duration = 0.0f; \
        break_target_valid = false; \
        hand_swing_timer = HAND_SWING_SECONDS; \
        creative_flight_enabled = false; \
        creative_jump_tap_timer = 0.0f; \
        fall_tracking = false; \
        fall_damage_armed = false; \
        fall_start_y = player.y; \
        last_player_mode = player.mode; \
        chat_log(&chat, "%s", (message)); \
    } while (0)
    int perf_frames = 0;
    int perf_quads = 0;
    int perf_sky_quads = 0;
    int perf_physics_steps = 0;
    double perf_update_ns = 0.0;
    double perf_begin_ns = 0.0;
    double perf_draw_ns = 0.0;
    double perf_end_ns = 0.0;
    double perf_sleep_ns = 0.0;
    double perf_work_ns = 0.0;
    double perf_max_work_ns = 0.0;
    double perf_physics_ns = 0.0;
    double perf_stream_ns = 0.0;
    double perf_stream_wait_ns = 0.0;
    double perf_stream_body_ns = 0.0;
    double perf_lighting_ns = 0.0;
    double perf_gen_drain_ns = 0.0;
    double perf_mesh_drain_ns = 0.0;
    int perf_physics_dropped_steps = 0;
    int status_log_enabled = read_status_log_enabled(debug_enabled);
    int debug_hud_enabled = read_debug_hud_enabled();
    char fps_text[16] = "fps --";
    int fps_text_len = (int)strlen(fps_text);
    clock_gettime(CLOCK_MONOTONIC, &prev);
    perf_window_start = prev;

    while (!inp.quit) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct timespec loop_start = now;
        float frame_dt = (float)ns_diff(&now, &prev) / 1e9f;
        if (frame_dt > MAX_FRAME_DT)
            frame_dt = MAX_FRAME_DT;
        prev = now;
        world_time += frame_dt;
        physics_accumulator += frame_dt;
        environment_tick_accumulator += frame_dt;

        /* Minecraft-style environment simulation: intentionally slower than
         * placement so fluids and falling blocks move visibly. */
        if (environment_tick_accumulator >= ENVIRONMENT_TICK_INTERVAL) {
            environment_tick_accumulator -= ENVIRONMENT_TICK_INTERVAL;
            world_water_tick(&world);
            if (debug_enabled) {
                WaterTickStats ws = world_water_tick_stats();
                if (ws.sources_seen || ws.flows_seen || ws.spread_placed ||
                    ws.evaporated || ws.falling_moved) {
                    chat_log(&chat, "sim: src=%d flow=%d +%d -%d fall=%d",
                             ws.sources_seen, ws.flows_seen,
                             ws.spread_placed, ws.evaporated,
                             ws.falling_moved);
                }
            }
        }
        world_update_falling_blocks(&world, frame_dt);

        input_update(&inp);

        chat_tick(&chat, frame_dt);
        if (damage_flash_timer > 0.0f) {
            damage_flash_timer -= frame_dt;
            if (damage_flash_timer < 0.0f)
                damage_flash_timer = 0.0f;
        }
        if (creative_jump_tap_timer > 0.0f) {
            creative_jump_tap_timer -= frame_dt;
            if (creative_jump_tap_timer < 0.0f)
                creative_jump_tap_timer = 0.0f;
        }

        /* ESC: close chat first, otherwise toggle the pause overlay. The
         * chat branch already consumes ESC while text mode is on, so any
         * pause_toggle event that reaches us here is coming from the
         * game view. */
        if (input_consume_pause_toggle(&inp)) {
            if (inventory_open) {
                close_survival_inventory(&survival_inventory, &item_drops,
                                         &player, &inventory_open);
            } else if (!chat_is_open(&chat)) {
                pause_menu_toggle(&pause);
            }
        }
        if (input_consume_debug_hud_toggle(&inp))
            debug_hud_enabled = !debug_hud_enabled;

        bool paused = pause_menu_is_open(&pause);
        if (paused && inventory_open)
            close_survival_inventory(&survival_inventory, &item_drops,
                                     &player, &inventory_open);
        if (paused && pause_menu_update(&pause, &inp, &pause_settings)) {
            world_set_stream_chunks_per_frame(&world,
                                              pause_settings.stream_chunks_per_frame);
            world_set_near_chunk_radius(&world,
                                        pause_settings.near_chunk_radius);
            world_set_render_distance(&world,
                                      pause_settings.render_distance);
            pause_settings.stream_chunks_per_frame =
                world_stream_chunks_per_frame(&world);
            pause_settings.near_chunk_radius =
                world_near_chunk_radius(&world);
            pause_settings.render_distance =
                world_render_distance(&world);
            /* near_chunk_radius_max tracks the current render distance */
            pause_settings.near_chunk_radius_max =
                world_render_distance(&world);
        }

        if (input_consume_inventory_toggle(&inp) &&
            !paused && !chat_is_open(&chat) &&
            player.mode == PLAYER_MODE_SURVIVAL) {
            if (inventory_open) {
                close_survival_inventory(&survival_inventory, &item_drops,
                                         &player, &inventory_open);
            } else {
                inventory_open = true;
                inventory_cursor_x = SCREEN_WIDTH * 0.5f;
                inventory_cursor_y = SCREEN_HEIGHT * 0.5f;
            }
        }

        /* Chat toggle: ignored while paused or inventory is open — those
         * overlays own the screen. */
        if (input_consume_chat_toggle(&inp) && !paused && !inventory_open) {
            chat_toggle(&chat);
            input_set_text_mode(&inp, chat_is_open(&chat));
        }

        bool chat_open = chat_is_open(&chat);
        bool kill_requested = false;
        input_set_pointer_capture(&inp, !paused && !chat_open && !inventory_open);

        if (chat_open && !paused) {
            for (int i = 0; i < inp.text_queue_len; i++) {
                char ch = inp.text_queue[i];
                if (ch == INPUT_TEXT_BACKSPACE)
                    chat_handle_backspace(&chat);
                else if (ch == INPUT_TEXT_ENTER) {
                    char submitted[CHAT_LINE_MAX + 1];

                    snprintf(submitted, sizeof(submitted), "%s", chat.input);
                    chat_handle_enter(&chat);
                    execute_chat_command(&chat, &player, &world_time,
                                         &kill_requested, submitted);
                } else {
                    chat_handle_char(&chat, ch);
                }
            }
        }
        input_clear_text_queue(&inp);
        if (kill_requested)
            RESET_PLAYER_AFTER_DEATH("killed");

        if (input_consume_mode_toggle(&inp) && !paused) {
            player_cycle_mode(&player);
            chat_log(&chat, "mode: %s", player_mode_name(player.mode));
        }

        if (player.mode != last_player_mode) {
            creative_flight_enabled = false;
            creative_jump_tap_timer = 0.0f;
            fall_tracking = false;
            fall_start_y = player.y;
            if (player.mode != PLAYER_MODE_SURVIVAL)
                fall_damage_armed = false;
            if (player.mode != PLAYER_MODE_SURVIVAL && inventory_open)
                close_survival_inventory(&survival_inventory, &item_drops,
                                         &player, &inventory_open);
            last_player_mode = player.mode;
        }

        /* Look: mouse + arrow keys. Muted while paused so the camera
         * stays still in the pause view, but we still drain mouse state
         * so a held mouse doesn't accumulate deltas across the pause. */
        if (inventory_open) {
            inventory_cursor_x += inp.cursor_dx * INVENTORY_CURSOR_SPEED;
            inventory_cursor_y += inp.cursor_dy * INVENTORY_CURSOR_SPEED;
            if (inventory_cursor_x < INVENTORY_CURSOR_MIN_X)
                inventory_cursor_x = INVENTORY_CURSOR_MIN_X;
            if (inventory_cursor_y < INVENTORY_CURSOR_MIN_Y)
                inventory_cursor_y = INVENTORY_CURSOR_MIN_Y;
            if (inventory_cursor_x > SCREEN_WIDTH - 1.0f)
                inventory_cursor_x = SCREEN_WIDTH - 1.0f;
            if (inventory_cursor_y > SCREEN_HEIGHT - 1.0f)
                inventory_cursor_y = SCREEN_HEIGHT - 1.0f;
        } else if (!paused && !chat_open) {
            cam.yaw   += inp.mouse_dx * mouse_sens;
            cam.pitch -= inp.mouse_dy * mouse_sens;
            if (inp.look_right) cam.yaw   += LOOK_SPEED * frame_dt;
            if (inp.look_left)  cam.yaw   -= LOOK_SPEED * frame_dt;
            if (inp.look_down)  cam.pitch += LOOK_SPEED * frame_dt;
            if (inp.look_up)    cam.pitch -= LOOK_SPEED * frame_dt;
            if (cam.pitch >  PITCH_LIMIT) cam.pitch =  PITCH_LIMIT;
            if (cam.pitch < -PITCH_LIMIT) cam.pitch = -PITCH_LIMIT;
        }
        input_clear_mouse(&inp);

        /* Determine walking direction vector from camera yaw */
        float fwd_x =  sinf(cam.yaw), fwd_z = cosf(cam.yaw);
        float rgt_x =  cosf(cam.yaw), rgt_z = -sinf(cam.yaw);

        float wish_x = 0.0f;
        float wish_z = 0.0f;

        /* Movement inputs are silently dropped while paused — the player
         * coasts to a stop through the normal friction path. */
        if (!paused && !inventory_open && !chat_open) {
            if (inp.forward) { wish_x += fwd_x; wish_z += fwd_z; }
            if (inp.back)    { wish_x -= fwd_x; wish_z -= fwd_z; }
            if (inp.right)   { wish_x += rgt_x; wish_z += rgt_z; }
            if (inp.left)    { wish_x -= rgt_x; wish_z -= rgt_z; }
        }

        /* Normalize wish direction so diagonal movement isn't faster */
        float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
        if (wish_len > 0.0f) {
            wish_x /= wish_len;
            wish_z /= wish_len;
        }

        int physics_steps = 0;
        bool jump_pressed = false;
        if (physics_accumulator >= PHYSICS_DT)
            jump_pressed = input_consume_jump(&inp);
        if (paused || inventory_open || chat_open)
            jump_pressed = false;

        if (!paused && !inventory_open && !chat_open &&
            player.mode == PLAYER_MODE_CREATIVE && jump_pressed) {
            if (creative_jump_tap_timer > 0.0f) {
                creative_flight_enabled = !creative_flight_enabled;
                creative_jump_tap_timer = 0.0f;
                player.vy = 0.0f;
                player.is_grounded = false;
                fall_tracking = false;
                fall_start_y = player.y;
                jump_pressed = false;
            } else {
                creative_jump_tap_timer = CREATIVE_FLIGHT_DOUBLE_TAP_SECONDS;
            }
        }

        /* In fly modes Space/Shift are held to ascend/descend, so pass the
         * held state rather than the edge-triggered jump. Creative toggles
         * this with double-Space; spectator always flies. */
        bool flight_controls = (player.mode == PLAYER_MODE_SPECTATOR) ||
            (player.mode == PLAYER_MODE_CREATIVE && creative_flight_enabled);
        bool controls_blocked = paused || inventory_open || chat_open;
        bool up_input    = controls_blocked ? false : inp.up;
        bool down_input  = controls_blocked ? false : inp.down;
        bool sprint_input = controls_blocked ? false : inp.sprint;

        struct timespec physics_start, physics_end;
        clock_gettime(CLOCK_MONOTONIC, &physics_start);
        while (physics_accumulator >= PHYSICS_DT &&
               (max_physics_steps_per_frame <= 0 ||
                physics_steps < max_physics_steps_per_frame)) {
            bool jump_input = flight_controls ? up_input : jump_pressed;
            bool was_grounded = player.is_grounded;
            float before_y = player.y;
            player_update(&player, &world, wish_x, wish_z,
                          jump_input, up_input, down_input, sprint_input,
                          flight_controls, PHYSICS_DT);

            if (!paused && player.mode == PLAYER_MODE_SURVIVAL) {
                if (player.is_in_water) {
                    fall_tracking = false;
                    fall_start_y = player.y;
                } else if (player.is_grounded) {
                    if (fall_tracking && !was_grounded && fall_damage_armed) {
                        float fall_distance = fall_start_y - player.y;
                        float damage_distance =
                            fall_distance - FALL_DAMAGE_SAFE_DISTANCE;

                        if (damage_distance > FALL_DAMAGE_EPSILON) {
                            int fall_damage = (int)ceilf(
                                damage_distance * FALL_DAMAGE_MULTIPLIER);
                            if (apply_survival_damage(&player_health_units,
                                                      fall_damage,
                                                      &damage_flash_timer)) {
                                RESET_PLAYER_AFTER_DEATH("fell from a high place");
                                break;
                            }
                        }
                    }
                    fall_tracking = false;
                    fall_start_y = player.y;
                    fall_damage_armed = true;
                } else {
                    if (!fall_tracking || was_grounded)
                        fall_start_y = before_y;
                    if (player.y > fall_start_y)
                        fall_start_y = player.y;
                    fall_tracking = true;
                }
            } else {
                fall_tracking = false;
                fall_start_y = player.y;
            }

            jump_pressed = false;
            physics_accumulator -= PHYSICS_DT;
            physics_steps++;
        }
        int dropped_physics_steps = 0;
        if (max_physics_steps_per_frame > 0 &&
            physics_accumulator >= PHYSICS_DT) {
            dropped_physics_steps = (int)(physics_accumulator / PHYSICS_DT);
            physics_accumulator = 0.0f;
        }
        clock_gettime(CLOCK_MONOTONIC, &physics_end);

        /* Sync Camera to Player's updated physical position */
        cam.position.x = player.x;
        cam.position.y = player_get_eye_height(&player);
        cam.position.z = player.z;

        struct timespec stream_start, stream_end;
        clock_gettime(CLOCK_MONOTONIC, &stream_start);
        if (!world_stream_around(&world, player.x, player.z)) {
            fprintf(stderr, "\nchunk streaming failed\n");
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &stream_end);

        bool camera_underwater = camera_is_underwater(&world, &cam);
        if (!paused && player.mode == PLAYER_MODE_SURVIVAL) {
            if (camera_underwater) {
                player_air_seconds -= frame_dt;
                if (player_air_seconds < 0.0f)
                    player_air_seconds = 0.0f;

                if (player_air_seconds <= 0.0f) {
                    drown_timer += frame_dt;
                    while (drown_timer >= DROWN_DAMAGE_INTERVAL_SECONDS) {
                        drown_timer -= DROWN_DAMAGE_INTERVAL_SECONDS;
                        if (apply_survival_damage(&player_health_units,
                                                  DROWN_DAMAGE_UNITS,
                                                  &damage_flash_timer)) {
                            RESET_PLAYER_AFTER_DEATH("drowned");
                            camera_underwater = false;
                            break;
                        }
                    }
                } else {
                    drown_timer = 0.0f;
                }
            } else {
                player_air_seconds += PLAYER_AIR_REFILL_PER_SECOND * frame_dt;
                if (player_air_seconds > PLAYER_MAX_AIR_SECONDS)
                    player_air_seconds = PLAYER_MAX_AIR_SECONDS;
                drown_timer = 0.0f;
            }
        } else if (player.mode != PLAYER_MODE_SURVIVAL) {
            player_air_seconds = PLAYER_MAX_AIR_SECONDS;
            drown_timer = 0.0f;
        }

        if (player.mode == PLAYER_MODE_SURVIVAL)
            item_entities_update(&item_drops, &world, &survival_inventory,
                                 &player, frame_dt);

        if (!paused && !chat_is_open(&chat) && inventory_open) {
            bool left_click = input_consume_break(&inp);
            bool right_click = input_consume_place(&inp);
            InventoryHit hit = survival_inventory_hit_test(inventory_cursor_x,
                                                           inventory_cursor_y);

            break_timer = 0.0f;
            break_duration = 0.0f;
            break_target_valid = false;
            hand_swing_timer = HAND_SWING_SECONDS;
            (void)input_consume_hotbar_slot(&inp);
            (void)input_consume_hotbar_page(&inp);

            if ((left_click || right_click) && hit.area != INVENTORY_SLOT_NONE)
                survival_inventory_click(&survival_inventory,
                                         hit.area, hit.index, right_click);
            inp.break_down = false;
        } else if (!paused && !chat_is_open(&chat) && !inventory_open) {
            int hotbar_slot = input_consume_hotbar_slot(&inp);
            bool hotbar_page_pressed = input_consume_hotbar_page(&inp);
            bool break_pressed = input_consume_break(&inp);
            bool place_pressed = input_consume_place(&inp);

            if (break_pressed || place_pressed)
                hand_swing_timer = 0.0f;

            if (hotbar_page_pressed &&
                player.mode == PLAYER_MODE_CREATIVE) {
                selected_hotbar_page =
                    (selected_hotbar_page + 1) % HOTBAR_PAGE_COUNT;
                BlockID selected = hotbar_block_at(selected_hotbar_page,
                                                   selected_hotbar_slot);
                chat_log(&chat, "hotbar page: %d  selected: %d %s",
                         selected_hotbar_page + 1,
                         selected_hotbar_slot + 1,
                         BlockRegistry[selected].name);
            }

            if (hotbar_slot >= 0 && hotbar_slot < HOTBAR_SLOT_COUNT &&
                hotbar_slot != selected_hotbar_slot) {
                selected_hotbar_slot = hotbar_slot;
                if (player.mode == PLAYER_MODE_SURVIVAL) {
                    const ItemStack *stack =
                        survival_inventory_hotbar_stack(&survival_inventory,
                                                        selected_hotbar_slot);

                    if (item_stack_is_empty(stack)) {
                        chat_log(&chat, "selected: %d empty",
                                 selected_hotbar_slot + 1);
                    } else {
                        chat_log(&chat, "selected: %d %s x%u",
                                 selected_hotbar_slot + 1,
                                 item_name(stack->item),
                                 (unsigned)stack->count);
                    }
                } else {
                    BlockID selected = hotbar_block_at(selected_hotbar_page,
                                                       selected_hotbar_slot);
                    chat_log(&chat, "selected: %d %s",
                             selected_hotbar_slot + 1,
                             BlockRegistry[selected].name);
                }
            }

            if (player.mode == PLAYER_MODE_CREATIVE) {
                if (inp.break_down)
                    break_timer += frame_dt;
                else
                    break_timer = 0.0f;

                if (break_pressed ||
                    (inp.break_down &&
                     break_timer >= CREATIVE_BLOCK_BREAK_REPEAT_SECONDS)) {
                    try_break_targeted_block(&world, &cam);
                    hand_swing_timer = 0.0f;
                    break_timer = 0.0f;
                }
                break_duration = 0.0f;
                break_target_valid = false;
            } else {
                if (inp.break_down) {
                    BlockTarget target = {0};
                    if (trace_target_block(&world, &cam, BLOCK_REACH_DISTANCE, &target)) {
                        BlockID target_block = world_get_block(&world,
                                                               target.hit_x,
                                                               target.hit_y,
                                                               target.hit_z);
                        float target_break_duration = block_break_seconds(target_block);
                        bool same_target =
                            break_target_valid &&
                            target.hit_x == break_target.hit_x &&
                            target.hit_y == break_target.hit_y &&
                            target.hit_z == break_target.hit_z;

                        if (target_break_duration > 0.0f) {
                            if (!same_target) {
                                break_target = target;
                                break_target_valid = true;
                                break_timer = 0.0f;
                            }
                            break_duration = target_break_duration;
                            break_timer += frame_dt;
                            if (hand_swing_timer >= HAND_SWING_SECONDS)
                                hand_swing_timer = 0.0f;
                            if (break_timer >= break_duration) {
                                BlockID broken = BLOCK_AIR;

                                if (break_block_target(&world, &break_target,
                                                       &broken)) {
                                    item_entity_spawn_block_drop(
                                        &item_drops, broken,
                                        break_target.hit_x,
                                        break_target.hit_y,
                                        break_target.hit_z,
                                        &cam);
                                }
                                break_timer = 0.0f;
                                break_duration = 0.0f;
                                break_target_valid = false;
                            }
                        } else {
                            break_timer = 0.0f;
                            break_duration = 0.0f;
                            break_target_valid = false;
                        }
                    } else {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    }
                } else {
                    break_timer = 0.0f;
                    break_duration = 0.0f;
                    break_target_valid = false;
                }
            }
            if (place_pressed) {
                if (player.mode == PLAYER_MODE_CREATIVE) {
                    BlockID held = hotbar_block_at(selected_hotbar_page,
                                                   selected_hotbar_slot);
                    PlaceResult pr = try_place_targeted_block(&world, &cam,
                                                              &player, held);
                    if (pr != PLACE_OK && debug_enabled) {
                        chat_log(&chat, "place %s -> %s",
                                 BlockRegistry[held].name,
                                 place_result_name(pr));
                    }
                } else if (player.mode == PLAYER_MODE_SURVIVAL) {
                    const ItemStack *held_stack =
                        survival_inventory_hotbar_stack(&survival_inventory,
                                                        selected_hotbar_slot);

                    if (!item_stack_is_empty(held_stack)) {
                        ItemID held_item = held_stack->item;

                        if (item_is_placeable_block(held_item)) {
                            BlockID held = item_place_block(held_item);
                            PlaceResult pr =
                                try_place_targeted_block(&world, &cam,
                                                        &player, held);

                            if (pr == PLACE_OK) {
                                survival_inventory_remove_storage(&survival_inventory,
                                                                  selected_hotbar_slot,
                                                                  1);
                            } else if (debug_enabled) {
                                chat_log(&chat, "place %s -> %s",
                                         item_name(held_item),
                                         place_result_name(pr));
                            }
                        } else if (debug_enabled) {
                            chat_log(&chat, "%s is not placeable",
                                     item_name(held_item));
                        }
                    }
                }
            }
        } else {
            break_timer = 0.0f;
            break_duration = 0.0f;
            break_target_valid = false;
            hand_swing_timer = HAND_SWING_SECONDS;
            (void)input_consume_hotbar_slot(&inp);
            (void)input_consume_hotbar_page(&inp);
            (void)input_consume_break(&inp);
            (void)input_consume_place(&inp);
        }
        if (hand_swing_timer < HAND_SWING_SECONDS) {
            hand_swing_timer += frame_dt;
            if (hand_swing_timer > HAND_SWING_SECONDS)
                hand_swing_timer = HAND_SWING_SECONDS;
        }

        double lighting_ns = 0.0;
        double gen_drain_ns = 0.0;
        double mesh_drain_ns = 0.0;
        if (world.lighting_dirty &&
            can_rebuild_deferred_lighting(&world, &player)) {
            struct timespec lighting_start, lighting_end;
            clock_gettime(CLOCK_MONOTONIC, &lighting_start);
            world_rebuild_lighting(&world);
            world.lighting_dirty = false;
            clock_gettime(CLOCK_MONOTONIC, &lighting_end);
            lighting_ns = (double)ns_diff(&lighting_end, &lighting_start);
        }
        struct timespec gen_start, gen_end;
        clock_gettime(CLOCK_MONOTONIC, &gen_start);
        gen_worker_drain_pending(&world);
        clock_gettime(CLOCK_MONOTONIC, &gen_end);
        gen_drain_ns = (double)ns_diff(&gen_end, &gen_start);
        if (world.meshes_dirty) {
            struct timespec mesh_start, mesh_end;
            clock_gettime(CLOCK_MONOTONIC, &mesh_start);
            mesh_worker_drain_dirty(&world);
            clock_gettime(CLOCK_MONOTONIC, &mesh_end);
            mesh_drain_ns = (double)ns_diff(&mesh_end, &mesh_start);
        }

        struct timespec render_start, begin_end, draw_end, end_end;
        clock_gettime(CLOCK_MONOTONIC, &render_start);

        /* Late mouse re-sample: pull any motion that arrived during the
         * stream/lighting/mesh phases and fold it into the camera before
         * we hand it to the renderer. The early apply at the top of the
         * loop still feeds physics direction; this just closes the
         * input-to-display gap (~5-15 ms on the FPGA target) on the
         * frames where update work isn't trivial. Keys are not consumed
         * here - any edge events the second poll picks up roll into the
         * next frame's normal input handling. */
        if (!paused && inventory_open) {
            input_update(&inp);
            inventory_cursor_x += inp.cursor_dx * INVENTORY_CURSOR_SPEED;
            inventory_cursor_y += inp.cursor_dy * INVENTORY_CURSOR_SPEED;
            if (inventory_cursor_x < INVENTORY_CURSOR_MIN_X)
                inventory_cursor_x = INVENTORY_CURSOR_MIN_X;
            if (inventory_cursor_y < INVENTORY_CURSOR_MIN_Y)
                inventory_cursor_y = INVENTORY_CURSOR_MIN_Y;
            if (inventory_cursor_x > SCREEN_WIDTH - 1.0f)
                inventory_cursor_x = SCREEN_WIDTH - 1.0f;
            if (inventory_cursor_y > SCREEN_HEIGHT - 1.0f)
                inventory_cursor_y = SCREEN_HEIGHT - 1.0f;
            input_clear_mouse(&inp);
        } else if (!paused && !chat_open) {
            input_update(&inp);
            cam.yaw   += inp.mouse_dx * mouse_sens;
            cam.pitch -= inp.mouse_dy * mouse_sens;
            if (cam.pitch >  PITCH_LIMIT) cam.pitch =  PITCH_LIMIT;
            if (cam.pitch < -PITCH_LIMIT) cam.pitch = -PITCH_LIMIT;
            input_clear_mouse(&inp);
        }

        renderer_set_camera(ctx, &cam);
        renderer_begin_frame(ctx);
        clock_gettime(CLOCK_MONOTONIC, &begin_end);
        int sky_quads = renderer_draw_sky(ctx, world_time);
        int quads = renderer_draw_world(ctx, &world, world_time);
        item_entities_draw(ctx, &item_drops, world_time);
        if (camera_underwater)
            draw_underwater_overlay(ctx);
        if (player.mode == PLAYER_MODE_SURVIVAL)
            draw_damage_overlay(ctx, damage_flash_timer);
        if (!paused && !chat_open &&
            player.mode == PLAYER_MODE_SURVIVAL &&
            break_target_valid && break_timer > 0.0f && break_duration > 0.0f) {
            quads += renderer_draw_block_break_overlay(
                ctx,
                break_target.hit_x, break_target.hit_y, break_target.hit_z,
                break_timer / break_duration);
        }
        chat_draw(&chat, ctx);
        if (!paused && !chat_is_open(&chat)) {
            if (player.mode == PLAYER_MODE_CREATIVE) {
                draw_block_in_hand(ctx,
                                   hotbar_block_at(selected_hotbar_page,
                                                   selected_hotbar_slot),
                                   hand_swing_timer);
                draw_hotbar(ctx, selected_hotbar_slot,
                            selected_hotbar_page, player.mode, NULL);
                renderer_draw_crosshair(ctx);
            } else if (player.mode == PLAYER_MODE_SURVIVAL) {
                draw_bare_hand(ctx, hand_swing_timer);
                draw_hotbar(ctx, selected_hotbar_slot, 0, player.mode,
                            &survival_inventory);
                draw_healthbar(ctx, player_health_units, damage_flash_timer);
                draw_hungerbar(ctx);
                if (camera_underwater ||
                    player_air_seconds < PLAYER_MAX_AIR_SECONDS)
                    draw_oxygenbar(ctx, player_air_seconds);
                renderer_draw_crosshair(ctx);
            }
        }
        if (!paused && inventory_open)
            draw_survival_inventory(ctx, &survival_inventory,
                                    selected_hotbar_slot,
                                    inventory_cursor_x,
                                    inventory_cursor_y);
        pause_menu_draw(&pause, ctx, &pause_settings);
        if (fps_text_len > 0) {
            chat_draw_text_scaled(ctx, fps_text, fps_text_len,
                                  13.0f, 13.0f, 0, HUD_SCALE_I);
            chat_draw_text_scaled(ctx, fps_text, fps_text_len,
                                  12.0f, 12.0f, 5, HUD_SCALE_I);
        }
        if (debug_hud_enabled)
            draw_debug_hud(ctx, &player, &cam, &world);
        clock_gettime(CLOCK_MONOTONIC, &draw_end);
        renderer_end_frame(ctx);
        clock_gettime(CLOCK_MONOTONIC, &end_end);
        mesh_worker_reap_retired(&world);

        double update_ns = (double)ns_diff(&render_start, &loop_start);
        double begin_ns = (double)ns_diff(&begin_end, &render_start);
        double draw_ns = (double)ns_diff(&draw_end, &begin_end);
        double end_ns = (double)ns_diff(&end_end, &draw_end);
        double work_ns = (double)ns_diff(&end_end, &loop_start);

        if (status_log_enabled) {
            printf("\rmode=%-9s pos=(%.1f,%.1f,%.1f) v=(%.1f,%.1f,%.1f) gnd=%d yaw=%.2f pitch=%.2f quads=%3d sky=%2d  ",
                   player_mode_name(player.mode),
                   player.x, player.y, player.z,
                   player.vx, player.vy, player.vz, player.is_grounded,
                   cam.yaw, cam.pitch, quads, sky_quads);
            fflush(stdout);
        }

        /* Sleep for the remainder of the frame budget */
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long used = ns_diff(&frame_end, &loop_start);
        double sleep_ns = 0.0;
        if (used < frame_ns) {
            struct timespec ts = { 0, frame_ns - used };
            struct timespec sleep_start, sleep_end;
            clock_gettime(CLOCK_MONOTONIC, &sleep_start);
            nanosleep(&ts, NULL);
            clock_gettime(CLOCK_MONOTONIC, &sleep_end);
            sleep_ns = (double)ns_diff(&sleep_end, &sleep_start);
        }

        perf_frames++;
        perf_quads += quads;
        perf_sky_quads += sky_quads;
        perf_physics_steps += physics_steps;
        perf_physics_dropped_steps += dropped_physics_steps;
        perf_update_ns += update_ns;
        perf_begin_ns += begin_ns;
        perf_draw_ns += draw_ns;
        perf_end_ns += end_ns;
        perf_sleep_ns += sleep_ns;
        perf_work_ns += work_ns;
        perf_physics_ns += (double)ns_diff(&physics_end, &physics_start);
        perf_stream_ns += (double)ns_diff(&stream_end, &stream_start);
        perf_stream_wait_ns += (double)world.last_stream_lock_wait_ns;
        perf_stream_body_ns += (double)world.last_stream_body_ns;
        perf_lighting_ns += lighting_ns;
        perf_gen_drain_ns += gen_drain_ns;
        perf_mesh_drain_ns += mesh_drain_ns;
        if (work_ns > perf_max_work_ns)
            perf_max_work_ns = work_ns;

        struct timespec perf_now;
        clock_gettime(CLOCK_MONOTONIC, &perf_now);
        long perf_elapsed_ns = ns_diff(&perf_now, &perf_window_start);
        if (perf_elapsed_ns >= PERF_LOG_NS && perf_frames > 0) {
            double elapsed_s = (double)perf_elapsed_ns / 1e9;
            double frame_div = (double)perf_frames;
            double fps = frame_div / elapsed_s;

            int n = snprintf(fps_text, sizeof(fps_text), "fps %.1f", fps);
            if (n < 0) n = 0;
            if (n > (int)sizeof(fps_text) - 1) n = (int)sizeof(fps_text) - 1;
            fps_text_len = n;

            if (debug_enabled) {
                if (status_log_enabled) {
                    printf("\n");
                    fflush(stdout);
                }

                fprintf(stderr,
                        "perf: fps=%5.1f frame=%6.2fms work=%6.2fms "
                        "update=%5.2fms begin=%5.2fms draw=%6.2fms "
                        "end=%6.2fms sleep=%5.2fms max_work=%6.2fms "
                        "quads=%5.1f sky=%4.1f phys=%4.1f drop=%4.1f "
                        "upd_phys=%5.2f stream=%5.2f wait=%5.2f body=%5.2f "
                        "light=%5.2f gen=%5.2f mesh=%5.2f\n",
                        fps,
                        ns_to_ms((double)perf_elapsed_ns / frame_div),
                        ns_to_ms(perf_work_ns / frame_div),
                        ns_to_ms(perf_update_ns / frame_div),
                        ns_to_ms(perf_begin_ns / frame_div),
                        ns_to_ms(perf_draw_ns / frame_div),
                        ns_to_ms(perf_end_ns / frame_div),
                        ns_to_ms(perf_sleep_ns / frame_div),
                        ns_to_ms(perf_max_work_ns),
                        (double)perf_quads / frame_div,
                        (double)perf_sky_quads / frame_div,
                        (double)perf_physics_steps / frame_div,
                        (double)perf_physics_dropped_steps / frame_div,
                        ns_to_ms(perf_physics_ns / frame_div),
                        ns_to_ms(perf_stream_ns / frame_div),
                        ns_to_ms(perf_stream_wait_ns / frame_div),
                        ns_to_ms(perf_stream_body_ns / frame_div),
                        ns_to_ms(perf_lighting_ns / frame_div),
                        ns_to_ms(perf_gen_drain_ns / frame_div),
                        ns_to_ms(perf_mesh_drain_ns / frame_div));
            }

            perf_window_start = perf_now;
            perf_frames = 0;
            perf_quads = 0;
            perf_sky_quads = 0;
            perf_physics_steps = 0;
            perf_physics_dropped_steps = 0;
            perf_update_ns = 0.0;
            perf_begin_ns = 0.0;
            perf_draw_ns = 0.0;
            perf_end_ns = 0.0;
            perf_sleep_ns = 0.0;
            perf_work_ns = 0.0;
            perf_physics_ns = 0.0;
            perf_stream_ns = 0.0;
            perf_stream_wait_ns = 0.0;
            perf_stream_body_ns = 0.0;
            perf_lighting_ns = 0.0;
            perf_gen_drain_ns = 0.0;
            perf_mesh_drain_ns = 0.0;
            perf_max_work_ns = 0.0;
        }
    }

#undef RESET_PLAYER_AFTER_DEATH
#undef ENVIRONMENT_TICK_INTERVAL

    if (status_log_enabled)
        printf("\n");
    input_shutdown(&inp);
    /* Stop gen worker first: it can enqueue mesh-dirty work via finalize,
     * so quiescing it before the mesh worker avoids a final partial mesh
     * round that would just be discarded on shutdown. */
    gen_worker_stop();
    mesh_worker_stop();
    if (!world_flush(&world))
        fprintf(stderr, "world: failed to flush modified chunks on shutdown\n");
    world_free(&world);
    renderer_shutdown(ctx);
    return 0;
}
