#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "renderer.h"
#include "input.h"
#include "block_types.h"

#define MAX_BLOCKS   1536
#define EYE_HEIGHT   1.7f
#define MOVE_SPEED   5.0f      /* blocks per second */
#define MOUSE_SENS   0.002f    /* radians per pixel */
#define LOOK_SPEED   1.8f      /* radians per second (arrow keys) */
#define PITCH_LIMIT  1.48f     /* ~85 degrees, avoids gimbal flip */
#define TARGET_FPS   30
#define FRAME_NS     (1000000000L / TARGET_FPS)
#define CHUNK_SIZE   16
#define CHUNK_HEIGHT 4
#define WORLD_CHUNKS_X 2
#define WORLD_CHUNKS_Z 2
#define WORLD_ORIGIN_CHUNK_X (-1)
#define WORLD_ORIGIN_CHUNK_Z 0
#define STONE_SEED   0x48403421u
#define STONE_TRIES_PER_CHUNK 24

static Block world[MAX_BLOCKS];
static int   world_count;

typedef struct {
    int chunk_x;
    int chunk_z;
    BlockID blocks[CHUNK_HEIGHT][CHUNK_SIZE][CHUNK_SIZE];
} Chunk;

static Chunk chunks[WORLD_CHUNKS_X * WORLD_CHUNKS_Z];

static void place(BlockID type, float x, float y, float z)
{
    if (world_count < MAX_BLOCKS)
        world[world_count++] = (Block){ type, { x, y, z } };
}

static uint32_t rng_next(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static uint32_t chunk_seed(int chunk_x, int chunk_z)
{
    uint32_t xbits = (uint32_t)chunk_x * 0x9e3779b9u;
    uint32_t zbits = (uint32_t)chunk_z * 0x85ebca6bu;
    uint32_t seed = STONE_SEED ^ xbits ^ zbits;

    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return seed;
}

static void clear_chunk(Chunk *chunk)
{
    for (int y = 0; y < CHUNK_HEIGHT; y++)
        for (int z = 0; z < CHUNK_SIZE; z++)
            for (int x = 0; x < CHUNK_SIZE; x++)
                chunk->blocks[y][z][x] = BLOCK_AIR;
}

static void generate_chunk(Chunk *chunk, int chunk_x, int chunk_z)
{
    uint32_t seed = chunk_seed(chunk_x, chunk_z);

    chunk->chunk_x = chunk_x;
    chunk->chunk_z = chunk_z;
    clear_chunk(chunk);

    /* Flat grass ground at y=0 across the entire chunk. */
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++)
            chunk->blocks[0][z][x] = BLOCK_GRASS;

    /*
     * Scatter deterministic stone pillars up to 3 blocks tall.
     * Keep the player's initial corridor open near world x in [-1, 1] and
     * world z in [2, 5] so startup is not cluttered.
     */
    for (int i = 0; i < STONE_TRIES_PER_CHUNK; i++) {
        int lx = (int)(rng_next(&seed) % CHUNK_SIZE);
        int lz = (int)(rng_next(&seed) % CHUNK_SIZE);
        int wx = chunk_x * CHUNK_SIZE + lx;
        int wz = chunk_z * CHUNK_SIZE + lz;
        int height = 1 + (int)(rng_next(&seed) % (CHUNK_HEIGHT - 1));

        if (fabsf((float)wx) <= 1.0f && wz <= 5)
            continue;
        if (chunk->blocks[1][lz][lx] != BLOCK_AIR)
            continue;

        for (int y = 1; y <= height; y++)
            chunk->blocks[y][lz][lx] = BLOCK_STONE;
    }
}

static void flatten_chunks_to_world(void)
{
    world_count = 0;

    for (int ci = 0; ci < WORLD_CHUNKS_X * WORLD_CHUNKS_Z; ci++) {
        const Chunk *chunk = &chunks[ci];

        for (int y = 0; y < CHUNK_HEIGHT; y++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                for (int x = 0; x < CHUNK_SIZE; x++) {
                    BlockID id = chunk->blocks[y][z][x];

                    if (id == BLOCK_AIR)
                        continue;
                    if (world_count >= MAX_BLOCKS)
                        return;

                    place(id,
                          (float)(chunk->chunk_x * CHUNK_SIZE + x),
                          (float)y,
                          (float)(chunk->chunk_z * CHUNK_SIZE + z));
                }
            }
        }
    }
}

