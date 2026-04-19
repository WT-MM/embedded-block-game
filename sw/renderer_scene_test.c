/*
 * renderer_scene_test.c — minimal renderer-driven scene test.
 *
 * Uses the real software renderer to submit a tiny fixed block scene with
 * flat colors only. This is the intended pre-z-buffer integration path:
 * geometry, projection, quad setup, submission, flip.
 */

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "renderer.h"
#include "block_types.h"

#define DEMO_FRAMES 120
#define DEMO_FRAME_DELAY_US 33000

static const Block demo_scene[] = {
    { BLOCK_GRASS, { -1.2f, 0.0f, 5.5f } },
    { BLOCK_DIRT,  {  0.2f, 0.0f, 6.6f } },
    { BLOCK_WOOD,  { -0.4f, 1.0f, 7.4f } },
    { BLOCK_STONE, {  1.2f, 0.5f, 8.4f } },
};

static Camera make_demo_camera(int frame)
{
    Camera camera = {
        .position = { 0.4f, 1.6f, 1.0f },
        .pitch = -0.34f,
        .yaw = 0.10f + 0.18f * sinf(frame * 0.06f),
        .depth = 170.0f,
    };

    return camera;
}

int main(void)
{
    RenderContext *ctx = renderer_init();
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    init_block_types();

    printf("renderer_scene_test: rendering %zu blocks for %d frames\n",
           sizeof(demo_scene) / sizeof(demo_scene[0]), DEMO_FRAMES);

    for (int frame = 0; frame < DEMO_FRAMES; frame++) {
        Camera camera = make_demo_camera(frame);
        int quads;

        renderer_set_camera(ctx, &camera);
        renderer_begin_frame(ctx);
        quads = renderer_draw_chunk(ctx, demo_scene,
                                    (int)(sizeof(demo_scene) / sizeof(demo_scene[0])));
        renderer_end_frame(ctx);

        printf("frame %3d: quads=%d yaw=%.3f pitch=%.3f\n",
               frame, quads, camera.yaw, camera.pitch);
        usleep(DEMO_FRAME_DELAY_US);
    }

    renderer_shutdown(ctx);
    return 0;
}
