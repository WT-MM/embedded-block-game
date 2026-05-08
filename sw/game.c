#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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
#include "pause_menu.h"

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
#define MAX_WORLD_RENDER_DISTANCE_CHUNKS 8
#define DEFAULT_STREAM_CHUNKS_PER_FRAME 1
#define MAX_STREAM_CHUNKS_PER_FRAME 64
#define DEFERRED_LIGHTING_MAX_STREAM_BODY_NS 1000000ULL
#define DEFERRED_LIGHTING_MAX_SPEED_SQ 0.25f
#define STONE_SEED   0x48403421u
#define STONE_TRIES_PER_CHUNK 24
#define HOTBAR_SLOT_COUNT 6
#define BLOCK_REACH_DISTANCE 6.0f
#define BLOCK_TRACE_STEP 0.05f
#define DEFAULT_WORLD_SAVE_DIR "../worlds/default"

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

static const BlockID HOTBAR_BLOCKS[HOTBAR_SLOT_COUNT] = {
    BLOCK_GRASS,
    BLOCK_DIRT,
    BLOCK_STONE,
    BLOCK_WOOD,
    BLOCK_GLASS,
    BLOCK_LAMP,
};

static const uint8_t HOTBAR_DIGITS[HOTBAR_SLOT_COUNT][5] = {
    { 0x2, 0x6, 0x2, 0x2, 0x7 }, /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 }, /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 }, /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 }, /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 }, /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 }, /* 6 */
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
    return read_env_bool("VOXEL_DEBUG_HUD");
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
        if (block != BLOCK_AIR) {
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

static bool try_break_targeted_block(VoxelWorld *world, const Camera *cam)
{
    BlockTarget target = {0};

    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target))
        return false;

    if (!world_set_block(world,
                         target.hit_x, target.hit_y, target.hit_z,
                         BLOCK_AIR))
        return false;

    /* Route the edited chunk through the mesh worker's priority lane so
     * the broken-block visual lands on the next frame even if the main
     * queue carries a backlog. */
    world_mark_chunk_mesh_edit_priority(world, target.hit_x, target.hit_z);
    return true;
}

