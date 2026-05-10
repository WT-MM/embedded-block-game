#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

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
#include "game_items.h"
#include "game_home.h"
#include "env_util.h"

#define DEFAULT_MOUSE_SENS 0.003f /* radians per pixel */
#define MIN_MOUSE_SENS_PERCENT 25
#define MAX_MOUSE_SENS_PERCENT 400
#define MOUSE_SENS_PERCENT_STEP 5
#define DEFAULT_FOV_DEG 86.5f
#define MIN_FOV_DEG_X10 300
#define MAX_FOV_DEG_X10 1500
#define FOV_DEG_X10_STEP 50
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
#define HOTBAR_SLOT_COUNT 9
#define HOTBAR_PAGE_COUNT 7
#define BLOCK_REACH_DISTANCE 6.0f
#define BLOCK_TRACE_STEP 0.05f
#define HAND_SWING_SECONDS 0.26f
#define CREATIVE_BLOCK_BREAK_REPEAT_SECONDS 0.12f
#define PLAYER_MAX_HEALTH_UNITS 20
#define PLAYER_MAX_FOOD_UNITS 20
#define FOOD_REGEN_INTERVAL_SECONDS 1.0f
#define PLAYER_MAX_AIR_SECONDS 15.0f
#define PLAYER_AIR_REFILL_PER_SECOND 5.0f
#define DROWN_DAMAGE_INTERVAL_SECONDS 1.0f
#define DROWN_DAMAGE_UNITS 2
#define AIR_BUBBLE_COUNT 10
#define AIR_BUBBLE_POP_WINDOW_SECONDS 0.10f
#define DAMAGE_FLASH_SECONDS 0.50f
#define CACTUS_DAMAGE_INTERVAL_SECONDS 0.50f
#define CACTUS_DAMAGE_UNITS 1
#define CACTUS_CONTACT_MARGIN 0.08f
#define LAVA_DAMAGE_INTERVAL_SECONDS 0.50f
#define LAVA_DAMAGE_UNITS 4
#define FALL_DAMAGE_SAFE_DISTANCE 3.0f
#define FALL_DAMAGE_EPSILON 0.01f
#define FALL_DAMAGE_MULTIPLIER 1.0f
#define CREATIVE_FLIGHT_DOUBLE_TAP_SECONDS 0.35f
#define PLAYER_SPAWN_X 0.0f
#define PLAYER_SPAWN_Y ((float)(WORLD_CHUNK_HEIGHT - 2))
#define PLAYER_SPAWN_Z -1.5f
#define COMMAND_TIME_DAY_SECONDS   0.0f
#define COMMAND_TIME_NIGHT_SECONDS 90.0f
#define COMMAND_FILL_MAX_BLOCKS 4096
#define COMMAND_ITEMS_PER_PAGE 6
#define HOTBAR_SLOT_PIXELS (17.0f * HUD_SCALE)
#define HOTBAR_GAP_PIXELS (2.0f * HUD_SCALE)
#define HOTBAR_BOTTOM_MARGIN_PIXELS (4.0f * HUD_SCALE + 1.0f)
#define INVENTORY_CURSOR_MIN_X 0.0f
#define INVENTORY_CURSOR_MIN_Y 0.0f
#define INVENTORY_CURSOR_SPEED 1.0f
#define FURNACE_SMELT_SECONDS 0.75f
#define FURNACE_MAX_STATES 32
#define RECIPE_LOOKUP_RECIPES_PER_PAGE 2

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
    BlockID type;
    int wx;
    int wy;
    int wz;
} BlockDropRequest;

typedef struct {
    InventorySlotArea area;
    int index;
} InventoryHit;

typedef enum {
    FURNACE_SLOT_NONE = 0,
    FURNACE_SLOT_INPUT,
    FURNACE_SLOT_FUEL,
    FURNACE_SLOT_OUTPUT,
    FURNACE_SLOT_STORAGE,
} FurnaceSlotArea;

typedef struct {
    FurnaceSlotArea area;
    int index;
} FurnaceHit;

typedef struct {
    bool active;
    int x;
    int y;
    int z;
    ItemStack input;
    ItemStack fuel;
    ItemStack output;
    bool smelting;
    float timer;
    ItemID smelt_output;
} FurnaceState;

typedef struct {
    float panel_x;
    float panel_y;
    float panel_w;
    float panel_h;
    float recipe_x;
    float recipe_y;
    float recipe_w;
    float recipe_h;
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
    int craft_grid_dim;
} SurvivalInventoryLayout;

typedef struct {
    float panel_x;
    float panel_y;
    float panel_w;
    float panel_h;
    float slot;
    float gap;
    float input_x;
    float input_y;
    float fuel_x;
    float fuel_y;
    float output_x;
    float output_y;
    float progress_x;
    float progress_y;
    float progress_w;
    float progress_h;
    float main_x;
    float main_y;
    float hotbar_x;
    float hotbar_y;
} FurnaceLayout;

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
        BLOCK_FURNACE,
        BLOCK_TORCH,
        BLOCK_DOOR,
        BLOCK_CACTUS,
        BLOCK_RED_MUSHROOM,
        BLOCK_BROWN_MUSHROOM,
        BLOCK_SUGAR_CANE,
        BLOCK_YELLOW_FLOWER,
    },
    {
        BLOCK_REDSTONE_WIRE_UNCONNECTED,
        BLOCK_REDSTONE_WIRE_OFF,
        BLOCK_REDSTONE_WIRE_ON,
        BLOCK_REDSTONE_TORCH_OFF,
        BLOCK_REDSTONE_TORCH_ON,
        BLOCK_REPEATER_OFF,
        BLOCK_REPEATER_ON,
        BLOCK_LAMP_OFF,
        BLOCK_BUTTON,
    },
    {
        BLOCK_COMPARATOR_OFF,
        BLOCK_COMPARATOR_ON,
        BLOCK_COMPARATOR_EAST_OFF,
        BLOCK_COMPARATOR_EAST_ON,
        BLOCK_COMPARATOR_SOUTH_OFF,
        BLOCK_COMPARATOR_SOUTH_ON,
        BLOCK_COMPARATOR_WEST_OFF,
        BLOCK_COMPARATOR_WEST_ON,
        BLOCK_LAMP,
    },
    {
        BLOCK_LEVER_OFF,
        BLOCK_WOOD_PRESSURE_PLATE,
        BLOCK_STONE_PRESSURE_PLATE,
        BLOCK_REDSTONE_BLOCK,
        BLOCK_REDSTONE_WIRE_UNCONNECTED,
        BLOCK_REDSTONE_TORCH_ON,
        BLOCK_REPEATER_OFF,
        BLOCK_COMPARATOR_OFF,
        BLOCK_LAMP_OFF,
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
    float parsed;

    if (!env_parse_float_value(env_get_nonempty("VOXEL_MOUSE_SENS"), &parsed) ||
        parsed <= 0.0f)
        return DEFAULT_MOUSE_SENS;

    return parsed;
}

static int clamp_mouse_sensitivity_percent(int percent)
{
    if (percent < MIN_MOUSE_SENS_PERCENT)
        return MIN_MOUSE_SENS_PERCENT;
    if (percent > MAX_MOUSE_SENS_PERCENT)
        return MAX_MOUSE_SENS_PERCENT;
    return percent;
}

static int mouse_sensitivity_to_percent(float sensitivity)
{
    int percent;

    if (sensitivity <= 0.0f)
        sensitivity = DEFAULT_MOUSE_SENS;

    percent = (int)lroundf((sensitivity / DEFAULT_MOUSE_SENS) * 100.0f);
    return clamp_mouse_sensitivity_percent(percent);
}

static float mouse_sensitivity_from_percent(int percent)
{
    percent = clamp_mouse_sensitivity_percent(percent);
    return DEFAULT_MOUSE_SENS * ((float)percent / 100.0f);
}

static int clamp_fov_degrees_x10(int fov_x10)
{
    if (fov_x10 < MIN_FOV_DEG_X10)
        return MIN_FOV_DEG_X10;
    if (fov_x10 > MAX_FOV_DEG_X10)
        return MAX_FOV_DEG_X10;
    return fov_x10;
}

static int fov_degrees_to_x10(float fov_deg)
{
    int fov_x10;

    if (fov_deg != fov_deg)
        fov_deg = DEFAULT_FOV_DEG;

    fov_x10 = (int)lroundf(fov_deg * 10.0f);
    return clamp_fov_degrees_x10(fov_x10);
}

static float fov_degrees_from_x10(int fov_x10)
{
    fov_x10 = clamp_fov_degrees_x10(fov_x10);
    return (float)fov_x10 / 10.0f;
}

static int read_status_log_enabled(int debug_enabled)
{
    const char *value = env_get_nonempty("VOXEL_STATUS_LOG");

    if (!value || strcmp(value, "auto") == 0)
        return debug_enabled;
    return env_value_is_true(value);
}

static int read_target_fps(void)
{
    return env_int_or_default("VOXEL_TARGET_FPS",
                              DEFAULT_TARGET_FPS,
                              MIN_TARGET_FPS,
                              MAX_TARGET_FPS);
}

static int read_render_distance_chunks(void)
{
    return env_int_or_default("VOXEL_RENDER_DISTANCE",
                              DEFAULT_WORLD_RENDER_DISTANCE_CHUNKS,
                              1,
                              MAX_WORLD_RENDER_DISTANCE_CHUNKS);
}

static int read_stream_chunks_per_frame(void)
{
    return env_int_or_default_fallback("VOXEL_CHUNKS_PER_FRAME",
                                       "VOXEL_CHUNK_PER_FRAME",
                                       DEFAULT_STREAM_CHUNKS_PER_FRAME,
                                       0,
                                       MAX_STREAM_CHUNKS_PER_FRAME);
}

static int read_max_physics_steps_per_frame(void)
{
    return env_int_capped_or_default("VOXEL_MAX_PHYSICS_STEPS_PER_FRAME",
                                     DEFAULT_MAX_PHYSICS_STEPS_PER_FRAME,
                                     0,
                                     MAX_PHYSICS_STEPS_PER_FRAME);
}

static float read_camera_fov_degrees(void)
{
    return env_float_or_default("VOXEL_FOV_DEG",
                                DEFAULT_FOV_DEG,
                                fov_degrees_from_x10(MIN_FOV_DEG_X10),
                                fov_degrees_from_x10(MAX_FOV_DEG_X10));
}

/* Camera focal length is stored in screen pixels, so the FOV scales with the
 * render resolution unless we compensate. The pre-SDRAM 320x240 build used
 * focal=170 -> horizontal FOV ~= 86.5 degrees. */
