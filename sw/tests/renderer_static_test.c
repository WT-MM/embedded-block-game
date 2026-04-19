/*
 * renderer_static_test.c — fixed-camera renderer test for shared-edge coverage.
 *
 * Renders a tiled platform and a short raised ridge with a static camera.
 * This makes 1-pixel cracks, uneven top-face coverage, and z mistakes easier
 * to spot than in the moving demo.
 */

#include <stdio.h>
#include <unistd.h>

#include "renderer.h"
#include "block_types.h"

#define DEMO_FRAMES 120
#define DEMO_FRAME_DELAY_US 33000

static const Block demo_scene[] = {
    { BLOCK_GRASS, { -1.5f, 0.0f, 5.5f } },
    { BLOCK_GRASS, { -0.5f, 0.0f, 5.5f } },
    { BLOCK_GRASS, {  0.5f, 0.0f, 5.5f } },
    { BLOCK_GRASS, {  1.5f, 0.0f, 5.5f } },

    { BLOCK_GRASS, { -1.5f, 0.0f, 6.5f } },
    { BLOCK_GRASS, { -0.5f, 0.0f, 6.5f } },
    { BLOCK_GRASS, {  0.5f, 0.0f, 6.5f } },
    { BLOCK_GRASS, {  1.5f, 0.0f, 6.5f } },

    { BLOCK_GRASS, { -1.5f, 0.0f, 7.5f } },
    { BLOCK_GRASS, { -0.5f, 0.0f, 7.5f } },
    { BLOCK_GRASS, {  0.5f, 0.0f, 7.5f } },
    { BLOCK_GRASS, {  1.5f, 0.0f, 7.5f } },

    { BLOCK_STONE, { -0.5f, 1.0f, 6.5f } },
    { BLOCK_STONE, {  0.5f, 1.0f, 6.5f } },
};

static Camera make_demo_camera(void)
{
    Camera camera = {
        .position = { 0.5f, 3.3f, 2.2f },
        .pitch = -0.68f,
        .yaw = 0.03f,
        .depth = 190.0f,
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

    printf("renderer_static_test: rendering %zu blocks for %d frames\n",
           sizeof(demo_scene) / sizeof(demo_scene[0]), DEMO_FRAMES);
    printf("expected image:\n");
    printf("  the 4x3 grass platform should read as one clean tiled plane\n");
    printf("  the two raised stone blocks should have flat, solid top faces\n");
    printf("  you should not see dark cracks or missing pixels between adjacent top faces\n");

    Camera camera = make_demo_camera();
    for (int frame = 0; frame < DEMO_FRAMES; frame++) {
        int quads;

        renderer_set_camera(ctx, &camera);
        renderer_begin_frame(ctx);
        quads = renderer_draw_chunk(ctx, demo_scene,
                                    (int)(sizeof(demo_scene) / sizeof(demo_scene[0])));
        renderer_end_frame(ctx);

        if (frame == 0) {
            printf("frame %3d: quads=%d yaw=%.3f pitch=%.3f\n",
                   frame, quads, camera.yaw, camera.pitch);
        }
        usleep(DEMO_FRAME_DELAY_US);
    }

    renderer_shutdown(ctx);
    return 0;
}
