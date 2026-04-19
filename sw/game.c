#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "renderer.h"
#include "input.h"
#include "block_types.h"
#include "world.h"

#define EYE_HEIGHT   1.7f
#define MOVE_SPEED   5.0f      /* blocks per second */
#define DEFAULT_MOUSE_SENS 0.003f /* radians per pixel */
#define LOOK_SPEED   1.8f      /* radians per second (arrow keys) */
#define PITCH_LIMIT  1.48f     /* ~85 degrees, avoids gimbal flip */
#define TARGET_FPS   30
#define FRAME_NS     (1000000000L / TARGET_FPS)
#define WORLD_RENDER_DISTANCE_CHUNKS 3
#define STONE_SEED   0x48403421u
#define STONE_TRIES_PER_CHUNK 24

static long ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
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

int main(void)
{
    RenderContext *ctx = renderer_init();
    VoxelWorld world;
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    InputState inp;
    input_init(&inp);

    init_block_types();
    world_init(&world);
    float mouse_sens = read_mouse_sensitivity();

    Camera cam = {
        .position = { 0.0f, EYE_HEIGHT, -1.5f },
        .pitch    = -0.3f,  /* negative pitch looks down in renderer.c */
        .yaw      = 0.0f,
        .depth    = 170.0f,
    };

    if (!world_init_infinite_flat_random_stone(&world,
                                               STONE_SEED,
                                               STONE_TRIES_PER_CHUNK,
                                               WORLD_RENDER_DISTANCE_CHUNKS,
                                               cam.position.x,
                                               cam.position.z)) {
        fprintf(stderr, "world generation failed\n");
        input_shutdown(&inp);
        renderer_shutdown(ctx);
        return 1;
    }

    printf("Controls: WASD=move  Space/Shift=up/down  Arrows=look  Mouse=look  Esc=quit\n");
    printf("World: infinite deterministic chunk stream of %dx%dx%d blocks (seed 0x%08x)\n",
           WORLD_CHUNK_SIZE, WORLD_CHUNK_HEIGHT, WORLD_CHUNK_SIZE, STONE_SEED);
    printf("Loaded window: %dx%d chunks around player (%d-chunk render radius + 1 border)\n",
           world.chunks_x, world.chunks_z, world.render_distance_chunks);
    printf("Cached loaded world: blocks=%d exposed_faces=%d\n",
           world_total_blocks(&world), world_total_faces(&world));
    printf("Mouse sensitivity: %.4f rad/input (set VOXEL_MOUSE_SENS to override)\n",
           mouse_sens);

    struct timespec prev, now, frame_end;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    while (!inp.quit) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)ns_diff(&now, &prev) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;

        input_update(&inp);

        /* Look — mouse or arrow keys */
        cam.yaw   += inp.mouse_dx * mouse_sens;
        cam.pitch -= inp.mouse_dy * mouse_sens;
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

        if (!world_stream_around(&world, cam.position.x, cam.position.z)) {
            fprintf(stderr, "\nchunk streaming failed\n");
            break;
        }

        renderer_set_camera(ctx, &cam);
        renderer_begin_frame(ctx);
        int quads = renderer_draw_world(ctx, &world);
        renderer_end_frame(ctx);

        printf("\rpos=(%.1f,%.1f,%.1f) chunk=(%d,%d) yaw=%.2f pitch=%.2f quads=%3d  ",
               cam.position.x, cam.position.y, cam.position.z,
               world.center_chunk_x, world.center_chunk_z,
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
    world_free(&world);
    renderer_shutdown(ctx);
    return 0;
}