static float camera_focal_px_from_fov(float fov_deg)
{
    float fov_rad;

    fov_deg = fov_degrees_from_x10(fov_degrees_to_x10(fov_deg));
    fov_rad = fov_deg * (float)M_PI / 180.0f;
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
    PlayerPhysicsConfig physics;
    float yaw = 0.0f;
    float pitch = -0.3f;
    float depth = camera_focal_px_from_fov(read_camera_fov_degrees());

    if (!player)
        return;

    mode = player->mode;
    physics = player->physics;
    if (cam) {
        yaw = cam->yaw;
        pitch = cam->pitch;
        depth = cam->depth;
    }

    player_init(player, PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z);
    player->physics = physics;
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

static bool game_block_is_lava(BlockID id)
{
    return id == BLOCK_LAVA || id == BLOCK_LAVA_FLOW;
}

static bool game_block_is_fluid_source(BlockID id)
{
    return id == BLOCK_WATER || id == BLOCK_LAVA;
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

static bool trace_target_fluid_source(const VoxelWorld *world, const Camera *cam,
                                      float max_distance, BlockTarget *out)
{
    Vec3 dir = camera_forward(cam);
    bool have_prev_cell = false;
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
        if (game_block_is_fluid_source(block)) {
            if (out) {
                out->hit = true;
                out->hit_x = block_x;
                out->hit_y = block_y;
                out->hit_z = block_z;
            }
            return true;
        }
        if (!game_block_is_trace_passable(block))
            return false;
    }

    return false;
}

static bool player_touches_lava(const VoxelWorld *world, const Player *player)
{
    float min_x;
    float max_x;
    float min_y;
    float max_y;
    float min_z;
    float max_z;
    int block_min_x;
    int block_max_x;
    int block_min_y;
    int block_max_y;
    int block_min_z;
    int block_max_z;

    if (!world || !player)
        return false;

    min_x = player->x - PLAYER_WIDTH * 0.5f;
    max_x = player->x + PLAYER_WIDTH * 0.5f;
    min_y = player->y;
    max_y = player->y + PLAYER_HEIGHT;
    min_z = player->z - PLAYER_DEPTH * 0.5f;
    max_z = player->z + PLAYER_DEPTH * 0.5f;

    block_min_x = (int)floorf(min_x);
    block_max_x = (int)floorf(max_x);
    block_min_y = (int)floorf(min_y);
    block_max_y = (int)floorf(max_y);
    block_min_z = (int)floorf(min_z);
    block_max_z = (int)floorf(max_z);

    for (int y = block_min_y; y <= block_max_y; y++) {
        if (y < 0 || y >= WORLD_CHUNK_HEIGHT)
            continue;
        for (int z = block_min_z; z <= block_max_z; z++) {
            for (int x = block_min_x; x <= block_max_x; x++) {
                if (game_block_is_lava(world_get_block(world, x, y, z)))
                    return true;
            }
        }
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

static bool player_touches_cactus(const VoxelWorld *world, const Player *player)
{
    float min_x;
    float max_x;
    float min_y;
    float max_y;
    float min_z;
    float max_z;
    int block_min_x;
    int block_max_x;
    int block_min_y;
    int block_max_y;
    int block_min_z;
    int block_max_z;

    if (!world || !player)
        return false;

    min_x = player->x - PLAYER_WIDTH * 0.5f - CACTUS_CONTACT_MARGIN;
    max_x = player->x + PLAYER_WIDTH * 0.5f + CACTUS_CONTACT_MARGIN;
    min_y = player->y;
    max_y = player->y + PLAYER_HEIGHT;
    min_z = player->z - PLAYER_DEPTH * 0.5f - CACTUS_CONTACT_MARGIN;
    max_z = player->z + PLAYER_DEPTH * 0.5f + CACTUS_CONTACT_MARGIN;

    block_min_x = (int)floorf(min_x);
    block_max_x = (int)floorf(max_x);
    block_min_y = (int)floorf(min_y);
    block_max_y = (int)floorf(max_y);
    block_min_z = (int)floorf(min_z);
    block_max_z = (int)floorf(max_z);

    for (int y = block_min_y; y <= block_max_y; y++) {
        if (y < 0 || y >= WORLD_CHUNK_HEIGHT)
            continue;
        for (int z = block_min_z; z <= block_max_z; z++) {
            for (int x = block_min_x; x <= block_max_x; x++) {
                if (world_get_block(world, x, y, z) == BLOCK_CACTUS)
                    return true;
            }
        }
    }

    return false;
}

static void update_pressure_plate_depressions(VoxelWorld *world,
                                              const Player *player,
                                              const ItemEntityPool *drops)
{
    WorldPressurePlateTrigger triggers[ITEM_ENTITY_MAX + 1];
    size_t count = 0;

    if (!world || !player)
        return;

    triggers[count++] = (WorldPressurePlateTrigger){
        .min_x = player->x - PLAYER_WIDTH * 0.5f,
        .max_x = player->x + PLAYER_WIDTH * 0.5f,
        .min_y = player->y,
        .max_y = player->y + PLAYER_HEIGHT,
        .min_z = player->z - PLAYER_DEPTH * 0.5f,
        .max_z = player->z + PLAYER_DEPTH * 0.5f,
        .mask = WORLD_PRESSURE_TRIGGER_WOOD | WORLD_PRESSURE_TRIGGER_STONE,
    };

    if (drops) {
        const float radius = ITEM_ENTITY_SIZE_WORLD * 0.5f;

        for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
            const ItemEntity *item = &drops->items[i];

            if (!item->active || item_stack_is_empty(&item->stack))
                continue;
            triggers[count++] = (WorldPressurePlateTrigger){
                .min_x = item->position.x - radius,
                .max_x = item->position.x + radius,
                .min_y = item->position.y - radius,
                .max_y = item->position.y + radius,
                .min_z = item->position.z - radius,
                .max_z = item->position.z + radius,
                .mask = WORLD_PRESSURE_TRIGGER_WOOD,
            };
        }
    }

    (void)world_update_pressure_plates_for_triggers(world, triggers, count);
}

static bool block_is_vertical_plant_drop(BlockID type)
{
    return type == BLOCK_SUGAR_CANE || type == BLOCK_CACTUS;
}

static size_t collect_vertical_plant_cascade_drops(
    const VoxelWorld *world,
    const BlockTarget *target,
    BlockDropRequest drops[WORLD_CHUNK_HEIGHT],
    size_t drop_cap)
{
    size_t count = 0;

    if (!world || !target || !target->hit || !drops)
        return 0;

    for (int y = target->hit_y + 1;
         y < WORLD_CHUNK_HEIGHT && count < drop_cap;
         y++) {
        BlockID type = world_get_block(world, target->hit_x, y,
                                       target->hit_z);

        if (!block_is_vertical_plant_drop(type))
            break;
        drops[count++] = (BlockDropRequest){
            .type = type,
            .wx = target->hit_x,
            .wy = y,
            .wz = target->hit_z,
        };
    }

    return count;
}

static void spawn_block_drop_requests(ItemEntityPool *drops,
                                      const BlockDropRequest requests[],
                                      size_t request_count,
                                      Vec3 push_dir)
{
    if (!drops || !requests)
        return;

    for (size_t i = 0; i < request_count; i++) {
        item_entity_spawn_block_drop(drops,
                                     requests[i].type,
                                     requests[i].wx,
                                     requests[i].wy,
                                     requests[i].wz,
                                     push_dir);
    }
}

static bool break_block_target(VoxelWorld *world, const BlockTarget *target,
                               BlockID *broken_block_out)
{
    BlockID broken;
    int lower_y;

    if (!target || !target->hit)
        return false;
    broken = world_get_block(world, target->hit_x, target->hit_y, target->hit_z);
    if (broken == BLOCK_AIR)
        return false;

    if (block_is_door(broken)) {
        lower_y = block_is_door_upper(broken) ?
            target->hit_y - 1 : target->hit_y;

        if (block_is_door(world_get_block(world,
                                          target->hit_x,
                                          lower_y,
                                          target->hit_z))) {
            world_set_block(world, target->hit_x, lower_y, target->hit_z,
                            BLOCK_AIR);
            world_mark_chunk_mesh_edit_priority(world,
                                                target->hit_x,
                                                target->hit_z);
        }
        if (block_is_door(world_get_block(world,
                                          target->hit_x,
                                          lower_y + 1,
                                          target->hit_z))) {
            world_set_block(world, target->hit_x, lower_y + 1, target->hit_z,
                            BLOCK_AIR);
            world_mark_chunk_mesh_edit_priority(world,
                                                target->hit_x,
                                                target->hit_z);
        }

        if (broken_block_out)
            *broken_block_out = BLOCK_DOOR;
        return true;
    }

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

static bool try_break_targeted_block(VoxelWorld *world,
                                     const Camera *cam,
                                     BlockTarget *target_out,
                                     BlockID *broken_block_out)
{
    BlockTarget target = {0};
    BlockID broken = BLOCK_AIR;

    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;

    if (!break_block_target(world, &target, &broken))
        return false;

    if (target_out)
        *target_out = target;
    if (broken_block_out)
        *broken_block_out = broken;
    return true;
}

typedef enum {
    PLACE_OK = 0,
    PLACE_FAIL_BAD_TYPE,
    PLACE_FAIL_NO_TRACE,
    PLACE_FAIL_NO_AIR_NEAR_HIT,
    PLACE_FAIL_TARGET_OCCUPIED,
    PLACE_FAIL_NO_SUPPORT,
    PLACE_FAIL_PLAYER_BLOCKED,
    PLACE_FAIL_WORLD_REJECTED,
} PlaceResult;

static bool place_block_is_redstone_wire(BlockID type)
{
    return type == BLOCK_REDSTONE_WIRE_UNCONNECTED ||
           type == BLOCK_REDSTONE_WIRE_OFF ||
           type == BLOCK_REDSTONE_WIRE_ON;
}

static bool place_block_is_button(BlockID type)
{
    return type == BLOCK_BUTTON || type == BLOCK_BUTTON_PRESSED;
}

static bool place_block_is_pressure_plate(BlockID type)
{
    return block_is_pressure_plate(type);
}

static BlockDoorFacing door_facing_from_camera(const Camera *cam);

static BlockID normalize_placed_block(BlockID type, const Camera *cam)
{
    if (place_block_is_redstone_wire(type))
        return BLOCK_REDSTONE_WIRE_UNCONNECTED;
    if (place_block_is_button(type))
        return BLOCK_BUTTON;
    if (place_block_is_pressure_plate(type))
        return block_pressure_plate_unpressed(type);
    if (block_is_repeater(type))
        return block_repeater_make(door_facing_from_camera(cam),
                                   block_redstone_directional_powered(type));
    if (block_is_comparator(type))
        return block_comparator_make(door_facing_from_camera(cam),
                                     block_redstone_directional_powered(type));
    return type;
}

static bool placement_block_can_be_replaced(BlockID type)
{
    return type == BLOCK_AIR ||
           type == BLOCK_WATER ||
           type == BLOCK_WATER_FLOW ||
           type == BLOCK_LAVA ||
           type == BLOCK_LAVA_FLOW;
}

static bool placement_requires_floor_support(BlockID type)
{
    BlockRenderModel model = block_render_model(type);

    return model == BLOCK_RENDER_CROSS ||
           model == BLOCK_RENDER_TORCH ||
           model == BLOCK_RENDER_FLAT;
}

static bool placement_has_floor_support(const VoxelWorld *world,
                                        int wx,
                                        int wy,
                                        int wz)
{
    BlockID support;

    if (wy <= 0)
        return false;
    support = world_get_block(world, wx, wy - 1, wz);
    return support != BLOCK_AIR && !block_is_passable(support);
}

static PlaceResult try_place_targeted_block(VoxelWorld *world, const Camera *cam,
                                            const Player *player, BlockID type)
{
    BlockTarget target = {0};
    BlockID place_type;

    if (type <= BLOCK_AIR || type >= NUM_BLOCK_TYPES)
        return PLACE_FAIL_BAD_TYPE;
    place_type = normalize_placed_block(type, cam);
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return PLACE_FAIL_NO_TRACE;
    if (!target.place_valid)
        return PLACE_FAIL_NO_AIR_NEAR_HIT;
    /* Allow overwriting only cells that are actually empty/fluid. Decorative
     * passable blocks still occupy their grid cell and should not stack. */
    BlockID at_place = world_get_block(world,
                                       target.place_x,
                                       target.place_y,
                                       target.place_z);
    if (!placement_block_can_be_replaced(at_place))
        return PLACE_FAIL_TARGET_OCCUPIED;
    if (placement_requires_floor_support(place_type) &&
        !placement_has_floor_support(world,
                                     target.place_x,
                                     target.place_y,
                                     target.place_z))
        return PLACE_FAIL_NO_SUPPORT;
    if (!block_is_passable(place_type) &&
        player_intersects_block(player,
                                target.place_x,
                                target.place_y,
                                target.place_z))
        return PLACE_FAIL_PLAYER_BLOCKED;

    if (!world_set_block(world,
                         target.place_x, target.place_y, target.place_z,
                         place_type))
        return PLACE_FAIL_WORLD_REJECTED;

    world_mark_chunk_mesh_edit_priority(world, target.place_x, target.place_z);
    return PLACE_OK;
}

static bool inventory_can_replace_one_hotbar_item(const SurvivalInventory *inventory,
                                                  int hotbar_slot,
                                                  ItemID expected,
                                                  ItemID replacement)
{
    const ItemStack *held;

    if (!inventory || hotbar_slot < 0 ||
        hotbar_slot >= SURVIVAL_HOTBAR_SLOT_COUNT)
        return false;

    held = &inventory->storage[hotbar_slot];
    if (item_stack_is_empty(held) ||
        held->item != expected ||
        held->count <= 0)
        return false;
    if (held->count == 1)
        return true;

    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT; i++) {
        const ItemStack *stack = &inventory->storage[i];

        if (!item_stack_is_empty(stack) &&
            stack->item == replacement &&
            stack->count < ITEM_STACK_MAX)
            return true;
    }
    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT; i++) {
        if (i == hotbar_slot)
            continue;
        if (item_stack_is_empty(&inventory->storage[i]))
            return true;
    }

    return false;
}

static bool inventory_replace_one_hotbar_item(SurvivalInventory *inventory,
                                              int hotbar_slot,
                                              ItemID expected,
                                              ItemID replacement)
{
    ItemStack *held;

    if (!inventory_can_replace_one_hotbar_item(inventory, hotbar_slot,
                                               expected, replacement))
        return false;

    held = &inventory->storage[hotbar_slot];
    if (held->count == 1) {
        held->item = replacement;
        held->count = 1;
        return true;
    }

    item_stack_remove(held, 1);
    return survival_inventory_add_item(inventory, replacement, 1) == 0;
}

static bool try_fill_empty_bucket(VoxelWorld *world,
                                  const Camera *cam,
                                  SurvivalInventory *inventory,
                                  int hotbar_slot,
                                  Chat *chat)
{
    BlockTarget target = {0};
    BlockID source;
    ItemID filled_item;

    if (!world || !cam || !inventory)
        return false;
    if (!trace_target_fluid_source(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;

    source = world_get_block(world, target.hit_x, target.hit_y, target.hit_z);
    if (source == BLOCK_WATER) {
        filled_item = ITEM_WATER_BUCKET;
    } else if (source == BLOCK_LAVA) {
        filled_item = ITEM_LAVA_BUCKET;
    } else {
        return false;
    }
    if (!inventory_can_replace_one_hotbar_item(inventory, hotbar_slot,
                                               ITEM_BUCKET, filled_item))
        return false;

    if (!world_set_block(world, target.hit_x, target.hit_y, target.hit_z,
                         BLOCK_AIR))
        return false;
    if (!inventory_replace_one_hotbar_item(inventory, hotbar_slot,
                                           ITEM_BUCKET, filled_item)) {
        world_set_block(world, target.hit_x, target.hit_y, target.hit_z,
                        source);
        return false;
    }

    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    if (chat)
        chat_log(chat, "filled %s", item_name(filled_item));
    return true;
}

static bool try_empty_filled_bucket(VoxelWorld *world,
                                    const Camera *cam,
                                    SurvivalInventory *inventory,
                                    int hotbar_slot,
                                    ItemID held_item,
                                    Chat *chat)
{
    BlockTarget target = {0};
    BlockID fluid;
    BlockID previous;

    if (!world || !cam || !inventory)
        return false;
    if (held_item == ITEM_WATER_BUCKET) {
        fluid = BLOCK_WATER;
    } else if (held_item == ITEM_LAVA_BUCKET) {
        fluid = BLOCK_LAVA;
    } else {
        return false;
    }
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.place_valid)
        return false;
    previous = world_get_block(world,
                               target.place_x,
                               target.place_y,
                               target.place_z);
    if (!block_is_passable(previous))
        return false;
    if (!inventory_can_replace_one_hotbar_item(inventory, hotbar_slot,
                                               held_item, ITEM_BUCKET))
        return false;

    if (!world_set_block(world,
                         target.place_x, target.place_y, target.place_z,
                         fluid))
        return false;
    if (!inventory_replace_one_hotbar_item(inventory, hotbar_slot,
                                           held_item, ITEM_BUCKET)) {
        world_set_block(world,
                        target.place_x, target.place_y, target.place_z,
                        previous);
        return false;
    }

    world_mark_chunk_mesh_edit_priority(world, target.place_x, target.place_z);
    if (chat)
        chat_log(chat, "placed %s", fluid == BLOCK_WATER ? "water" : "lava");
    return true;
}

static bool try_use_held_bucket(VoxelWorld *world,
                                const Camera *cam,
                                SurvivalInventory *inventory,
                                int hotbar_slot,
                                ItemID held_item,
                                Chat *chat)
{
    if (held_item == ITEM_BUCKET)
        return try_fill_empty_bucket(world, cam, inventory, hotbar_slot, chat);
    if (held_item == ITEM_WATER_BUCKET || held_item == ITEM_LAVA_BUCKET)
        return try_empty_filled_bucket(world, cam, inventory, hotbar_slot,
                                       held_item, chat);
    return false;
}

static BlockDoorFacing door_facing_from_camera(const Camera *cam)
{
    float fx;
    float fz;

    if (!cam)
        return BLOCK_DOOR_FACING_NORTH;

    fx = sinf(cam->yaw);
    fz = cosf(cam->yaw);
    if (fabsf(fx) > fabsf(fz))
        return fx >= 0.0f ? BLOCK_DOOR_FACING_EAST :
                            BLOCK_DOOR_FACING_WEST;
    return fz >= 0.0f ? BLOCK_DOOR_FACING_SOUTH :
                        BLOCK_DOOR_FACING_NORTH;
}

static PlaceResult try_place_targeted_door(VoxelWorld *world,
                                           const Camera *cam,
                                           const Player *player)
{
    BlockTarget target = {0};
    BlockDoorFacing facing;
    BlockID lower;
    BlockID upper;

    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return PLACE_FAIL_NO_TRACE;
    if (!target.place_valid)
        return PLACE_FAIL_NO_AIR_NEAR_HIT;
    if (target.place_y + 1 >= WORLD_CHUNK_HEIGHT)
        return PLACE_FAIL_WORLD_REJECTED;
    if (!placement_block_can_be_replaced(world_get_block(world,
                                                         target.place_x,
                                                         target.place_y,
                                                         target.place_z)) ||
        !placement_block_can_be_replaced(world_get_block(world,
                                                         target.place_x,
                                                         target.place_y + 1,
                                                         target.place_z)))
        return PLACE_FAIL_TARGET_OCCUPIED;
    if (player &&
        (player_intersects_block(player,
                                 target.place_x,
                                 target.place_y,
                                 target.place_z) ||
         player_intersects_block(player,
                                 target.place_x,
                                 target.place_y + 1,
                                 target.place_z)))
        return PLACE_FAIL_PLAYER_BLOCKED;

    facing = door_facing_from_camera(cam);
    lower = block_door_make(facing, false, false);
    upper = block_door_make(facing, false, true);

    if (!world_set_block(world,
                         target.place_x, target.place_y, target.place_z,
                         lower))
        return PLACE_FAIL_WORLD_REJECTED;
    if (!world_set_block(world,
                         target.place_x, target.place_y + 1, target.place_z,
                         upper)) {
        world_set_block(world,
                        target.place_x, target.place_y, target.place_z,
                        BLOCK_AIR);
        return PLACE_FAIL_WORLD_REJECTED;
    }

    world_mark_chunk_mesh_edit_priority(world, target.place_x, target.place_z);
    return PLACE_OK;
}

static bool try_toggle_targeted_door(VoxelWorld *world,
                                     const Camera *cam,
                                     const Player *player,
                                     Chat *chat)
{
    BlockTarget target = {0};
    BlockID hit;
    int lower_y;
    BlockDoorFacing facing;
    bool next_open;

    if (!world || !cam)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit)
        return false;

    hit = world_get_block(world, target.hit_x, target.hit_y, target.hit_z);
    if (!block_is_door(hit))
        return false;

    lower_y = block_is_door_upper(hit) ? target.hit_y - 1 : target.hit_y;
    facing = block_door_facing(hit);
    next_open = !block_is_door_open(hit);

    if (!next_open && player &&
        (player_intersects_block(player,
                                 target.hit_x,
                                 lower_y,
                                 target.hit_z) ||
         player_intersects_block(player,
                                 target.hit_x,
                                 lower_y + 1,
                                 target.hit_z))) {
        if (chat)
            chat_log(chat, "door is blocked");
        return true;
    }

    if (block_is_door(world_get_block(world,
                                      target.hit_x,
                                      lower_y,
                                      target.hit_z))) {
        world_set_block(world,
                        target.hit_x, lower_y, target.hit_z,
                        block_door_make(facing, next_open, false));
    }
    if (block_is_door(world_get_block(world,
                                      target.hit_x,
                                      lower_y + 1,
                                      target.hit_z))) {
        world_set_block(world,
                        target.hit_x, lower_y + 1, target.hit_z,
                        block_door_make(facing, next_open, true));
    }

    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    if (chat)
        chat_log(chat, "door %s", next_open ? "opened" : "closed");
    return true;
}

static bool try_press_targeted_button(VoxelWorld *world,
                                      const Camera *cam,
                                      Chat *chat)
{
    BlockTarget target = {0};

    if (!world || !cam)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit)
        return false;
    BlockID target_block = world_get_block(world,
                                           target.hit_x,
                                           target.hit_y,
                                           target.hit_z);
    if (target_block != BLOCK_BUTTON && target_block != BLOCK_BUTTON_PRESSED)
        return false;
    if (!world_press_button(world,
                            target.hit_x,
                            target.hit_y,
                            target.hit_z))
        return false;

    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    if (chat)
        chat_log(chat, "button pressed");
    return true;
}

