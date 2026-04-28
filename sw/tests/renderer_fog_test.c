/*
 * renderer_fog_test.c — world renderer smoke test for radial fog + alpha HUD.
 *
 * Uses the streamed procedural world path so render-distance culling, fog, and
 * the translucent crosshair all share the same code path as the game loop.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "block_types.h"
#include "renderer.h"
#include "world.h"

#define DEMO_FRAMES 120
#define DEMO_FRAME_DELAY_US 33000
#define DEMO_RENDER_DISTANCE_CHUNKS 2
#define DEMO_WORLD_SEED 0x00C0FFEEu

static Camera make_demo_camera(int frame)
{
    Camera camera = {
        .position = { 8.0f, 3.4f, 8.0f },
        .pitch = -0.72f,
        .yaw = 0.55f + 0.18f * sinf(frame * 0.030f),
        .depth = 180.0f,
    };

    return camera;
}

int main(void)
{
    RenderContext *ctx = renderer_init();
    VoxelWorld world;

    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    world_init(&world);
    init_block_types();

    Camera camera = make_demo_camera(0);
    if (!world_init_infinite_procedural(&world,
                                        DEMO_WORLD_SEED,
                                        14,
                                        DEMO_RENDER_DISTANCE_CHUNKS,
                                        camera.position.x,
                                        camera.position.z,
                                        NULL)) {
        fprintf(stderr, "world_init_infinite_procedural failed\n");
        renderer_shutdown(ctx);
        return 1;
    }

    printf("renderer_fog_test: procedural world + radial fog (%d frames)\n", DEMO_FRAMES);
    printf("expected image:\n");
    printf("  distant terrain should fade into the sky with a curved horizon around the player,\n");
    printf("  not a straight scanline slicing across the frame\n");
    printf("  the center crosshair should stay visible but slightly translucent over world and sky\n");

    for (int frame = 0; frame < DEMO_FRAMES; frame++) {
        int sky_quads;
        int world_quads;
        bool crosshair_ok;

        camera = make_demo_camera(frame);
        if (!world_stream_around(&world, camera.position.x, camera.position.z)) {
            fprintf(stderr, "world_stream_around failed on frame %d\n", frame);
            world_free(&world);
            renderer_shutdown(ctx);
            return 1;
        }

        renderer_set_camera(ctx, &camera);
        renderer_begin_frame(ctx);
        sky_quads = renderer_draw_sky(ctx, 18.0f);
        world_quads = renderer_draw_world(ctx, &world, 18.0f);
        crosshair_ok = renderer_draw_crosshair(ctx);
        renderer_end_frame(ctx);

        if (frame == 0) {
            printf("frame %3d: sky_quads=%d world_quads=%d crosshair=%s chunks=%d faces=%d\n",
                   frame,
                   sky_quads,
                   world_quads,
                   crosshair_ok ? "ok" : "failed",
                   world.chunk_count,
                   world_total_faces(&world));
        }

        usleep(DEMO_FRAME_DELAY_US);
    }

    world_free(&world);
    renderer_shutdown(ctx);
    return 0;
}
