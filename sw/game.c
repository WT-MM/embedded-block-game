#include <math.h>
#include <stdio.h>
#include <time.h>

#include "renderer.h"
#include "input.h"
#include "block_types.h"

#define WORLD_W      16
#define WORLD_D      16
#define EYE_HEIGHT   1.7f
#define MOVE_SPEED   5.0f      /* blocks per second */
#define MOUSE_SENS   0.002f    /* radians per pixel */
#define LOOK_SPEED   1.8f      /* radians per second (arrow keys) */
#define PITCH_LIMIT  1.48f     /* ~85 degrees, avoids gimbal flip */
#define TARGET_FPS   30
#define FRAME_NS     (1000000000L / TARGET_FPS)

static Block world[WORLD_W * WORLD_D];

static void build_world(void)
{
    int i = 0;
    for (int z = 0; z < WORLD_D; z++) {
        for (int x = 0; x < WORLD_W; x++) {
            BlockID type = BLOCK_GRASS;
            /* checkerboard of dirt patches so the ground reads as 3D */
            if ((x + z) % 6 == 0) type = BLOCK_DIRT;
            world[i++] = (Block){ type, { (float)(x - WORLD_W / 2), 0.0f, (float)(z + 2) } };
        }
    }
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
        .position = { 0.0f, EYE_HEIGHT, 0.0f },
        .pitch    = 0.0f,
        .yaw      = 0.0f,
        .depth    = 170.0f,
    };

    printf("Controls: WASD=move  Space/Shift=up/down  Arrows=look  Mouse=look  Esc=quit\n");

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
        cam.pitch += inp.mouse_dy * MOUSE_SENS;
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
        int quads = renderer_draw_chunk(ctx, world, WORLD_W * WORLD_D);
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