static bool try_toggle_targeted_lever(VoxelWorld *world,
                                      const Camera *cam,
                                      Chat *chat)
{
    BlockTarget target = {0};
    bool powered = false;

    if (!world || !cam)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit)
        return false;
    if (!block_is_lever(world_get_block(world,
                                        target.hit_x,
                                        target.hit_y,
                                        target.hit_z)))
        return false;
    if (!world_toggle_lever(world,
                            target.hit_x,
                            target.hit_y,
                            target.hit_z,
                            &powered))
        return false;

    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    if (chat)
        chat_log(chat, "lever %s", powered ? "on" : "off");
    return true;
}

static bool try_cycle_targeted_repeater(VoxelWorld *world,
                                        const Camera *cam,
                                        Chat *chat)
{
    BlockTarget target = {0};
    uint8_t delay_ticks = 0;

    if (!world || !cam)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit)
        return false;
    if (!block_is_repeater(world_get_block(world,
                                           target.hit_x,
                                           target.hit_y,
                                           target.hit_z)))
        return false;
    if (!world_cycle_repeater_delay(world,
                                    target.hit_x,
                                    target.hit_y,
                                    target.hit_z,
                                    &delay_ticks))
        return false;

    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    if (chat)
        chat_log(chat, "repeater delay: %u tick%s",
                 (unsigned)delay_ticks, delay_ticks == 1 ? "" : "s");
    return true;
}

static int furnace_state_find(const FurnaceState furnaces[],
                              int x, int y, int z)
{
    if (!furnaces)
        return -1;

    for (int i = 0; i < FURNACE_MAX_STATES; i++) {
        if (furnaces[i].active &&
            furnaces[i].x == x &&
            furnaces[i].y == y &&
            furnaces[i].z == z)
            return i;
    }

    return -1;
}

static FurnaceState *furnace_state_free_slot(FurnaceState furnaces[])
{
    if (!furnaces)
        return NULL;

    for (int i = 0; i < FURNACE_MAX_STATES; i++) {
        if (!furnaces[i].active)
            return &furnaces[i];
    }

    return NULL;
}

static FurnaceState *furnace_state_get_or_create(FurnaceState furnaces[],
                                                 int x, int y, int z)
{
    int index = furnace_state_find(furnaces, x, y, z);
    FurnaceState *furnace;

    if (index >= 0)
        return &furnaces[index];

    furnace = furnace_state_free_slot(furnaces);
    if (!furnace)
        return NULL;

    memset(furnace, 0, sizeof(*furnace));
    furnace->active = true;
    furnace->x = x;
    furnace->y = y;
    furnace->z = z;
    furnace->smelt_output = ITEM_NONE;
    return furnace;
}

static bool furnace_output_can_accept(const FurnaceState *furnace,
                                      ItemID output)
{
    if (!furnace || output == ITEM_NONE)
        return false;
    if (item_stack_is_empty(&furnace->output))
        return true;
    return furnace->output.item == output &&
           furnace->output.count < ITEM_STACK_MAX;
}

static bool furnace_slot_accepts_item(FurnaceSlotArea area, ItemID item)
{
    if (item == ITEM_NONE)
        return false;

    switch (area) {
    case FURNACE_SLOT_INPUT:
        return item_furnace_smelt_output(item) != ITEM_NONE;
    case FURNACE_SLOT_FUEL:
        return item_is_furnace_fuel(item);
    case FURNACE_SLOT_STORAGE:
        return true;
    default:
        return false;
    }
}

static void furnace_try_start(FurnaceState *furnace)
{
    ItemID output;

    if (!furnace || !furnace->active || furnace->smelting)
        return;
    if (item_stack_is_empty(&furnace->input) ||
        item_stack_is_empty(&furnace->fuel))
        return;

    output = item_furnace_smelt_output(furnace->input.item);
    if (!furnace_output_can_accept(furnace, output))
        return;

    item_stack_remove(&furnace->input, 1);
    item_stack_remove(&furnace->fuel, 1);
    furnace->smelting = true;
    furnace->timer = 0.0f;
    furnace->smelt_output = output;
}

static void furnace_finish_smelt(FurnaceState *furnace)
{
    if (!furnace || !furnace->smelting)
        return;
    if (!furnace_output_can_accept(furnace, furnace->smelt_output))
        return;

    if (item_stack_add(&furnace->output, furnace->smelt_output, 1) == 0) {
        furnace->smelting = false;
        furnace->timer = 0.0f;
        furnace->smelt_output = ITEM_NONE;
        furnace_try_start(furnace);
    }
}

static void furnace_states_update(FurnaceState furnaces[],
                                  const VoxelWorld *world,
                                  float dt)
{
    if (!furnaces || !world)
        return;

    if (dt < 0.0f)
        dt = 0.0f;

    for (int i = 0; i < FURNACE_MAX_STATES; i++) {
        FurnaceState *furnace = &furnaces[i];

        if (!furnace->active)
            continue;

        if (world_get_block(world, furnace->x, furnace->y, furnace->z) !=
            BLOCK_FURNACE) {
            memset(furnace, 0, sizeof(*furnace));
            continue;
        }

        furnace_try_start(furnace);
        if (!furnace->smelting)
            continue;

        furnace->timer += dt;
        if (furnace->timer >= FURNACE_SMELT_SECONDS)
            furnace_finish_smelt(furnace);
    }
}

static void furnace_drop_stack(ItemEntityPool *drops,
                               const Player *player,
                               ItemStack *stack)
{
    if (!stack || item_stack_is_empty(stack))
        return;

    item_entity_spawn_item_near_player(drops, player, stack->item, stack->count);
    item_stack_clear(stack);
}

static void furnace_state_drop_contents(FurnaceState furnaces[],
                                        ItemEntityPool *drops,
                                        const Player *player,
                                        int x, int y, int z)
{
    int index = furnace_state_find(furnaces, x, y, z);
    FurnaceState *furnace;

    if (index < 0)
        return;

    furnace = &furnaces[index];
    furnace_drop_stack(drops, player, &furnace->input);
    furnace_drop_stack(drops, player, &furnace->fuel);
    furnace_drop_stack(drops, player, &furnace->output);
    memset(furnace, 0, sizeof(*furnace));
}

static bool furnace_state_is_empty(const FurnaceState *furnace)
{
    if (!furnace)
        return true;

    return !furnace->smelting &&
           item_stack_is_empty(&furnace->input) &&
           item_stack_is_empty(&furnace->fuel) &&
           item_stack_is_empty(&furnace->output);
}

static void return_inventory_cursor_to_storage_or_drop(SurvivalInventory *inventory,
                                                       ItemEntityPool *drops,
                                                       const Player *player)
{
    int leftover;

    if (!inventory || item_stack_is_empty(&inventory->cursor))
        return;

    leftover = survival_inventory_add_item(inventory,
                                           inventory->cursor.item,
                                           inventory->cursor.count);
    if (leftover > 0)
        item_entity_spawn_item_near_player(drops, player,
                                           inventory->cursor.item,
                                           leftover);
    item_stack_clear(&inventory->cursor);
}

static void close_furnace_ui(SurvivalInventory *inventory,
                             ItemEntityPool *drops,
                             const Player *player,
                             FurnaceState furnaces[],
                             bool *furnace_open,
                             int *open_furnace_index)
{
    int index = open_furnace_index ? *open_furnace_index : -1;

    return_inventory_cursor_to_storage_or_drop(inventory, drops, player);
    if (furnaces && index >= 0 && index < FURNACE_MAX_STATES &&
        furnace_state_is_empty(&furnaces[index]))
        memset(&furnaces[index], 0, sizeof(furnaces[index]));
    if (furnace_open)
        *furnace_open = false;
    if (open_furnace_index)
        *open_furnace_index = -1;
}

static bool furnace_take_output(SurvivalInventory *inventory,
                                FurnaceState *furnace)
{
    int remaining;
    int moved;

    if (!inventory || !furnace || item_stack_is_empty(&furnace->output))
        return false;

    if (item_stack_is_empty(&inventory->cursor)) {
        inventory->cursor = furnace->output;
        item_stack_clear(&furnace->output);
        furnace_try_start(furnace);
        return true;
    }

    if (inventory->cursor.item != furnace->output.item ||
        inventory->cursor.count >= ITEM_STACK_MAX)
        return false;

    remaining = item_stack_add(&inventory->cursor,
                               furnace->output.item,
                               furnace->output.count);
    moved = (int)furnace->output.count - remaining;
    if (moved <= 0)
        return false;

    item_stack_remove(&furnace->output, moved);
    furnace_try_start(furnace);
    return true;
}

static ItemStack *furnace_stack_for_area(FurnaceState *furnace,
                                         FurnaceSlotArea area)
{
    if (!furnace)
        return NULL;

    switch (area) {
    case FURNACE_SLOT_INPUT:
        return &furnace->input;
    case FURNACE_SLOT_FUEL:
        return &furnace->fuel;
    default:
        return NULL;
    }
}

static bool furnace_click_stack_left(SurvivalInventory *inventory,
                                     ItemStack *slot,
                                     FurnaceSlotArea area)
{
    if (!inventory || !slot)
        return false;

    if (item_stack_is_empty(&inventory->cursor)) {
        if (item_stack_is_empty(slot))
            return false;

        inventory->cursor = *slot;
        item_stack_clear(slot);
        return true;
    }

    if (!furnace_slot_accepts_item(area, inventory->cursor.item))
        return false;

    if (item_stack_is_empty(slot)) {
        *slot = inventory->cursor;
        item_stack_clear(&inventory->cursor);
        return true;
    }

    if (slot->item == inventory->cursor.item &&
        slot->count < ITEM_STACK_MAX) {
        int remaining = item_stack_add(slot,
                                       inventory->cursor.item,
                                       inventory->cursor.count);

        inventory->cursor.count = (uint8_t)remaining;
        if (remaining == 0)
            item_stack_clear(&inventory->cursor);
        return true;
    }

    {
        ItemStack tmp = *slot;

        *slot = inventory->cursor;
        inventory->cursor = tmp;
    }
    return true;
}

static bool furnace_click_stack_right(SurvivalInventory *inventory,
                                      ItemStack *slot,
                                      FurnaceSlotArea area)
{
    if (!inventory || !slot)
        return false;

    if (item_stack_is_empty(&inventory->cursor)) {
        int take;

        if (item_stack_is_empty(slot))
            return false;

        take = ((int)slot->count + 1) / 2;
        inventory->cursor.item = slot->item;
        inventory->cursor.count = 0;
        item_stack_add(&inventory->cursor, slot->item, take);
        item_stack_remove(slot, take);
        return true;
    }

    if (!furnace_slot_accepts_item(area, inventory->cursor.item))
        return false;

    if (item_stack_is_empty(slot)) {
        slot->item = inventory->cursor.item;
        slot->count = 1;
        item_stack_remove(&inventory->cursor, 1);
        return true;
    }

    if (slot->item == inventory->cursor.item &&
        slot->count < ITEM_STACK_MAX) {
        item_stack_add(slot, inventory->cursor.item, 1);
        item_stack_remove(&inventory->cursor, 1);
        return true;
    }

    return false;
}

static bool furnace_click_slot(SurvivalInventory *inventory,
                               FurnaceState *furnace,
                               FurnaceSlotArea area,
                               bool right_click)
{
    ItemStack *slot;
    bool changed;

    if (!inventory || !furnace)
        return false;

    if (area == FURNACE_SLOT_OUTPUT)
        return furnace_take_output(inventory, furnace);

    slot = furnace_stack_for_area(furnace, area);
    if (!slot)
        return false;

    changed = right_click ?
        furnace_click_stack_right(inventory, slot, area) :
        furnace_click_stack_left(inventory, slot, area);
    if (changed)
        furnace_try_start(furnace);
    return changed;
}