static void build_world(void)
{
    int ci = 0;

    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
        for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
            generate_chunk(&chunks[ci],
                           WORLD_ORIGIN_CHUNK_X + cx,
                           WORLD_ORIGIN_CHUNK_Z + cz);
            ci++;
        }
    }

    flatten_chunks_to_world();
}

static long ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

int main(void)
{
    RenderContext *ctx = renderer_init();
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    InputState inp;
    input_init(&inp);

    init_block_types();
    build_world();

    Camera cam = {
        .position = { 0.0f, EYE_HEIGHT, -1.5f },
        .pitch    = -0.3f,  /* negative pitch looks down in renderer.c */
        .yaw      = 0.0f,
        .depth    = 170.0f,
    };

    printf("Controls: WASD=move  Space/Shift=up/down  Arrows=look  Mouse=look  Esc=quit\n");
    printf("World: %dx%d chunks of %dx%d flat ground with deterministic random stone blocks (seed 0x%08x)\n",
           WORLD_CHUNKS_X, WORLD_CHUNKS_Z, CHUNK_SIZE, CHUNK_SIZE, STONE_SEED);

    struct timespec prev, now, frame_end;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    while (!inp.quit) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)ns_diff(&now, &prev) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;

        input_update(&inp);

        /* Look — mouse or arrow keys */
        cam.yaw   += inp.mouse_dx * MOUSE_SENS;
        cam.pitch -= inp.mouse_dy * MOUSE_SENS;
        if (inp.look_right) cam.yaw   += LOOK_SPEED * dt;
        if (inp.look_left)  cam.yaw   -= LOOK_SPEED * dt;
        if (inp.look_down)  cam.pitch += LOOK_SPEED * dt;
        if (inp.look_up)    cam.pitch -= LOOK_SPEED * dt;
        if (cam.pitch >  PITCH_LIMIT) cam.pitch =  PITCH_LIMIT;
        if (cam.pitch < -PITCH_LIMIT) cam.pitch = -PITCH_LIMIT;
        input_clear_mouse(&inp);

        /* Move in the horizontal plane relative to yaw.
         * Forward world vector: (sin(yaw), 0, cos(yaw))
         * Right   world vector: (cos(yaw), 0, -sin(yaw))  */
        float fwd_x =  sinf(cam.yaw), fwd_z = cosf(cam.yaw);
        float rgt_x =  cosf(cam.yaw), rgt_z = -sinf(cam.yaw);
        float d = MOVE_SPEED * dt;

        if (inp.forward) { cam.position.x += fwd_x * d; cam.position.z += fwd_z * d; }
        if (inp.back)    { cam.position.x -= fwd_x * d; cam.position.z -= fwd_z * d; }
        if (inp.right)   { cam.position.x += rgt_x * d; cam.position.z += rgt_z * d; }
        if (inp.left)    { cam.position.x -= rgt_x * d; cam.position.z -= rgt_z * d; }
        if (inp.up)      cam.position.y += d;
        if (inp.down)    cam.position.y -= d;

        renderer_set_camera(ctx, &cam);
        renderer_begin_frame(ctx);
        int quads = renderer_draw_chunk(ctx, world, world_count);
        renderer_end_frame(ctx);

        printf("\rpos=(%.1f,%.1f,%.1f) yaw=%.2f pitch=%.2f quads=%3d  ",
               cam.position.x, cam.position.y, cam.position.z,
               cam.yaw, cam.pitch, quads);
        fflush(stdout);

        /* Sleep for the remainder of the frame budget */
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long used = ns_diff(&frame_end, &now);
        if (used < FRAME_NS) {
            struct timespec ts = { 0, FRAME_NS - used };
            nanosleep(&ts, NULL);
        }
    }

    printf("\n");
    input_shutdown(&inp);
    renderer_shutdown(ctx);
    return 0;
}
