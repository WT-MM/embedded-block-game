#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "renderer.h"
#include "input.h"
#include "block_types.h"
#include "world.h"
#include "player_physics.h" /* Added physics integration */

#define DEFAULT_MOUSE_SENS 0.003f /* radians per pixel */
#define LOOK_SPEED   1.8f         /* radians per second (arrow keys) */
#define PITCH_LIMIT  1.48f        /* ~85 degrees, avoids gimbal flip */
#define TARGET_FPS   30
#define FRAME_NS     (1000000000L / TARGET_FPS)
#define PERF_LOG_NS  1000000000L
#define WORLD_RENDER_DISTANCE_CHUNKS 3
#define STONE_SEED   0x48403421u
#define STONE_TRIES_PER_CHUNK 24

static long ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

static double ns_to_ms(double ns)
{
    return ns / 1000000.0;
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

    /* Initialize Player */
    Player player;
    /* Spawning a bit higher so the player drops onto the terrain */
    player_init(&player, 0.0f, 10.0f, -1.5f);

    Camera cam = {
        .position = { player.x, player_get_eye_height(&player), player.z },
        .pitch    = -0.3f,  /* negative pitch looks down in renderer.c */
        .yaw      = 0.0f,
        .depth    = 170.0f,
    };

    if (!world_init_infinite_procedural(&world,
                                        STONE_SEED,
                                        STONE_TRIES_PER_CHUNK,
                                        WORLD_RENDER_DISTANCE_CHUNKS,
                                        player.x,
                                        player.z)) {
        fprintf(stderr, "world generation failed\n");
        input_shutdown(&inp);
        renderer_shutdown(ctx);
        return 1;
    }

    printf("Controls: WASD=move  Space=jump  Shift=crouch  Arrows=look  Mouse=look  Esc=quit\n");
    printf("World: infinite deterministic chunk stream of %dx%dx%d blocks (seed 0x%08x)\n",
           WORLD_CHUNK_SIZE, WORLD_CHUNK_HEIGHT, WORLD_CHUNK_SIZE, STONE_SEED);
    printf("Loaded window: %dx%d chunks around player (%d-chunk render radius + 1 border)\n",
           world.chunks_x, world.chunks_z, world.render_distance_chunks);
    printf("Cached loaded world: blocks=%d exposed_faces=%d\n",
           world_total_blocks(&world), world_total_faces(&world));
    printf("Mouse sensitivity: %.4f rad/input (set VOXEL_MOUSE_SENS to override)\n",
           mouse_sens);

    struct timespec prev, now, frame_end;
    struct timespec perf_window_start;
    float world_time = 0.0f;
    int perf_frames = 0;
    int perf_quads = 0;
    int perf_sky_quads = 0;
    double perf_update_ns = 0.0;
    double perf_begin_ns = 0.0;
    double perf_draw_ns = 0.0;
    double perf_end_ns = 0.0;
    double perf_sleep_ns = 0.0;
    double perf_work_ns = 0.0;
    double perf_max_work_ns = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    perf_window_start = prev;

    while (!inp.quit) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct timespec loop_start = now;
        float dt = (float)ns_diff(&now, &prev) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;
        world_time += dt;

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

        /* Determine walking direction vector from camera yaw */
        float fwd_x =  sinf(cam.yaw), fwd_z = cosf(cam.yaw);
        float rgt_x =  cosf(cam.yaw), rgt_z = -sinf(cam.yaw);
        
        float wish_x = 0.0f;
        float wish_z = 0.0f;

        if (inp.forward) { wish_x += fwd_x; wish_z += fwd_z; }
        if (inp.back)    { wish_x -= fwd_x; wish_z -= fwd_z; }
        if (inp.right)   { wish_x += rgt_x; wish_z += rgt_z; }
        if (inp.left)    { wish_x -= rgt_x; wish_z -= rgt_z; }

        /* Normalize wish direction so diagonal movement isn't faster */
        float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
        if (wish_len > 0.0f) {
            wish_x /= wish_len;
            wish_z /= wish_len;
        }

        /* Update Physics (inp.up = Jump, inp.down = Shift/Crouch) */
        player_update(&player, &world, wish_x, wish_z, inp.up, inp.down, dt);

        /* Sync Camera to Player's updated physical position */
        cam.position.x = player.x;
        cam.position.y = player_get_eye_height(&player);
        cam.position.z = player.z;

        if (!world_stream_around(&world, player.x, player.z)) {
            fprintf(stderr, "\nchunk streaming failed\n");
            break;
        }

        struct timespec render_start, begin_end, draw_end, end_end;
        clock_gettime(CLOCK_MONOTONIC, &render_start);
        renderer_set_camera(ctx, &cam);
        renderer_begin_frame(ctx);
        clock_gettime(CLOCK_MONOTONIC, &begin_end);
        int sky_quads = renderer_draw_sky(ctx, world_time);
        int quads = renderer_draw_world(ctx, &world);
        renderer_draw_crosshair(ctx);
        clock_gettime(CLOCK_MONOTONIC, &draw_end);
        renderer_end_frame(ctx);
        clock_gettime(CLOCK_MONOTONIC, &end_end);

        double update_ns = (double)ns_diff(&render_start, &loop_start);
        double begin_ns = (double)ns_diff(&begin_end, &render_start);
        double draw_ns = (double)ns_diff(&draw_end, &begin_end);
        double end_ns = (double)ns_diff(&end_end, &draw_end);
        double work_ns = (double)ns_diff(&end_end, &loop_start);

        /* Added grounded status and velocity to debug readout */
        printf("\rpos=(%.1f,%.1f,%.1f) v=(%.1f,%.1f,%.1f) gnd=%d yaw=%.2f pitch=%.2f quads=%3d sky=%2d  ",
               player.x, player.y, player.z,
               player.vx, player.vy, player.vz, player.is_grounded,
               cam.yaw, cam.pitch, quads, sky_quads);
        fflush(stdout);

        /* Sleep for the remainder of the frame budget */
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long used = ns_diff(&frame_end, &loop_start);
        double sleep_ns = 0.0;
        if (used < FRAME_NS) {
            struct timespec ts = { 0, FRAME_NS - used };
            struct timespec sleep_start, sleep_end;
            clock_gettime(CLOCK_MONOTONIC, &sleep_start);
            nanosleep(&ts, NULL);
            clock_gettime(CLOCK_MONOTONIC, &sleep_end);
            sleep_ns = (double)ns_diff(&sleep_end, &sleep_start);
        }

        perf_frames++;
        perf_quads += quads;
        perf_sky_quads += sky_quads;
        perf_update_ns += update_ns;
        perf_begin_ns += begin_ns;
        perf_draw_ns += draw_ns;
        perf_end_ns += end_ns;
        perf_sleep_ns += sleep_ns;
        perf_work_ns += work_ns;
        if (work_ns > perf_max_work_ns)
            perf_max_work_ns = work_ns;

        struct timespec perf_now;
        clock_gettime(CLOCK_MONOTONIC, &perf_now);
        long perf_elapsed_ns = ns_diff(&perf_now, &perf_window_start);
        if (perf_elapsed_ns >= PERF_LOG_NS && perf_frames > 0) {
            double elapsed_s = (double)perf_elapsed_ns / 1e9;
            double frame_div = (double)perf_frames;

            fprintf(stderr,
                    "\nperf: fps=%5.1f frame=%6.2fms work=%6.2fms "
                    "update=%5.2fms begin=%5.2fms draw=%6.2fms "
                    "end=%6.2fms sleep=%5.2fms max_work=%6.2fms "
                    "quads=%5.1f sky=%4.1f\n",
                    frame_div / elapsed_s,
                    ns_to_ms((double)perf_elapsed_ns / frame_div),
                    ns_to_ms(perf_work_ns / frame_div),
                    ns_to_ms(perf_update_ns / frame_div),
                    ns_to_ms(perf_begin_ns / frame_div),
                    ns_to_ms(perf_draw_ns / frame_div),
                    ns_to_ms(perf_end_ns / frame_div),
                    ns_to_ms(perf_sleep_ns / frame_div),
                    ns_to_ms(perf_max_work_ns),
                    (double)perf_quads / frame_div,
                    (double)perf_sky_quads / frame_div);

            perf_window_start = perf_now;
            perf_frames = 0;
            perf_quads = 0;
            perf_sky_quads = 0;
            perf_update_ns = 0.0;
            perf_begin_ns = 0.0;
            perf_draw_ns = 0.0;
            perf_end_ns = 0.0;
            perf_sleep_ns = 0.0;
            perf_work_ns = 0.0;
            perf_max_work_ns = 0.0;
        }
    }

    printf("\n");
    input_shutdown(&inp);
    world_free(&world);
    renderer_shutdown(ctx);
    return 0;
}