static bool try_open_targeted_furnace(const VoxelWorld *world,
                                      const Camera *cam,
                                      FurnaceState furnaces[],
                                      bool *furnace_open,
                                      int *open_furnace_index,
                                      float *cursor_x,
                                      float *cursor_y,
                                      Chat *chat)
{
    BlockTarget target = {0};
    FurnaceState *furnace;

    if (!world || !cam || !furnaces || !furnace_open)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit ||
        world_get_block(world, target.hit_x, target.hit_y, target.hit_z) !=
            BLOCK_FURNACE)
        return false;

    furnace = furnace_state_get_or_create(furnaces,
                                          target.hit_x,
                                          target.hit_y,
                                          target.hit_z);
    if (!furnace) {
        if (chat)
            chat_log(chat, "furnace storage is full");
        return true;
    }

    *furnace_open = true;
    if (open_furnace_index)
        *open_furnace_index = (int)(furnace - furnaces);
    if (cursor_x)
        *cursor_x = SCREEN_WIDTH * 0.5f;
    if (cursor_y)
        *cursor_y = SCREEN_HEIGHT * 0.5f;
    if (chat)
        chat_log(chat, "opened furnace");
    return true;
}

static void furnace_states_draw(RenderContext *ctx,
                                const FurnaceState furnaces[],
                                float world_time)
{
    if (!ctx || !furnaces)
        return;

    for (int i = 0; i < FURNACE_MAX_STATES; i++) {
        const FurnaceState *furnace = &furnaces[i];
        float progress;
        float flicker;
        Vec3 flame_center;

        if (!furnace->active || !furnace->smelting)
            continue;

        progress = furnace->timer / FURNACE_SMELT_SECONDS;
        if (progress < 0.0f)
            progress = 0.0f;
        if (progress > 1.0f)
            progress = 1.0f;

        flicker = (sinf(world_time * 42.0f + (float)i * 1.7f) + 1.0f) * 0.5f;
        flame_center = (Vec3){
            (float)furnace->x + 0.5f,
            (float)furnace->y + 1.02f + progress * 0.22f + flicker * 0.04f,
            (float)furnace->z + 0.5f,
        };
        renderer_draw_world_billboard_tile(ctx,
                                           flame_center,
                                           0.32f + flicker * 0.16f,
                                           TEX_TILE_TORCH,
                                           QUAD_FLAG_ALPHA_KEY |
                                               QUAD_FLAG_ZTEST);
    }
}