static bool try_place_targeted_block(VoxelWorld *world, const Camera *cam,
                                     const Player *player, BlockID type)
{
    BlockTarget target = {0};

    if (type <= BLOCK_AIR || type >= NUM_BLOCK_TYPES)
        return false;
    if (!trace_target_block(world, cam, BLOCK_REACH_DISTANCE, &target) ||
        !target.place_valid)
        return false;
    if (world_get_block(world, target.place_x, target.place_y, target.place_z) != BLOCK_AIR)
        return false;
    if (player_intersects_block(player, target.place_x, target.place_y, target.place_z))
        return false;

    if (!world_set_block(world,
                         target.place_x, target.place_y, target.place_z,
                         type))
        return false;

    world_mark_chunk_mesh_edit_priority(world, target.place_x, target.place_z);
    return true;
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

static void draw_hotbar(RenderContext *ctx, int selected_slot)
{
    const float s = HUD_SCALE;
    const float slot_size = 20.0f * s;
    const float gap = 4.0f * s;
    const float slot_top = SCREEN_HEIGHT - slot_size - 8.0f * s;
    const float total_width =
        HOTBAR_SLOT_COUNT * slot_size + (HOTBAR_SLOT_COUNT - 1) * gap;
    const float slot_left = floorf((SCREEN_WIDTH - total_width) * 0.5f);

    renderer_fill_rect(ctx,
                       slot_left - 4.0f * s, slot_top - 4.0f * s,
                       slot_left + total_width + 4.0f * s,
                       slot_top + slot_size + 4.0f * s,
                       14, 0);
    renderer_fill_rect(ctx,
                       slot_left - 3.0f * s, slot_top - 3.0f * s,
                       slot_left + total_width + 3.0f * s,
                       slot_top + slot_size + 3.0f * s,
                       0, 0);

    for (int i = 0; i < HOTBAR_SLOT_COUNT; i++) {
        float x0 = slot_left + (slot_size + gap) * (float)i;
        float x1 = x0 + slot_size;
        float y0 = slot_top;
        float y1 = y0 + slot_size;
        uint8_t border = (i == selected_slot) ? 5 : 14;
        uint8_t number = (i == selected_slot) ? 8 : 5;

        renderer_fill_rect(ctx, x0, y0, x1, y1, border, 0);
        renderer_fill_rect(ctx, x0 + 1.0f * s, y0 + 1.0f * s,
                           x1 - 1.0f * s, y1 - 1.0f * s, 0, 0);
        renderer_draw_screen_tile(ctx,
                                  x0 + 2.0f * s, y0 + 2.0f * s,
                                  x1 - 2.0f * s, y1 - 2.0f * s,
                                  block_face_texture_id(HOTBAR_BLOCKS[i], FACE_FRONT),
                                  0);
        renderer_fill_rect(ctx, x0 + 1.0f * s, y0 + 1.0f * s,
                           x0 + 5.0f * s, y0 + 7.0f * s, 0, 0);
        draw_hotbar_digit(ctx, i + 1, x0 + 2.0f * s, y0 + 2.0f * s, number);

        if (i == selected_slot) {
            renderer_fill_rect(ctx,
                               x0 + 2.0f * s, y1 - 3.0f * s,
                               x1 - 2.0f * s, y1 - 1.0f * s,
                               8, 0);
        }
    }
}

/* Debug HUD: player position, chunk coords, render distance, loaded chunks.
 * Enabled by VOXEL_DEBUG_HUD=1. Drawn in the top-right corner using the
 * same drop-shadow text style as the FPS counter. */
static void draw_debug_hud(RenderContext *ctx, const Player *player,
                           const Camera *cam, const VoxelWorld *world)
{
    char lines[8][56];
    int line_count = 0;
    int cell_w = chat_font_cell_w();
    int cell_h = chat_font_cell_h();
    int line_step = cell_h + 2;
    int chunk_x = (int)floorf(player->x / (float)WORLD_CHUNK_SIZE);
    int chunk_z = (int)floorf(player->z / (float)WORLD_CHUNK_SIZE);

    /* Block position within the current chunk (0..15 in each axis). */
    float local_x = player->x - (float)(chunk_x * WORLD_CHUNK_SIZE);
    float local_z = player->z - (float)(chunk_z * WORLD_CHUNK_SIZE);

    snprintf(lines[line_count++], sizeof(lines[0]),
             "XYZ: %.1f / %.1f / %.1f", player->x, player->y, player->z);
    snprintf(lines[line_count++], sizeof(lines[0]),
             "Chunk: %d, %d  [%.1f, %.1f]", chunk_x, chunk_z,
             local_x, local_z);
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

    float y = 12.0f;
    for (int i = 0; i < line_count; i++) {
        int len = (int)strlen(lines[i]);
        float text_w = (float)(len * cell_w);
        float x = SCREEN_WIDTH - text_w - 12.0f;
        if (x < 0.0f) x = 0.0f;
        /* Drop shadow + foreground, same as FPS counter */
        chat_draw_text(ctx, lines[i], len, x + 1.0f, y + 1.0f, 0);
        chat_draw_text(ctx, lines[i], len, x, y, 5);
        y += (float)line_step;
    }
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
    const char *world_save_dir = read_world_save_dir();
    int selected_hotbar_slot = 0;
    PauseMenuSettings pause_settings = {0};

    /* Initialize Player */
    Player player;
    /* Spawning a bit higher so the player drops onto the terrain */
    player_init(&player, 0.0f, 10.0f, -1.5f);

    Camera cam = {
        .position = { player.x, player_get_eye_height(&player), player.z },
        .pitch    = -0.3f,  /* negative pitch looks down in renderer.c */
        .yaw      = 0.0f,
        .depth    = compute_camera_focal_px(),
    };

    if (!world_init_infinite_procedural(&world,
                                        STONE_SEED,
                                        STONE_TRIES_PER_CHUNK,
                                        render_distance_chunks,
                                        player.x,
                                        player.z,
                                        world_save_dir)) {
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
    pause_settings.render_distance_max = render_distance_chunks;

    bool mesh_worker_running = mesh_worker_start(&world);
    bool gen_worker_running = gen_worker_start(&world);

    if (debug_enabled) {
        printf("Controls: WASD=move  double-tap W=sprint  Space=jump/fly-up  Shift=crouch/fly-down  1-6=hotbar  F/LMB=break  R/RMB=place  G=cycle mode  T=chat  Esc=pause/release mouse  Q=quit\n");
        printf("Mode: %s (survival=gravity+collision, creative=fly+collision, spectator=fly+no-collision)\n",
               player_mode_name(player.mode));
        printf("World: infinite deterministic chunk stream of %dx%dx%d blocks (seed 0x%08x)\n",
               WORLD_CHUNK_SIZE, WORLD_CHUNK_HEIGHT, WORLD_CHUNK_SIZE, STONE_SEED);
        printf("World save dir: %s\n", world_save_dir);
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

        input_update(&inp);

        chat_tick(&chat, frame_dt);

        /* ESC: close chat first, otherwise toggle the pause overlay. The
         * chat branch already consumes ESC while text mode is on, so any
         * pause_toggle event that reaches us here is coming from the
         * game view. */
        if (input_consume_pause_toggle(&inp)) {
            if (!chat_is_open(&chat))
                pause_menu_toggle(&pause);
        }

        bool paused = pause_menu_is_open(&pause);
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

        /* Chat toggle: ignored while paused — pause owns the overlay. */
        if (input_consume_chat_toggle(&inp) && !paused) {
            chat_toggle(&chat);
            input_set_text_mode(&inp, chat_is_open(&chat));
        }

        bool chat_open = chat_is_open(&chat);
        input_set_pointer_capture(&inp, !paused && !chat_open);

        if (chat_open && !paused) {
            for (int i = 0; i < inp.text_queue_len; i++) {
                char ch = inp.text_queue[i];
                if (ch == INPUT_TEXT_BACKSPACE)
                    chat_handle_backspace(&chat);
                else if (ch == INPUT_TEXT_ENTER)
                    chat_handle_enter(&chat);
                else
                    chat_handle_char(&chat, ch);
            }
        }
        input_clear_text_queue(&inp);

        if (input_consume_mode_toggle(&inp) && !paused) {
            player_cycle_mode(&player);
            chat_log(&chat, "mode: %s", player_mode_name(player.mode));
        }

        /* Look: mouse + arrow keys. Muted while paused so the camera
         * stays still in the pause view, but we still drain mouse state
         * so a held mouse doesn't accumulate deltas across the pause. */
        if (!paused) {
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
        if (!paused) {
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
        if (paused)
            jump_pressed = false;

        /* In fly modes Space/Shift are held to ascend/descend, so pass the
         * held state rather than the edge-triggered jump. */
        bool flying = (player.mode != PLAYER_MODE_SURVIVAL);
        bool up_input    = paused ? false : inp.up;
        bool down_input  = paused ? false : inp.down;
        bool sprint_input = paused ? false : inp.sprint;

        struct timespec physics_start, physics_end;
        clock_gettime(CLOCK_MONOTONIC, &physics_start);
        while (physics_accumulator >= PHYSICS_DT &&
               (max_physics_steps_per_frame <= 0 ||
                physics_steps < max_physics_steps_per_frame)) {
            bool jump_input = flying ? up_input : jump_pressed;
            player_update(&player, &world, wish_x, wish_z,
                          jump_input, down_input, sprint_input, PHYSICS_DT);
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

        if (!paused && !chat_is_open(&chat)) {
            int hotbar_slot = input_consume_hotbar_slot(&inp);

            if (hotbar_slot >= 0 && hotbar_slot < HOTBAR_SLOT_COUNT &&
                hotbar_slot != selected_hotbar_slot) {
                selected_hotbar_slot = hotbar_slot;
                chat_log(&chat, "selected: %d %s",
                         selected_hotbar_slot + 1,
                         BlockRegistry[HOTBAR_BLOCKS[selected_hotbar_slot]].name);
            }

            if (input_consume_break(&inp))
                try_break_targeted_block(&world, &cam);
            if (input_consume_place(&inp)) {
                try_place_targeted_block(&world, &cam, &player,
                                         HOTBAR_BLOCKS[selected_hotbar_slot]);
            }
        } else {
            (void)input_consume_hotbar_slot(&inp);
            (void)input_consume_break(&inp);
            (void)input_consume_place(&inp);
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
        if (!paused) {
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
        chat_draw(&chat, ctx);
        if (!paused && !chat_is_open(&chat))
            draw_hotbar(ctx, selected_hotbar_slot);
        renderer_draw_crosshair(ctx);
        pause_menu_draw(&pause, ctx, &pause_settings);
        if (fps_text_len > 0) {
            chat_draw_text(ctx, fps_text, fps_text_len, 13.0f, 13.0f, 0);
            chat_draw_text(ctx, fps_text, fps_text_len, 12.0f, 12.0f, 5);
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