static const char *place_result_name(PlaceResult r)
{
    switch (r) {
    case PLACE_OK:                    return "ok";
    case PLACE_FAIL_BAD_TYPE:         return "bad-type";
    case PLACE_FAIL_NO_TRACE:         return "no-trace";
    case PLACE_FAIL_NO_AIR_NEAR_HIT:  return "no-air-near-hit";
    case PLACE_FAIL_TARGET_OCCUPIED:  return "target-occupied";
    case PLACE_FAIL_NO_SUPPORT:       return "no-support";
    case PLACE_FAIL_PLAYER_BLOCKED:   return "player-blocked";
    case PLACE_FAIL_WORLD_REJECTED:   return "world-rejected";
    default:                          return "unknown";
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

static int command_coord_base(const Player *player, int axis)
{
    if (!player)
        return 0;

    switch (axis) {
    case 0:
        return (int)floorf(player->x);
    case 1:
        return (int)floorf(player->y);
    case 2:
        return (int)floorf(player->z);
    default:
        return 0;
    }
}

static int command_resolve_coord(const GameCommandCoord *coord,
                                 const Player *player, int axis)
{
    long long resolved;

    if (!coord)
        return 0;
    if (coord->relative) {
        resolved = (long long)command_coord_base(player, axis) +
                   (long long)coord->value;
        if (resolved < INT_MIN)
            return INT_MIN;
        if (resolved > INT_MAX)
            return INT_MAX;
        return (int)resolved;
    }
    return coord->value;
}

static int command_min_i(int a, int b)
{
    return a < b ? a : b;
}

static int command_max_i(int a, int b)
{
    return a > b ? a : b;
}

static int command_floor_div_i(int value, int divisor)
{
    int q = value / divisor;
    int r = value % divisor;

    if (r < 0)
        q--;
    return q;
}

static const char *command_block_name(BlockID block)
{
    if (block >= BLOCK_AIR && block < NUM_BLOCK_TYPES &&
        BlockRegistry[block].name)
        return BlockRegistry[block].name;
    return "unknown";
}

static bool command_check_float_range(Chat *chat, const char *name,
                                      float value, float min_value,
                                      float max_value)
{
    if (value < min_value || value > max_value) {
        chat_log(chat, "%s must be %.2f..%.2f",
                 name, min_value, max_value);
        return false;
    }

    return true;
}

static void command_sync_jump_velocity_to_height(Player *player)
{
    if (!player)
        return;

    if (player->physics.gravity > 0.0f) {
        player->physics.jump_velocity =
            sqrtf(2.0f * player->physics.gravity *
                  player->physics.jump_height);
    } else if (player->physics.jump_height <= 0.0f) {
        player->physics.jump_velocity = 0.0f;
    }
}

static void command_sync_jump_height_to_velocity(Player *player)
{
    if (!player)
        return;

    if (player->physics.gravity > 0.0f) {
        player->physics.jump_height =
            (player->physics.jump_velocity * player->physics.jump_velocity) /
            (2.0f * player->physics.gravity);
    } else if (player->physics.jump_velocity <= 0.0f) {
        player->physics.jump_height = 0.0f;
    }
}

static bool execute_physics_command(Chat *chat, Player *player,
                                    const GameCommandAst *ast)
{
    GameCommandPhysicsProperty property;
    float value;

    if (!player || !ast)
        return true;

    if (ast->action == GAME_COMMAND_ACTION_RESET) {
        player_reset_physics(player);
        chat_log(chat, "physics: reset");
        return true;
    }

    property = ast->value.physics.property;
    value = ast->value.physics.value;

    switch (property) {
    case GAME_COMMAND_PHYSICS_GRAVITY:
        if (!command_check_float_range(chat, "gravity", value, 0.0f, 200.0f))
            return true;
        player->physics.gravity = value;
        command_sync_jump_velocity_to_height(player);
        break;
    case GAME_COMMAND_PHYSICS_PLAYER_SPEED:
        if (!command_check_float_range(chat, "player_speed", value,
                                       0.0f, 100.0f))
            return true;
        player->physics.max_speed = value;
        break;
    case GAME_COMMAND_PHYSICS_SPRINT_MULTIPLIER:
        if (!command_check_float_range(chat, "sprint_multiplier", value,
                                       0.0f, 10.0f))
            return true;
        player->physics.sprint_multiplier = value;
        break;
    case GAME_COMMAND_PHYSICS_JUMP_VELOCITY:
        if (!command_check_float_range(chat, "jump_velocity", value,
                                       0.0f, 100.0f))
            return true;
        player->physics.jump_velocity = value;
        command_sync_jump_height_to_velocity(player);
        break;
    case GAME_COMMAND_PHYSICS_JUMP_HEIGHT:
        if (!command_check_float_range(chat, "jump_height", value,
                                       0.0f, 64.0f))
            return true;
        if (player->physics.gravity <= 0.0f && value > 0.0f) {
            chat_log(chat, "jump_height needs gravity above zero");
            return true;
        }
        player->physics.jump_height = value;
        command_sync_jump_velocity_to_height(player);
        break;
    case GAME_COMMAND_PHYSICS_FLY_SPEED:
        if (!command_check_float_range(chat, "fly_speed", value,
                                       0.0f, 100.0f))
            return true;
        player->physics.fly_speed = value;
        break;
    default:
        chat_log(chat, "unknown physics property");
        return true;
    }

    chat_log(chat, "physics: %s %.2f",
             game_command_physics_property_name(property), value);
    return true;
}

static void command_mark_region_mesh_priority(VoxelWorld *world,
                                              int min_x, int max_x,
                                              int min_z, int max_z)
{
    int min_cx;
    int max_cx;
    int min_cz;
    int max_cz;

    if (!world)
        return;

    min_cx = command_floor_div_i(min_x, WORLD_CHUNK_SIZE);
    max_cx = command_floor_div_i(max_x, WORLD_CHUNK_SIZE);
    min_cz = command_floor_div_i(min_z, WORLD_CHUNK_SIZE);
    max_cz = command_floor_div_i(max_z, WORLD_CHUNK_SIZE);

    for (int cz = min_cz; cz <= max_cz; cz++) {
        for (int cx = min_cx; cx <= max_cx; cx++) {
            world_mark_chunk_mesh_edit_priority(
                world, cx * WORLD_CHUNK_SIZE, cz * WORLD_CHUNK_SIZE);
        }
    }
}

static bool execute_setblock_command(Chat *chat, Player *player,
                                     VoxelWorld *world,
                                     const GameCommandAst *ast)
{
    int x;
    int y;
    int z;
    BlockID block;

    if (!world || !ast)
        return true;

    x = command_resolve_coord(&ast->value.setblock.x, player, 0);
    y = command_resolve_coord(&ast->value.setblock.y, player, 1);
    z = command_resolve_coord(&ast->value.setblock.z, player, 2);
    block = ast->value.setblock.block;

    if (!world_set_block(world, x, y, z, block)) {
        chat_log(chat, "setblock failed at %d %d %d", x, y, z);
        return true;
    }

    world_mark_chunk_mesh_edit_priority(world, x, z);
    chat_log(chat, "setblock: %d %d %d -> %s",
             x, y, z, command_block_name(block));
    return true;
}

static bool execute_fill_command(Chat *chat, Player *player,
                                 VoxelWorld *world,
                                 const GameCommandAst *ast)
{
    int ax;
    int ay;
    int az;
    int bx;
    int by;
    int bz;
    int min_x;
    int min_y;
    int min_z;
    int max_x;
    int max_y;
    int max_z;
    long long span_x;
    long long span_y;
    long long span_z;
    long long volume;
    int ok_count = 0;
    int fail_count = 0;
    BlockID block;

    if (!world || !ast)
        return true;

    ax = command_resolve_coord(&ast->value.fill.x1, player, 0);
    ay = command_resolve_coord(&ast->value.fill.y1, player, 1);
    az = command_resolve_coord(&ast->value.fill.z1, player, 2);
    bx = command_resolve_coord(&ast->value.fill.x2, player, 0);
    by = command_resolve_coord(&ast->value.fill.y2, player, 1);
    bz = command_resolve_coord(&ast->value.fill.z2, player, 2);

    min_x = command_min_i(ax, bx);
    min_y = command_min_i(ay, by);
    min_z = command_min_i(az, bz);
    max_x = command_max_i(ax, bx);
    max_y = command_max_i(ay, by);
    max_z = command_max_i(az, bz);

    span_x = (long long)max_x - (long long)min_x + 1LL;
    span_y = (long long)max_y - (long long)min_y + 1LL;
    span_z = (long long)max_z - (long long)min_z + 1LL;
    if (span_x > COMMAND_FILL_MAX_BLOCKS ||
        span_y > COMMAND_FILL_MAX_BLOCKS ||
        span_z > COMMAND_FILL_MAX_BLOCKS ||
        span_x > COMMAND_FILL_MAX_BLOCKS / span_y ||
        (span_x * span_y) > COMMAND_FILL_MAX_BLOCKS / span_z) {
        chat_log(chat, "fill too large: max %d blocks",
                 COMMAND_FILL_MAX_BLOCKS);
        return true;
    }
    volume = span_x * span_y * span_z;

    block = ast->value.fill.block;
    for (long long y = min_y; y <= max_y; y++) {
        for (long long z = min_z; z <= max_z; z++) {
            for (long long x = min_x; x <= max_x; x++) {
                if (world_set_block(world, (int)x, (int)y, (int)z, block))
                    ok_count++;
                else
                    fail_count++;
            }
        }
    }

    if (ok_count > 0)
        command_mark_region_mesh_priority(world, min_x, max_x, min_z, max_z);

    if (fail_count > 0) {
        chat_log(chat, "fill: %d/%lld blocks -> %s (%d failed)",
                 ok_count, volume, command_block_name(block), fail_count);
    } else {
        chat_log(chat, "fill: %d blocks -> %s",
                 ok_count, command_block_name(block));
    }
    return true;
}

static bool execute_give_command(Chat *chat, SurvivalInventory *inventory,
                                 const GameCommandAst *ast)
{
    ItemID item;
    int count;
    int leftover;
    int added;

    if (!inventory || !ast)
        return true;

    item = ast->value.give.item;
    count = ast->value.give.count;
    leftover = survival_inventory_add_item(inventory, item, count);
    added = count - leftover;

    if (added > 0)
        chat_log(chat, "give: %s x%d", item_name(item), added);
    if (leftover > 0)
        chat_log(chat, "inventory full: %d not added", leftover);

    return true;
}

static bool execute_items_command(Chat *chat, const GameCommandAst *ast)
{
    int count = game_command_give_name_count();
    int pages;
    int page;
    int start;
    int end;

    if (!chat)
        return true;

    if (count <= 0) {
        chat_log(chat, "no giveable item names");
        return true;
    }

    pages = (count + COMMAND_ITEMS_PER_PAGE - 1) / COMMAND_ITEMS_PER_PAGE;
    page = ast ? ast->value.items.page : 1;
    if (page < 1)
        page = 1;
    if (page > pages)
        page = pages;

    start = (page - 1) * COMMAND_ITEMS_PER_PAGE;
    end = start + COMMAND_ITEMS_PER_PAGE;
    if (end > count)
        end = count;

    chat_log(chat, "items %d/%d:", page, pages);
    for (int i = start; i < end; i++) {
        const char *name = game_command_give_name_at(i);
        if (name)
            chat_log(chat, "%s", name);
    }
    chat_log(chat, "use /give <name> [count]");
    return true;
}

static void handle_chat_tab_completion(Chat *chat)
{
    char completed[CHAT_LINE_MAX + 1];

    if (!chat || !chat_is_open(chat))
        return;

    if (!chat->completion_active) {
        snprintf(chat->completion_base, sizeof(chat->completion_base),
                 "%s", chat->input);
        chat->completion_index = 0;
        chat->completion_active = true;
    } else {
        chat->completion_index++;
    }

    if (game_command_complete(chat->completion_base,
                              chat->completion_index,
                              completed, sizeof(completed))) {
        chat_set_input(chat, completed);
    } else {
        chat_reset_completion(chat);
    }
}

static bool execute_chat_command(Chat *chat, Player *player,
                                 VoxelWorld *world,
                                 SurvivalInventory *inventory,
                                 float *world_time,
                                 bool *kill_requested,
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
    case GAME_COMMAND_KIND_PHYSICS:
        return execute_physics_command(chat, player, &parsed.ast);
    case GAME_COMMAND_KIND_SETBLOCK:
        return execute_setblock_command(chat, player, world, &parsed.ast);
    case GAME_COMMAND_KIND_FILL:
        return execute_fill_command(chat, player, world, &parsed.ast);
    case GAME_COMMAND_KIND_GIVE:
        return execute_give_command(chat, inventory, &parsed.ast);
    case GAME_COMMAND_KIND_ITEMS:
        return execute_items_command(chat, &parsed.ast);
    case GAME_COMMAND_KIND_KILL:
        if (kill_requested)
            *kill_requested = true;
        return true;
    case GAME_COMMAND_KIND_HELP:
        chat_log(chat, "/time set day|night");
        chat_log(chat, "/gamemode set survival|creative|spectator");
        chat_log(chat, "/physics set gravity|speed|jump_height <value>");
        chat_log(chat, "/setblock <x> <y> <z> <block>");
        chat_log(chat, "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>");
        chat_log(chat, "/give [me] <item> [count]");
        chat_log(chat, "/items [page]");
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

static int clamp_ui_craft_grid_dim(int craft_grid_dim)
{
    return craft_grid_dim >= SURVIVAL_CRAFT_GRID_TABLE ?
        SURVIVAL_CRAFT_GRID_TABLE : SURVIVAL_CRAFT_GRID_PLAYER;
}

static void survival_inventory_layout(SurvivalInventoryLayout *layout,
                                      int craft_grid_dim)
{
    int grid_dim = clamp_ui_craft_grid_dim(craft_grid_dim);
    const float slot = HOTBAR_SLOT_PIXELS;
    const float gap = HOTBAR_GAP_PIXELS;
    const float storage_w =
        SURVIVAL_HOTBAR_SLOT_COUNT * slot +
        (SURVIVAL_HOTBAR_SLOT_COUNT - 1) * gap;
    const float recipe_w = 190.0f;
    const float group_gap = 8.0f;
    const float panel_w = storage_w + 24.0f;
    const float panel_h = grid_dim == SURVIVAL_CRAFT_GRID_TABLE ? 336.0f : 284.0f;
    const float group_w = recipe_w + group_gap + panel_w;
    const float recipe_x = floorf((SCREEN_WIDTH - group_w) * 0.5f);
    const float panel_x = recipe_x + recipe_w + group_gap;
    const float panel_y = floorf((SCREEN_HEIGHT - panel_h) * 0.5f) - 10.0f;
    const float craft_span = (float)grid_dim * slot +
                             (float)(grid_dim - 1) * gap;

    if (!layout)
        return;

    layout->panel_x = panel_x;
    layout->panel_y = panel_y;
    layout->panel_w = panel_w;
    layout->panel_h = panel_h;
    layout->recipe_x = recipe_x;
    layout->recipe_y = panel_y;
    layout->recipe_w = recipe_w;
    layout->recipe_h = panel_h;
    layout->slot = slot;
    layout->gap = gap;
    layout->craft_grid_dim = grid_dim;
    layout->craft_x = panel_x + 28.0f;
    layout->craft_y = panel_y + 38.0f;
    layout->output_x = layout->craft_x + craft_span + 28.0f;
    layout->output_y = layout->craft_y + (craft_span - slot) * 0.5f;
    layout->main_x = panel_x + 12.0f;
    layout->main_y = panel_y +
        (grid_dim == SURVIVAL_CRAFT_GRID_TABLE ? 168.0f : 116.0f);
    layout->hotbar_x = layout->main_x;
    layout->hotbar_y = layout->main_y + 3.0f * (slot + gap) + 16.0f;
}

static bool point_in_rect(float x, float y,
                          float x0, float y0,
                          float x1, float y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static InventoryHit survival_inventory_hit_test(float cursor_x, float cursor_y,
                                                int craft_grid_dim)
{
    SurvivalInventoryLayout layout;
    InventoryHit hit = { INVENTORY_SLOT_NONE, -1 };

    survival_inventory_layout(&layout, craft_grid_dim);

    for (int row = 0; row < layout.craft_grid_dim; row++) {
        for (int col = 0; col < layout.craft_grid_dim; col++) {
            float x0 = layout.craft_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.craft_y + (layout.slot + layout.gap) * (float)row;

            if (point_in_rect(cursor_x, cursor_y,
                              x0, y0, x0 + layout.slot, y0 + layout.slot)) {
                hit.area = INVENTORY_SLOT_CRAFT;
                hit.index = row * SURVIVAL_CRAFT_GRID_TABLE + col;
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

static void furnace_layout(FurnaceLayout *layout)
{
    const float slot = HOTBAR_SLOT_PIXELS;
    const float gap = HOTBAR_GAP_PIXELS;
    const float storage_w =
        SURVIVAL_HOTBAR_SLOT_COUNT * slot +
        (SURVIVAL_HOTBAR_SLOT_COUNT - 1) * gap;
    const float panel_w = storage_w + 24.0f;
    const float panel_h = 300.0f;
    const float panel_x = floorf((SCREEN_WIDTH - panel_w) * 0.5f);
    const float panel_y = floorf((SCREEN_HEIGHT - panel_h) * 0.5f) - 8.0f;

    if (!layout)
        return;

    layout->panel_x = panel_x;
    layout->panel_y = panel_y;
    layout->panel_w = panel_w;
    layout->panel_h = panel_h;
    layout->slot = slot;
    layout->gap = gap;
    layout->input_x = panel_x + 68.0f;
    layout->input_y = panel_y + 42.0f;
    layout->fuel_x = layout->input_x;
    layout->fuel_y = layout->input_y + slot + 12.0f;
    layout->output_x = panel_x + panel_w - 86.0f;
    layout->output_y = panel_y + 62.0f;
    layout->progress_x = layout->input_x + slot + 26.0f;
    layout->progress_y = panel_y + 76.0f;
    layout->progress_w = layout->output_x - layout->progress_x - 24.0f;
    layout->progress_h = 10.0f;
    layout->main_x = panel_x + 12.0f;
    layout->main_y = panel_y + 128.0f;
    layout->hotbar_x = layout->main_x;
    layout->hotbar_y = layout->main_y + 3.0f * (slot + gap) + 16.0f;
}

static FurnaceHit furnace_hit_test(float cursor_x, float cursor_y)
{
    FurnaceLayout layout;
    FurnaceHit hit = { FURNACE_SLOT_NONE, -1 };

    furnace_layout(&layout);

    if (point_in_rect(cursor_x, cursor_y,
                      layout.input_x, layout.input_y,
                      layout.input_x + layout.slot,
                      layout.input_y + layout.slot)) {
        hit.area = FURNACE_SLOT_INPUT;
        hit.index = 0;
        return hit;
    }

    if (point_in_rect(cursor_x, cursor_y,
                      layout.fuel_x, layout.fuel_y,
                      layout.fuel_x + layout.slot,
                      layout.fuel_y + layout.slot)) {
        hit.area = FURNACE_SLOT_FUEL;
        hit.index = 0;
        return hit;
    }

    if (point_in_rect(cursor_x, cursor_y,
                      layout.output_x, layout.output_y,
                      layout.output_x + layout.slot,
                      layout.output_y + layout.slot)) {
        hit.area = FURNACE_SLOT_OUTPUT;
        hit.index = 0;
        return hit;
    }

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
            float x0 = layout.main_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.main_y + (layout.slot + layout.gap) * (float)row;

            if (point_in_rect(cursor_x, cursor_y,
                              x0, y0, x0 + layout.slot, y0 + layout.slot)) {
                hit.area = FURNACE_SLOT_STORAGE;
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
            hit.area = FURNACE_SLOT_STORAGE;
            hit.index = col;
            return hit;
        }
    }

    return hit;
}

static void draw_text_shadow(RenderContext *ctx, const char *text,
                             float x, float y, uint8_t palette_index)
{
    int len;

    if (!ctx || !text)
        return;

    len = (int)strlen(text);
    chat_draw_text(ctx, text, len, x + 1.0f, y + 1.0f, 0);
    chat_draw_text(ctx, text, len, x, y, palette_index);
}

static void draw_inventory_panel(RenderContext *ctx,
                                 float x, float y,
                                 float w, float h)
{
    renderer_fill_rect(ctx, x, y, x + w, y + h, 14, 0);
    renderer_fill_rect(ctx, x + 2.0f, y + 2.0f,
                       x + w - 2.0f, y + h - 2.0f,
                       0, QUAD_ALPHA_25);
    renderer_fill_rect(ctx, x + 4.0f, y + 4.0f,
                       x + w - 4.0f, y + 5.0f,
                       5, QUAD_ALPHA_50);
}

static uint8_t item_icon_flags(ItemID item)
{
    if (!item_is_placeable_block(item))
        return QUAD_FLAG_ALPHA_KEY;
    if (item == (ItemID)BLOCK_DOOR)
        return QUAD_FLAG_ALPHA_KEY;

    {
        BlockID block = item_place_block(item);

        if (block_is_alpha_keyed(block) || block_is_translucent(block))
            return QUAD_FLAG_ALPHA_KEY;
    }

    return 0;
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
                              item_icon_flags(stack->item));
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
    uint8_t inner = hovered ? 14 : 0;

    renderer_fill_rect(ctx, x0, y0, x0 + slot, y0 + slot, border, 0);
    renderer_fill_rect(ctx,
                       x0 + 1.0f, y0 + 1.0f,
                       x0 + slot - 1.0f, y0 + slot - 1.0f,
                       inner, QUAD_ALPHA_25);
    renderer_fill_rect(ctx, x0 + 2.0f, y0 + 2.0f,
                       x0 + slot - 2.0f, y0 + 3.0f,
                       5, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, x0 + 2.0f, y0 + 2.0f,
                       x0 + 3.0f, y0 + slot - 2.0f,
                       5, QUAD_ALPHA_50);
    renderer_fill_rect(ctx, x0 + 2.0f, y0 + slot - 3.0f,
                       x0 + slot - 2.0f, y0 + slot - 2.0f,
                       0, QUAD_ALPHA_50);
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

static void draw_recipe_icon(RenderContext *ctx, ItemID item,
                             float x, float y, float size)
{
    if (item == ITEM_NONE)
        return;

    renderer_draw_screen_tile(ctx, x + 1.0f, y + 1.0f,
                              x + size - 1.0f, y + size - 1.0f,
                              item_texture_id(item), item_icon_flags(item));
}

static int recipe_lookup_page_count(int craft_grid_dim)
{
    int count = survival_craft_recipe_count_for_grid(craft_grid_dim);

    return count > 0 ?
        (count + RECIPE_LOOKUP_RECIPES_PER_PAGE - 1) /
            RECIPE_LOOKUP_RECIPES_PER_PAGE :
        1;
}

static int clamp_recipe_lookup_page(int craft_grid_dim, int page)
{
    int page_count = recipe_lookup_page_count(craft_grid_dim);

    if (page < 0)
        return 0;
    if (page >= page_count)
        return page_count - 1;
    return page;
}

static ItemID recipe_input_for_cell(const CraftRecipeView *recipe,
                                    int display_dim,
                                    int row,
                                    int col)
{
    if (!recipe)
        return ITEM_NONE;

    if (recipe->shapeless) {
        int cell = row * display_dim + col;
        int seen = 0;

        for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++) {
            if (recipe->inputs[i] == ITEM_NONE)
                continue;
            if (seen == cell)
                return recipe->inputs[i];
            seen++;
        }
        return ITEM_NONE;
    }

    if (row >= recipe->height || col >= recipe->width)
        return ITEM_NONE;
    return recipe->inputs[row * recipe->width + col];
}

static void draw_recipe_grid(RenderContext *ctx, const CraftRecipeView *recipe,
                             int display_dim, float x, float y,
                             float slot, float gap)
{
    for (int row = 0; row < display_dim; row++) {
        for (int col = 0; col < display_dim; col++) {
            float x0 = x + (slot + gap) * (float)col;
            float y0 = y + (slot + gap) * (float)row;
            ItemID item = recipe_input_for_cell(recipe, display_dim, row, col);

            renderer_fill_rect(ctx, x0, y0, x0 + slot, y0 + slot, 14, 0);
            renderer_fill_rect(ctx, x0 + 1.0f, y0 + 1.0f,
                               x0 + slot - 1.0f, y0 + slot - 1.0f,
                               0, QUAD_ALPHA_25);
            draw_recipe_icon(ctx, item, x0, y0, slot);
        }
    }
}

static void draw_recipe_arrow_button(RenderContext *ctx,
                                     float x, float y,
                                     bool right,
                                     bool enabled)
{
    const float size = 22.0f;
    uint8_t frame = enabled ? 5 : 14;
    uint8_t fill = enabled ? 14 : 0;

    renderer_fill_rect(ctx, x, y, x + size, y + size, frame, 0);
    renderer_fill_rect(ctx, x + 1.0f, y + 1.0f,
                       x + size - 1.0f, y + size - 1.0f,
                       fill, QUAD_ALPHA_25);

    for (int i = 0; i < 5; i++) {
        float yy = y + 6.0f + (float)i * 2.0f;
        float len = 2.0f + (float)(i < 3 ? i : 4 - i) * 3.0f;
        float x0 = right ? x + 7.0f : x + 15.0f - len;
        float x1 = right ? x + 7.0f + len : x + 15.0f;

        renderer_fill_rect(ctx, x0, yy, x1, yy + 2.0f, frame, 0);
    }
}

static void draw_recipe_lookup_card(RenderContext *ctx,
                                    const SurvivalInventoryLayout *layout,
                                    int recipe_index,
                                    float card_x,
                                    float card_y,
                                    float card_w,
                                    float card_h)
{
    CraftRecipeView recipe;
    ItemStack output;
    int display_dim;
    int label_len;
    float grid_slot;
    float grid_gap = 4.0f;
    float grid_x;
    float grid_y;
    float grid_span;
    float arrow_y;
    float out_x;
    float out_y;

    if (!ctx || !layout)
        return;
    if (!survival_craft_recipe_view_for_grid(recipe_index,
                                             layout->craft_grid_dim,
                                             &recipe))
        return;

    display_dim = layout->craft_grid_dim;
    grid_slot = display_dim == SURVIVAL_CRAFT_GRID_TABLE ? 20.0f : 26.0f;
    grid_span = (float)display_dim * grid_slot +
                (float)(display_dim - 1) * grid_gap;

    renderer_fill_rect(ctx, card_x, card_y,
                       card_x + card_w, card_y + card_h,
                       0, QUAD_ALPHA_25);

    label_len = (int)strlen(item_name(recipe.output));
    if (label_len > 22)
        label_len = 22;
    chat_draw_text(ctx, item_name(recipe.output), label_len,
                   card_x + 8.0f, card_y + 8.0f, 5);

    grid_x = card_x + (display_dim == SURVIVAL_CRAFT_GRID_TABLE ? 8.0f : 12.0f);
    grid_y = card_y + 30.0f;
    draw_recipe_grid(ctx, &recipe, display_dim, grid_x, grid_y,
                     grid_slot, grid_gap);

    arrow_y = grid_y + grid_span * 0.5f;
    renderer_fill_rect(ctx, grid_x + grid_span + 12.0f,
                       arrow_y - 2.0f,
                       grid_x + grid_span + 31.0f,
                       arrow_y + 2.0f, 5, 0);
    renderer_fill_rect(ctx, grid_x + grid_span + 27.0f,
                       arrow_y - 7.0f,
                       grid_x + grid_span + 35.0f,
                       arrow_y + 7.0f, 5, 0);

    output.item = recipe.output;
    output.count = recipe.output_count;
    out_x = card_x + card_w - 42.0f;
    out_y = arrow_y - HOTBAR_SLOT_PIXELS * 0.5f;
    draw_inventory_slot(ctx, &output, out_x, out_y, false, false);
}

static int recipe_lookup_nav_hit(float cursor_x,
                                 float cursor_y,
                                 int craft_grid_dim)
{
    SurvivalInventoryLayout layout;
    int page_count = recipe_lookup_page_count(craft_grid_dim);
    float nav_y;

    if (page_count <= 1)
        return 0;

    survival_inventory_layout(&layout, craft_grid_dim);
    nav_y = layout.recipe_y + layout.recipe_h - 30.0f;
    if (point_in_rect(cursor_x, cursor_y,
                      layout.recipe_x + 10.0f, nav_y,
                      layout.recipe_x + 32.0f, nav_y + 22.0f))
        return -1;
    if (point_in_rect(cursor_x, cursor_y,
                      layout.recipe_x + layout.recipe_w - 32.0f, nav_y,
                      layout.recipe_x + layout.recipe_w - 10.0f,
                      nav_y + 22.0f))
        return 1;
    return 0;
}

static void draw_recipe_lookup(RenderContext *ctx,
                               const SurvivalInventoryLayout *layout,
                               int recipe_page)
{
    char page_text[32];
    int recipe_count;
    int page_count;
    int page_len;
    float card_x;
    float card_y;
    float card_w;
    float card_h;
    float card_gap = 8.0f;
    float nav_y;

    if (!ctx || !layout)
        return;

    recipe_count = survival_craft_recipe_count_for_grid(layout->craft_grid_dim);
    page_count = recipe_lookup_page_count(layout->craft_grid_dim);
    recipe_page = clamp_recipe_lookup_page(layout->craft_grid_dim, recipe_page);

    draw_inventory_panel(ctx, layout->recipe_x, layout->recipe_y,
                         layout->recipe_w, layout->recipe_h);
    draw_text_shadow(ctx,
                     layout->craft_grid_dim == SURVIVAL_CRAFT_GRID_TABLE ?
                     "TABLE RECIPES" : "RECIPES",
                     layout->recipe_x + 10.0f, layout->recipe_y + 12.0f, 5);

    card_x = layout->recipe_x + 10.0f;
    card_y = layout->recipe_y + 38.0f;
    card_w = layout->recipe_w - 20.0f;
    nav_y = layout->recipe_y + layout->recipe_h - 30.0f;
    card_h = (nav_y - card_y - 8.0f - card_gap) * 0.5f;
    for (int i = 0; i < RECIPE_LOOKUP_RECIPES_PER_PAGE; i++) {
        int recipe_index = recipe_page * RECIPE_LOOKUP_RECIPES_PER_PAGE + i;

        if (recipe_index >= recipe_count)
            break;
        draw_recipe_lookup_card(ctx, layout, recipe_index,
                                card_x,
                                card_y + (card_h + card_gap) * (float)i,
                                card_w, card_h);
    }

    draw_recipe_arrow_button(ctx, layout->recipe_x + 10.0f, nav_y,
                             false, recipe_page > 0);
    draw_recipe_arrow_button(ctx, layout->recipe_x + layout->recipe_w - 32.0f,
                             nav_y, true, recipe_page < page_count - 1);
    snprintf(page_text, sizeof(page_text), "%d/%d", recipe_page + 1, page_count);
    page_len = (int)strlen(page_text);
    chat_draw_text(ctx, page_text, page_len,
                   layout->recipe_x + (layout->recipe_w -
                   (float)(page_len * chat_font_cell_w())) * 0.5f,
                   nav_y + 8.0f, 5);
}

static void draw_survival_inventory(RenderContext *ctx,
                                    const SurvivalInventory *inventory,
                                    int selected_slot,
                                    float cursor_x,
                                    float cursor_y,
                                    int recipe_page)
{
    SurvivalInventoryLayout layout;
    InventoryHit hover;
    int craft_grid_dim;

    if (!ctx || !inventory)
        return;

    craft_grid_dim = survival_inventory_craft_grid_dim(inventory);
    survival_inventory_layout(&layout, craft_grid_dim);
    hover = survival_inventory_hit_test(cursor_x, cursor_y, craft_grid_dim);

    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       0, QUAD_ALPHA_50);
    draw_recipe_lookup(ctx, &layout, recipe_page);
    draw_inventory_panel(ctx, layout.panel_x, layout.panel_y,
                         layout.panel_w, layout.panel_h);
    draw_text_shadow(ctx,
                     craft_grid_dim == SURVIVAL_CRAFT_GRID_TABLE ?
                     "CRAFTING TABLE" : "INVENTORY",
                     layout.panel_x + 12.0f, layout.panel_y + 12.0f, 5);
    draw_text_shadow(ctx, "HOTBAR", layout.hotbar_x,
                     layout.hotbar_y - 13.0f, 5);

    {
        float craft_span = (float)craft_grid_dim * layout.slot +
                           (float)(craft_grid_dim - 1) * layout.gap;

        renderer_fill_rect(ctx, layout.craft_x - 8.0f, layout.craft_y - 8.0f,
                           layout.output_x + layout.slot + 8.0f,
                           layout.craft_y + craft_span + 4.0f,
                           0, QUAD_ALPHA_25);
    }
    renderer_fill_rect(ctx, layout.main_x - 4.0f, layout.main_y - 4.0f,
                       layout.main_x + 9.0f * layout.slot + 8.0f * layout.gap + 4.0f,
                       layout.hotbar_y + layout.slot + 4.0f,
                       0, QUAD_ALPHA_25);

    for (int row = 0; row < craft_grid_dim; row++) {
        for (int col = 0; col < craft_grid_dim; col++) {
            int index = row * SURVIVAL_CRAFT_GRID_TABLE + col;
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

static void draw_furnace_progress(RenderContext *ctx,
                                  const FurnaceLayout *layout,
                                  const FurnaceState *furnace)
{
    float progress = 0.0f;
    float fill_w;
    float arrow_y;

    if (!ctx || !layout)
        return;

    if (furnace && furnace->smelting && FURNACE_SMELT_SECONDS > 0.0f) {
        progress = furnace->timer / FURNACE_SMELT_SECONDS;
        if (progress < 0.0f)
            progress = 0.0f;
        if (progress > 1.0f)
            progress = 1.0f;
    }

    arrow_y = layout->progress_y + layout->progress_h * 0.5f;
    renderer_fill_rect(ctx,
                       layout->progress_x - 2.0f,
                       arrow_y - 3.0f,
                       layout->progress_x + layout->progress_w - 6.0f,
                       arrow_y + 3.0f, 14, 0);
    renderer_fill_rect(ctx,
                       layout->progress_x + layout->progress_w - 11.0f,
                       arrow_y - 8.0f,
                       layout->progress_x + layout->progress_w,
                       arrow_y + 8.0f, 14, 0);

    fill_w = (layout->progress_w - 10.0f) * progress;
    if (fill_w > 0.0f) {
        renderer_fill_rect(ctx,
                           layout->progress_x,
                           arrow_y - 2.0f,
                           layout->progress_x + fill_w,
                           arrow_y + 2.0f, 8, 0);
        renderer_fill_rect(ctx,
                           layout->progress_x + fill_w - 2.0f,
                           arrow_y - 5.0f,
                           layout->progress_x + fill_w + 2.0f,
                           arrow_y + 5.0f, 8, 0);
    }

    if (progress > 0.0f) {
        float flame_x = layout->fuel_x + layout->slot + 7.0f;
        float flame_y = layout->fuel_y + 8.0f;
        float flame_h = 18.0f * (0.35f + progress * 0.65f);

        renderer_fill_rect(ctx, flame_x + 5.0f,
                           flame_y + 18.0f - flame_h,
                           flame_x + 13.0f, flame_y + 18.0f,
                           6, 0);
        renderer_fill_rect(ctx, flame_x + 7.0f,
                           flame_y + 18.0f - flame_h * 0.65f,
                           flame_x + 11.0f, flame_y + 18.0f,
                           8, 0);
    }
}

static void draw_furnace_ui(RenderContext *ctx,
                            const SurvivalInventory *inventory,
                            const FurnaceState *furnace,
                            int selected_slot,
                            float cursor_x,
                            float cursor_y)
{
    FurnaceLayout layout;
    FurnaceHit hover;

    if (!ctx || !inventory || !furnace)
        return;

    furnace_layout(&layout);
    hover = furnace_hit_test(cursor_x, cursor_y);

    renderer_fill_rect(ctx, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
                       0, QUAD_ALPHA_50);
    draw_inventory_panel(ctx, layout.panel_x, layout.panel_y,
                         layout.panel_w, layout.panel_h);
    renderer_draw_screen_tile(ctx,
                              layout.panel_x + 12.0f,
                              layout.panel_y + 10.0f,
                              layout.panel_x + 30.0f,
                              layout.panel_y + 28.0f,
                              TEX_TILE_FURNACE_FRONT,
                              0);
    draw_text_shadow(ctx, "FURNACE",
                     layout.panel_x + 36.0f, layout.panel_y + 16.0f, 5);

    renderer_fill_rect(ctx,
                       layout.input_x - 10.0f,
                       layout.input_y - 10.0f,
                       layout.output_x + layout.slot + 10.0f,
                       layout.fuel_y + layout.slot + 10.0f,
                       0, QUAD_ALPHA_25);
    draw_inventory_slot(ctx, &furnace->input,
                        layout.input_x, layout.input_y, false,
                        hover.area == FURNACE_SLOT_INPUT);
    draw_inventory_slot(ctx, &furnace->fuel,
                        layout.fuel_x, layout.fuel_y, false,
                        hover.area == FURNACE_SLOT_FUEL);
    draw_inventory_slot(ctx, &furnace->output,
                        layout.output_x, layout.output_y, false,
                        hover.area == FURNACE_SLOT_OUTPUT);
    draw_furnace_progress(ctx, &layout, furnace);

    renderer_fill_rect(ctx, layout.main_x - 4.0f, layout.main_y - 4.0f,
                       layout.main_x + 9.0f * layout.slot + 8.0f * layout.gap + 4.0f,
                       layout.hotbar_y + layout.slot + 4.0f,
                       0, QUAD_ALPHA_25);

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
            int index = SURVIVAL_HOTBAR_SLOT_COUNT +
                        row * SURVIVAL_HOTBAR_SLOT_COUNT + col;
            float x0 = layout.main_x + (layout.slot + layout.gap) * (float)col;
            float y0 = layout.main_y + (layout.slot + layout.gap) * (float)row;
            bool hovered = hover.area == FURNACE_SLOT_STORAGE &&
                           hover.index == index;

            draw_inventory_slot(ctx, &inventory->storage[index],
                                x0, y0, false, hovered);
        }
    }

    for (int col = 0; col < SURVIVAL_HOTBAR_SLOT_COUNT; col++) {
        float x0 = layout.hotbar_x + (layout.slot + layout.gap) * (float)col;
        bool hovered = hover.area == FURNACE_SLOT_STORAGE &&
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
                uint8_t flags = block_is_alpha_keyed(block) ?
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

static void draw_hungerbar(RenderContext *ctx, int food_units)
{
    float slot_left, slot_top, total_width;
    const float icon = 8.0f * HUD_SCALE;
    const float step = 8.0f * HUD_SCALE;

    if (food_units < 0)
        food_units = 0;
    if (food_units > PLAYER_MAX_FOOD_UNITS)
        food_units = PLAYER_MAX_FOOD_UNITS;

    hotbar_metrics(&slot_left, &slot_top, NULL, NULL, &total_width);
    const float health_top = slot_top - icon - 7.0f;
    const float hunger_right = slot_left + total_width;

    for (int i = 0; i < 10; i++) {
        float x0 = hunger_right - icon - (float)i * step;
        int threshold = (i + 1) * 2;
        uint8_t flags = QUAD_FLAG_ALPHA_KEY;

        renderer_draw_screen_tile(ctx,
                                  x0, health_top,
                                  x0 + icon, health_top + icon,
                                  TEX_TILE_DRUMSTICK_EMPTY,
                                  QUAD_FLAG_ALPHA_KEY);
        if (food_units <= i * 2)
            continue;
        if (food_units < threshold)
            flags |= QUAD_ALPHA_50;
        renderer_draw_screen_tile(ctx,
                                  x0, health_top,
                                  x0 + icon, health_top + icon,
                                  TEX_TILE_DRUMSTICK, flags);
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

static bool try_consume_held_food(SurvivalInventory *inventory,
                                  ItemEntityPool *drops,
                                  const Player *player,
                                  int hotbar_slot,
                                  int *food_units,
                                  Chat *chat)
{
    const ItemStack *held_stack;
    ItemID held_item;
    int gain;

    if (!inventory || !food_units)
        return false;

    held_stack = survival_inventory_hotbar_stack(inventory, hotbar_slot);
    if (item_stack_is_empty(held_stack))
        return false;

    held_item = held_stack->item;
    gain = item_food_units(held_item);
    if (gain <= 0)
        return false;

    if (*food_units >= PLAYER_MAX_FOOD_UNITS) {
        if (chat)
            chat_log(chat, "food is full");
        return true;
    }

    survival_inventory_remove_storage(inventory, hotbar_slot, 1);
    *food_units += gain;
    if (*food_units > PLAYER_MAX_FOOD_UNITS)
        *food_units = PLAYER_MAX_FOOD_UNITS;

    if (item_food_returns_bowl(held_item)) {
        int leftover = survival_inventory_add_item(inventory, ITEM_BOWL, 1);

        if (leftover > 0)
            item_entity_spawn_item_near_player(drops, player, ITEM_BOWL,
                                               leftover);
    }

    if (chat)
        chat_log(chat, "ate %s (+%d food)", item_name(held_item), gain);
    return true;
}

static bool try_toss_selected_item(SurvivalInventory *inventory,
                                   ItemEntityPool *drops,
                                   const Player *player,
                                   const Camera *cam,
                                   int hotbar_slot,
                                   Chat *chat)
{
    const ItemStack *held_stack;
    ItemID held_item;

    if (!inventory || !drops || !player || !cam)
        return false;

    held_stack = survival_inventory_hotbar_stack(inventory, hotbar_slot);
    if (item_stack_is_empty(held_stack)) {
        if (chat)
            chat_log(chat, "nothing to drop");
        return false;
    }

    held_item = held_stack->item;
    if (!survival_inventory_remove_storage(inventory, hotbar_slot, 1))
        return false;

    item_entity_toss_item(drops, player, camera_forward(cam), held_item, 1);
    if (chat)
        chat_log(chat, "dropped %s", item_name(held_item));
    return true;
}

static bool try_open_targeted_crafting_table(const VoxelWorld *world,
                                             const Camera *cam,
                                             SurvivalInventory *inventory,
                                             bool *inventory_open,
                                             float *cursor_x,
                                             float *cursor_y,
                                             Chat *chat)
{
    BlockTarget target = {0};

    if (!world || !cam || !inventory || !inventory_open)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;
    if (!target.hit ||
        world_get_block(world, target.hit_x, target.hit_y, target.hit_z) !=
            BLOCK_CRAFTING_TABLE)
        return false;

    survival_inventory_set_craft_grid_dim(inventory, SURVIVAL_CRAFT_GRID_TABLE);
    *inventory_open = true;
    if (cursor_x)
        *cursor_x = SCREEN_WIDTH * 0.5f;
    if (cursor_y)
        *cursor_y = SCREEN_HEIGHT * 0.5f;
    if (chat)
        chat_log(chat, "opened crafting table");
    return true;
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

    if (block_render_model(type) != BLOCK_RENDER_CUBE) {
        float size = 22.0f * s;
        float x0 = SCREEN_WIDTH - 42.0f * s - swing * 7.0f * s;
        float y0 = SCREEN_HEIGHT - 58.0f * s + swing * 12.0f * s;
        uint8_t tile = block_is_door(type) ?
            TEX_TILE_DOOR_ITEM : block_face_texture_id(type, FACE_FRONT);

        renderer_draw_custom_screen_quad(ctx,
            x0,                 y0,
            x0 + size,          y0 + 3.0f * s,
            x0 + size * 0.85f,  y0 + size + 7.0f * s,
            x0 - size * 0.15f,  y0 + size + 4.0f * s,
            tile,
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

static void draw_tool_in_hand(RenderContext *ctx, ItemID item, float swing_timer)
{
    const float s = HUD_SCALE;
    float swing;
    float x;
    float y;
    float size;
    float skew;
    float swing_left;
    float swing_down;

    if (!ctx || !item_is_tool(item))
        return;

    swing = hand_swing_phase(swing_timer);
    size = 70.0f * s;
    swing_left = 26.0f * s * swing;
    swing_down = 22.0f * s * swing;
    skew = 18.0f * s - 24.0f * s * swing;
    x = SCREEN_WIDTH - 76.0f * s - swing_left;
    y = SCREEN_HEIGHT - 92.0f * s + swing_down;

    renderer_draw_custom_screen_quad(ctx,
        x + skew,          y,
        x + size + skew,   y + 7.0f * s,
        x + size + 9.0f * s, y + size + 10.0f * s,
        x - 8.0f * s,       y + size,
        item_texture_id(item),
        QUAD_FLAG_ALPHA_KEY);
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
    int debug_enabled = env_flag("DEBUG", false);
    thread_affinity_pin_current("main", "VOXEL_MAIN_CPU", 0);

    RenderContext *ctx = renderer_init();
    static VoxelWorld world;
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    static InputState inp;
    input_init(&inp);

    static Chat chat;
    chat_init(&chat);

    static PauseMenu pause;
    pause_menu_init(&pause);

    init_block_types();
    world_init(&world);
    float mouse_sens = read_mouse_sensitivity();
    float fov_deg = read_camera_fov_degrees();
    int target_fps = read_target_fps();
    long frame_ns = 1000000000L / target_fps;
    int render_distance_chunks = read_render_distance_chunks();
    int stream_chunks_per_frame = read_stream_chunks_per_frame();
    int max_physics_steps_per_frame = read_max_physics_steps_per_frame();
    static SelectedWorld selected_world;
    int selected_hotbar_slot = 0;
    int selected_hotbar_page = 0;
    static SurvivalInventory survival_inventory;
    static ItemEntityPool item_drops;
    static FurnaceState furnace_states[FURNACE_MAX_STATES];
    bool inventory_open = false;
    bool furnace_open = false;
    int open_furnace_index = -1;
    int inventory_recipe_page = 0;
    float inventory_cursor_x = SCREEN_WIDTH * 0.5f;
    float inventory_cursor_y = SCREEN_HEIGHT * 0.5f;
    static PauseMenuSettings pause_settings;

home_menu_start:
    chat_init(&chat);
    pause_menu_init(&pause);
    memset(&selected_world, 0, sizeof(selected_world));
    selected_hotbar_slot = 0;
    selected_hotbar_page = 0;
    inventory_open = false;
    furnace_open = false;
    open_furnace_index = -1;
    inventory_recipe_page = 0;
    inventory_cursor_x = SCREEN_WIDTH * 0.5f;
    inventory_cursor_y = SCREEN_HEIGHT * 0.5f;
    memset(&pause_settings, 0, sizeof(pause_settings));
    memset(furnace_states, 0, sizeof(furnace_states));
    survival_inventory_init(&survival_inventory);
    item_entities_init(&item_drops);
    input_clear_text_queue(&inp);
    input_clear_mouse(&inp);

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
        .depth    = camera_focal_px_from_fov(fov_deg),
    };

    if (!world_init_infinite_procedural(&world,
                                        selected_world.seed,
                                        selected_world.stone_tries_per_chunk,
                                        selected_world.desert_lava_pools_enabled,
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
    pause_settings.mouse_sensitivity_percent =
        mouse_sensitivity_to_percent(mouse_sens);
    pause_settings.mouse_sensitivity_percent_min = MIN_MOUSE_SENS_PERCENT;
    pause_settings.mouse_sensitivity_percent_max = MAX_MOUSE_SENS_PERCENT;
    pause_settings.mouse_sensitivity_percent_step = MOUSE_SENS_PERCENT_STEP;
    mouse_sens = mouse_sensitivity_from_percent(
        pause_settings.mouse_sensitivity_percent);
    pause_settings.fov_degrees_x10 = fov_degrees_to_x10(fov_deg);
    pause_settings.fov_degrees_x10_min = MIN_FOV_DEG_X10;
    pause_settings.fov_degrees_x10_max = MAX_FOV_DEG_X10;
    pause_settings.fov_degrees_x10_step = FOV_DEG_X10_STEP;
    fov_deg = fov_degrees_from_x10(pause_settings.fov_degrees_x10);
    cam.depth = camera_focal_px_from_fov(fov_deg);

    bool mesh_worker_running = mesh_worker_start(&world);
    bool gen_worker_running = gen_worker_start(&world);

    if (debug_enabled) {
        printf("Controls: WASD=move  double-tap W=sprint  Space=jump/fly-up  double-tap Space=creative flight  Shift=crouch/fly-down  1-9=hotbar  Tab=hotbar page/chat autocomplete  F/LMB=break  R/RMB=place  Q=drop item  E=inventory  G=cycle mode  T=chat  Esc=pause/release mouse\n");
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
        printf("FOV: %.1f degrees (set VOXEL_FOV_DEG=30-150 to override)\n",
               fov_deg);
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
    float redstone_tick_accumulator = 0.0f;
    float break_timer = 0.0f;
    float break_duration = 0.0f;
    float hand_swing_timer = HAND_SWING_SECONDS;
    int player_health_units = PLAYER_MAX_HEALTH_UNITS;
    int player_food_units = PLAYER_MAX_FOOD_UNITS;
    float food_regen_timer = 0.0f;
    float player_air_seconds = PLAYER_MAX_AIR_SECONDS;
    float drown_timer = 0.0f;
    float cactus_damage_timer = 0.0f;
    float lava_damage_timer = 0.0f;
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
#define REDSTONE_TICK_INTERVAL 0.1f      /* One Minecraft-style redstone tick. */
#define RESET_PLAYER_AFTER_DEATH(message) do { \
        respawn_player(&player, &cam); \
        player_health_units = PLAYER_MAX_HEALTH_UNITS; \
        player_food_units = PLAYER_MAX_FOOD_UNITS; \
        food_regen_timer = 0.0f; \
        player_air_seconds = PLAYER_MAX_AIR_SECONDS; \
        drown_timer = 0.0f; \
        cactus_damage_timer = 0.0f; \
        lava_damage_timer = 0.0f; \
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
    double perf_environment_ns = 0.0;
    double perf_redstone_ns = 0.0;
    double perf_falling_ns = 0.0;
    double perf_physics_ns = 0.0;
    double perf_stream_ns = 0.0;
    double perf_stream_wait_ns = 0.0;
    double perf_stream_body_ns = 0.0;
    double perf_lighting_ns = 0.0;
    double perf_gen_drain_ns = 0.0;
    double perf_mesh_drain_ns = 0.0;
    int perf_physics_dropped_steps = 0;
    int status_log_enabled = read_status_log_enabled(debug_enabled);
    int debug_hud_enabled = env_flag("VOXEL_DEBUG_HUD", false);
    bool return_to_menu_requested = false;
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
        redstone_tick_accumulator += frame_dt;

        /* Minecraft-style environment simulation: intentionally slower than
         * placement so fluids and falling blocks move visibly. */
        double environment_ns = 0.0;
        if (environment_tick_accumulator >= ENVIRONMENT_TICK_INTERVAL) {
            struct timespec environment_start, environment_end;

            environment_tick_accumulator -= ENVIRONMENT_TICK_INTERVAL;
            clock_gettime(CLOCK_MONOTONIC, &environment_start);
            world_water_tick(&world);
            clock_gettime(CLOCK_MONOTONIC, &environment_end);
            environment_ns =
                (double)ns_diff(&environment_end, &environment_start);
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
        double redstone_ns = 0.0;
        while (redstone_tick_accumulator >= REDSTONE_TICK_INTERVAL) {
            struct timespec redstone_start, redstone_end;
            clock_gettime(CLOCK_MONOTONIC, &redstone_start);
            redstone_tick_accumulator -= REDSTONE_TICK_INTERVAL;
            world_update_redstone(&world, REDSTONE_TICK_INTERVAL);
            clock_gettime(CLOCK_MONOTONIC, &redstone_end);
            redstone_ns += (double)ns_diff(&redstone_end, &redstone_start);
        }
        struct timespec falling_start, falling_end;
        clock_gettime(CLOCK_MONOTONIC, &falling_start);
        world_update_falling_blocks(&world, frame_dt);
        clock_gettime(CLOCK_MONOTONIC, &falling_end);
        double falling_ns = (double)ns_diff(&falling_end, &falling_start);

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
            } else if (furnace_open) {
                close_furnace_ui(&survival_inventory, &item_drops, &player,
                                 furnace_states,
                                 &furnace_open, &open_furnace_index);
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
        if (paused && furnace_open)
            close_furnace_ui(&survival_inventory, &item_drops, &player,
                             furnace_states,
                             &furnace_open, &open_furnace_index);
        PauseMenuAction pause_action = PAUSE_MENU_ACTION_NONE;
        if (paused &&
            pause_menu_update(&pause, &inp, &pause_settings,
                              &pause_action)) {
            world_set_stream_chunks_per_frame(&world,
                                              pause_settings.stream_chunks_per_frame);
            world_set_near_chunk_radius(&world,
                                        pause_settings.near_chunk_radius);
            world_set_render_distance(&world,
                                      pause_settings.render_distance);
            mouse_sens = mouse_sensitivity_from_percent(
                pause_settings.mouse_sensitivity_percent);
            fov_deg = fov_degrees_from_x10(
                pause_settings.fov_degrees_x10);
            cam.depth = camera_focal_px_from_fov(fov_deg);
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
        if (paused)
            (void)input_consume_menu_select(&inp);
        if (pause_action == PAUSE_MENU_ACTION_EXIT_GAME) {
            inp.quit = true;
            break;
        } else if (pause_action == PAUSE_MENU_ACTION_EXIT_TO_MENU) {
            return_to_menu_requested = true;
            break;
        }

        if (input_consume_inventory_toggle(&inp) &&
            !paused && !chat_is_open(&chat) &&
            player.mode == PLAYER_MODE_SURVIVAL) {
            if (inventory_open) {
                close_survival_inventory(&survival_inventory, &item_drops,
                                         &player, &inventory_open);
            } else if (furnace_open) {
                close_furnace_ui(&survival_inventory, &item_drops, &player,
                                 furnace_states,
                                 &furnace_open, &open_furnace_index);
            } else {
                survival_inventory_set_craft_grid_dim(&survival_inventory,
                                                      SURVIVAL_CRAFT_GRID_PLAYER);
                inventory_open = true;
                inventory_recipe_page = 0;
                inventory_cursor_x = SCREEN_WIDTH * 0.5f;
                inventory_cursor_y = SCREEN_HEIGHT * 0.5f;
            }
        }

        /* Chat toggle: ignored while paused or inventory is open — those
         * overlays own the screen. */
        if (input_consume_chat_toggle(&inp) &&
            !paused && !inventory_open && !furnace_open) {
            chat_toggle(&chat);
            input_set_text_mode(&inp, chat_is_open(&chat));
        }

        bool chat_open = chat_is_open(&chat);
        bool kill_requested = false;
        input_set_pointer_capture(&inp,
                                  !paused && !chat_open &&
                                  !inventory_open && !furnace_open);

        if (chat_open && !paused) {
            for (int i = 0; i < inp.text_queue_len; i++) {
                char ch = inp.text_queue[i];
                if (ch == INPUT_TEXT_BACKSPACE)
                    chat_handle_backspace(&chat);
                else if (ch == INPUT_TEXT_TAB)
                    handle_chat_tab_completion(&chat);
                else if (ch == INPUT_TEXT_HISTORY_PREV)
                    chat_handle_history_prev(&chat);
                else if (ch == INPUT_TEXT_HISTORY_NEXT)
                    chat_handle_history_next(&chat);
                else if (ch == INPUT_TEXT_ENTER) {
                    char submitted[CHAT_LINE_MAX + 1];

                    strcpy(submitted, chat.input);
                    chat_handle_enter(&chat);
                    execute_chat_command(&chat, &player, &world,
                                         &survival_inventory, &world_time,
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
            if (player.mode != PLAYER_MODE_SURVIVAL && furnace_open)
                close_furnace_ui(&survival_inventory, &item_drops, &player,
                                 furnace_states,
                                 &furnace_open, &open_furnace_index);
            last_player_mode = player.mode;
        }

        /* Look: mouse + arrow keys. Muted while paused so the camera
         * stays still in the pause view, but we still drain mouse state
         * so a held mouse doesn't accumulate deltas across the pause. */
        if (inventory_open || furnace_open) {
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
        if (!paused && !inventory_open && !furnace_open && !chat_open) {
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
        if (paused || inventory_open || furnace_open || chat_open)
            jump_pressed = false;

        if (!paused && !inventory_open && !furnace_open && !chat_open &&
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
        bool controls_blocked = paused || inventory_open || furnace_open ||
            chat_open;
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
            cactus_damage_timer = 0.0f;
            lava_damage_timer = 0.0f;
            food_regen_timer = 0.0f;
        }

        if (!paused && player.mode == PLAYER_MODE_SURVIVAL) {
            if (player_touches_cactus(&world, &player)) {
                cactus_damage_timer += frame_dt;
                while (cactus_damage_timer >= CACTUS_DAMAGE_INTERVAL_SECONDS) {
                    cactus_damage_timer -= CACTUS_DAMAGE_INTERVAL_SECONDS;
                    if (apply_survival_damage(&player_health_units,
                                              CACTUS_DAMAGE_UNITS,
                                              &damage_flash_timer)) {
                        RESET_PLAYER_AFTER_DEATH("pricked to death");
                        break;
                    }
                }
            } else {
                cactus_damage_timer = CACTUS_DAMAGE_INTERVAL_SECONDS;
            }
        } else {
            cactus_damage_timer = 0.0f;
        }

        if (!paused && player.mode == PLAYER_MODE_SURVIVAL) {
            if (player_touches_lava(&world, &player)) {
                lava_damage_timer += frame_dt;
                while (lava_damage_timer >= LAVA_DAMAGE_INTERVAL_SECONDS) {
                    lava_damage_timer -= LAVA_DAMAGE_INTERVAL_SECONDS;
                    if (apply_survival_damage(&player_health_units,
                                              LAVA_DAMAGE_UNITS,
                                              &damage_flash_timer)) {
                        RESET_PLAYER_AFTER_DEATH("tried to swim in lava");
                        break;
                    }
                }
            } else {
                lava_damage_timer = LAVA_DAMAGE_INTERVAL_SECONDS;
            }
        } else {
            lava_damage_timer = 0.0f;
        }

        if (!paused && player.mode == PLAYER_MODE_SURVIVAL &&
            player_food_units > 0 &&
            player_health_units < PLAYER_MAX_HEALTH_UNITS) {
            food_regen_timer += frame_dt;
            while (food_regen_timer >= FOOD_REGEN_INTERVAL_SECONDS &&
                   player_food_units > 0 &&
                   player_health_units < PLAYER_MAX_HEALTH_UNITS) {
                food_regen_timer -= FOOD_REGEN_INTERVAL_SECONDS;
                player_health_units++;
                player_food_units--;
            }
        } else if (player_health_units >= PLAYER_MAX_HEALTH_UNITS ||
                   player_food_units <= 0) {
            food_regen_timer = 0.0f;
        }

        if (player.mode == PLAYER_MODE_SURVIVAL)
            item_entities_update(&item_drops, &world, &survival_inventory,
                                 &player, frame_dt);
        if (!paused)
            update_pressure_plate_depressions(&world, &player, &item_drops);
        if (!paused && player.mode == PLAYER_MODE_SURVIVAL)
            furnace_states_update(furnace_states, &world, frame_dt);

        if (!paused && !chat_is_open(&chat) && inventory_open) {
            bool left_click = input_consume_break(&inp);
            bool right_click = input_consume_place(&inp);
            int craft_grid_dim =
                survival_inventory_craft_grid_dim(&survival_inventory);
            int recipe_nav = recipe_lookup_nav_hit(inventory_cursor_x,
                                                   inventory_cursor_y,
                                                   craft_grid_dim);
            InventoryHit hit = survival_inventory_hit_test(inventory_cursor_x,
                                                           inventory_cursor_y,
                                                           craft_grid_dim);

            break_timer = 0.0f;
            break_duration = 0.0f;
            break_target_valid = false;
            hand_swing_timer = HAND_SWING_SECONDS;
            (void)input_consume_hotbar_slot(&inp);
            (void)input_consume_hotbar_page(&inp);
            (void)input_consume_item_drop(&inp);
            inventory_recipe_page =
                clamp_recipe_lookup_page(craft_grid_dim, inventory_recipe_page);

            if ((left_click || right_click) && recipe_nav != 0) {
                inventory_recipe_page =
                    clamp_recipe_lookup_page(craft_grid_dim,
                                             inventory_recipe_page +
                                             recipe_nav);
            } else if ((left_click || right_click) &&
                       hit.area != INVENTORY_SLOT_NONE) {
                survival_inventory_click(&survival_inventory,
                                         hit.area, hit.index, right_click);
            }
            inp.break_down = false;
        } else if (!paused && !chat_is_open(&chat) && furnace_open) {
            bool left_click = input_consume_break(&inp);
            bool right_click = input_consume_place(&inp);
            FurnaceState *furnace = NULL;
            FurnaceHit hit = furnace_hit_test(inventory_cursor_x,
                                              inventory_cursor_y);

            break_timer = 0.0f;
            break_duration = 0.0f;
            break_target_valid = false;
            hand_swing_timer = HAND_SWING_SECONDS;
            (void)input_consume_hotbar_slot(&inp);
            (void)input_consume_hotbar_page(&inp);
            (void)input_consume_item_drop(&inp);

            if (open_furnace_index >= 0 &&
                open_furnace_index < FURNACE_MAX_STATES &&
                furnace_states[open_furnace_index].active &&
                world_get_block(&world,
                                furnace_states[open_furnace_index].x,
                                furnace_states[open_furnace_index].y,
                                furnace_states[open_furnace_index].z) ==
                    BLOCK_FURNACE) {
                furnace = &furnace_states[open_furnace_index];
            } else {
                close_furnace_ui(&survival_inventory, &item_drops, &player,
                                 furnace_states,
                                 &furnace_open, &open_furnace_index);
            }

            if (furnace && (left_click || right_click) &&
                hit.area != FURNACE_SLOT_NONE) {
                if (hit.area == FURNACE_SLOT_STORAGE) {
                    survival_inventory_click(&survival_inventory,
                                             INVENTORY_SLOT_STORAGE,
                                             hit.index, right_click);
                } else {
                    furnace_click_slot(&survival_inventory, furnace,
                                       hit.area, right_click);
                }
            }
            inp.break_down = false;
        } else if (!paused && !chat_is_open(&chat) &&
                   !inventory_open && !furnace_open) {
            int hotbar_slot = input_consume_hotbar_slot(&inp);
            bool hotbar_page_pressed = input_consume_hotbar_page(&inp);
            bool break_pressed = input_consume_break(&inp);
            bool place_pressed = input_consume_place(&inp);
            bool item_drop_pressed = input_consume_item_drop(&inp);

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

            if (item_drop_pressed) {
                if (player.mode == PLAYER_MODE_SURVIVAL) {
                    try_toss_selected_item(&survival_inventory, &item_drops,
                                           &player, &cam,
                                           selected_hotbar_slot, &chat);
                } else if (player.mode == PLAYER_MODE_CREATIVE) {
                    chat_log(&chat, "creative item drops disabled");
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
                    BlockTarget broken_target = {0};
                    BlockID broken = BLOCK_AIR;

                    if (try_break_targeted_block(&world, &cam,
                                                 &broken_target, &broken) &&
                        broken == BLOCK_FURNACE) {
                        furnace_state_drop_contents(furnace_states, &item_drops,
                                                    &player,
                                                    broken_target.hit_x,
                                                    broken_target.hit_y,
                                                    broken_target.hit_z);
                    }
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
                        const ItemStack *tool_stack =
                            survival_inventory_hotbar_stack(&survival_inventory,
                                                            selected_hotbar_slot);
                        bool same_target =
                            break_target_valid &&
                            target.hit_x == break_target.hit_x &&
                            target.hit_y == break_target.hit_y &&
                            target.hit_z == break_target.hit_z;

                        if (!item_stack_is_empty(tool_stack))
                            target_break_duration =
                                item_break_seconds(tool_stack->item,
                                                   target_block,
                                                   target_break_duration);

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
                                BlockDropRequest cascade_drops[
                                    WORLD_CHUNK_HEIGHT];
                                size_t cascade_drop_count =
                                    collect_vertical_plant_cascade_drops(
                                        &world, &break_target,
                                        cascade_drops,
                                        WORLD_CHUNK_HEIGHT);
                                Vec3 drop_push = camera_forward(&cam);

                                if (break_block_target(&world, &break_target,
                                                       &broken)) {
                                    if (broken == BLOCK_FURNACE)
                                        furnace_state_drop_contents(
                                            furnace_states, &item_drops,
                                            &player,
                                            break_target.hit_x,
                                            break_target.hit_y,
                                            break_target.hit_z);
                                    item_entity_spawn_block_drop(
                                        &item_drops, broken,
                                        break_target.hit_x,
                                        break_target.hit_y,
                                        break_target.hit_z,
                                        drop_push);
                                    spawn_block_drop_requests(
                                        &item_drops, cascade_drops,
                                        cascade_drop_count, drop_push);
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
                    PlaceResult pr = PLACE_OK;

                    if (try_press_targeted_button(&world, &cam, &chat)) {
                        pr = PLACE_OK;
                    } else if (try_toggle_targeted_lever(&world, &cam,
                                                         &chat)) {
                        pr = PLACE_OK;
                    } else if (try_toggle_targeted_door(&world, &cam,
                                                        &player, &chat)) {
                        pr = PLACE_OK;
                    } else if (held == BLOCK_DOOR) {
                        pr = try_place_targeted_door(&world, &cam, &player);
                    } else {
                        pr = try_place_targeted_block(&world, &cam,
                                                      &player, held);
                    }
                    if (pr != PLACE_OK && debug_enabled) {
                        chat_log(&chat, "place %s -> %s",
                                 BlockRegistry[held].name,
                                 place_result_name(pr));
                    }
                } else if (player.mode == PLAYER_MODE_SURVIVAL) {
                    const ItemStack *held_stack =
                        survival_inventory_hotbar_stack(&survival_inventory,
                                                        selected_hotbar_slot);

                    if (try_press_targeted_button(&world, &cam, &chat)) {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (try_toggle_targeted_lever(&world, &cam,
                                                         &chat)) {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (try_cycle_targeted_repeater(&world, &cam,
                                                           &chat)) {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (try_toggle_targeted_door(&world, &cam,
                                                        &player, &chat)) {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (try_open_targeted_crafting_table(&world, &cam,
                                                         &survival_inventory,
                                                         &inventory_open,
                                                         &inventory_cursor_x,
                                                         &inventory_cursor_y,
                                                         &chat)) {
                        inventory_recipe_page = 0;
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (try_open_targeted_furnace(
                                   &world, &cam, furnace_states,
                                   &furnace_open, &open_furnace_index,
                                   &inventory_cursor_x, &inventory_cursor_y,
                                   &chat)) {
                        break_timer = 0.0f;
                        break_duration = 0.0f;
                        break_target_valid = false;
                    } else if (!item_stack_is_empty(held_stack)) {
                        ItemID held_item = held_stack->item;

                        if (try_consume_held_food(&survival_inventory,
                                                  &item_drops,
                                                  &player,
                                                  selected_hotbar_slot,
                                                  &player_food_units,
                                                  &chat)) {
                            /* Food uses the same right-click path as placing. */
                        } else if (try_use_held_bucket(&world, &cam,
                                                       &survival_inventory,
                                                       selected_hotbar_slot,
                                                       held_item,
                                                       &chat)) {
                            break_timer = 0.0f;
                            break_duration = 0.0f;
                            break_target_valid = false;
                        } else if (item_is_placeable_block(held_item)) {
                            BlockID held = item_place_block(held_item);
                            PlaceResult pr = held == BLOCK_DOOR ?
                                try_place_targeted_door(&world, &cam, &player) :
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
            (void)input_consume_item_drop(&inp);
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
        if (!paused && (inventory_open || furnace_open)) {
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
        furnace_states_draw(ctx, furnace_states, world_time);
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
                const ItemStack *held_stack =
                    survival_inventory_hotbar_stack(&survival_inventory,
                                                    selected_hotbar_slot);

                if (!item_stack_is_empty(held_stack) &&
                    item_is_tool(held_stack->item)) {
                    draw_tool_in_hand(ctx, held_stack->item, hand_swing_timer);
                } else {
                    draw_bare_hand(ctx, hand_swing_timer);
                }
                draw_hotbar(ctx, selected_hotbar_slot, 0, player.mode,
                            &survival_inventory);
                draw_healthbar(ctx, player_health_units, damage_flash_timer);
                draw_hungerbar(ctx, player_food_units);
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
                                    inventory_cursor_y,
                                    inventory_recipe_page);
        if (!paused && furnace_open &&
            open_furnace_index >= 0 &&
            open_furnace_index < FURNACE_MAX_STATES &&
            furnace_states[open_furnace_index].active)
            draw_furnace_ui(ctx, &survival_inventory,
                            &furnace_states[open_furnace_index],
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
        perf_environment_ns += environment_ns;
        perf_redstone_ns += redstone_ns;
        perf_falling_ns += falling_ns;
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
                double named_update_ns =
                    perf_environment_ns + perf_redstone_ns + perf_falling_ns +
                    perf_physics_ns + perf_stream_ns + perf_lighting_ns +
                    perf_gen_drain_ns + perf_mesh_drain_ns;
                double other_update_ns = perf_update_ns - named_update_ns;
                if (other_update_ns < 0.0)
                    other_update_ns = 0.0;

                if (status_log_enabled) {
                    printf("\n");
                    fflush(stdout);
                }

                fprintf(stderr,
                        "perf: fps=%5.1f frame=%6.2fms work=%6.2fms "
                        "update=%5.2fms begin=%5.2fms draw=%6.2fms "
                        "end=%6.2fms sleep=%5.2fms max_work=%6.2fms "
                        "quads=%5.1f sky=%4.1f phys=%4.1f drop=%4.1f "
                        "env=%5.2f red=%5.2f fall=%5.2f "
                        "other=%5.2f upd_phys=%5.2f stream=%5.2f "
                        "wait=%5.2f body=%5.2f light=%5.2f gen=%5.2f "
                        "mesh=%5.2f\n",
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
                        ns_to_ms(perf_environment_ns / frame_div),
                        ns_to_ms(perf_redstone_ns / frame_div),
                        ns_to_ms(perf_falling_ns / frame_div),
                        ns_to_ms(other_update_ns / frame_div),
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
            perf_environment_ns = 0.0;
            perf_redstone_ns = 0.0;
            perf_falling_ns = 0.0;
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
#undef REDSTONE_TICK_INTERVAL
#undef ENVIRONMENT_TICK_INTERVAL

    if (status_log_enabled)
        printf("\n");
    /* Stop gen worker first: it can enqueue mesh-dirty work via finalize,
     * so quiescing it before the mesh worker avoids a final partial mesh
     * round that would just be discarded on shutdown. */
    gen_worker_stop();
    mesh_worker_stop();
    if (!world_flush(&world))
        fprintf(stderr, "world: failed to flush modified chunks on shutdown\n");
    world_free(&world);
    if (return_to_menu_requested && !inp.quit) {
        world_init(&world);
        goto home_menu_start;
    }

    input_shutdown(&inp);
    renderer_shutdown(ctx);
    return 0;
}
