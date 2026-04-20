/*
 * renderer_quad_test.c — hand-placed screen-space quad for raster / packing checks.
 *
 * Submits a single clockwise quad in pixel coordinates with constant z, bypassing
 * world projection and chunk drawing. Use this to verify edge functions, depth
 * plane setup, and submission before debugging camera math.
 */

#include <stdio.h>
#include <unistd.h>

#include "renderer.h"
#include "voxel_gpu.h"

#define DEMO_FRAMES 120
#define DEMO_FRAME_DELAY_US 33000

int main(void)
{
    RenderContext *ctx = renderer_init();
    if (!ctx) {
        fprintf(stderr, "renderer_init failed\n");
        return 1;
    }

    Camera camera = {
        .position = { 0.0f, 0.0f, 0.0f },
        .pitch = 0.0f,
        .yaw = 0.0f,
        .depth = 170.0f,
    };

    printf("renderer_quad_test: one screen-space quad (%d frames)\n", DEMO_FRAMES);
    printf("  expect a bright rectangle; camera is ignored.\n");
    printf("  winding: TL -> TR -> BR -> BL (clockwise in screen space).\n");

    for (int frame = 0; frame < DEMO_FRAMES; frame++) {
        renderer_set_camera(ctx, &camera);
        renderer_begin_frame(ctx);

        RenderQuad q = {0};
        /* Top-left -> top-right -> bottom-right -> bottom-left */
        q.vertices[0] = (Vertex2D){  80.0f,  60.0f, 0.85f, 0.0f, 0.0f, 0.0f };
        q.vertices[1] = (Vertex2D){ 240.0f,  60.0f, 0.85f, 0.0f, 0.0f, 0.0f };
        q.vertices[2] = (Vertex2D){ 240.0f, 180.0f, 0.85f, 0.0f, 0.0f, 0.0f };
        q.vertices[3] = (Vertex2D){  80.0f, 180.0f, 0.85f, 0.0f, 0.0f, 0.0f };
        q.texture_id = 0;
        q.color_tint = 5; /* debug white in default palette */
        q.flags = QUAD_FLAG_ZTEST;

        if (!renderer_push_quad(ctx, &q))
            fprintf(stderr, "frame %d: renderer_push_quad failed\n", frame);

        renderer_end_frame(ctx);

        if (frame == 0)
            printf("frame %3d: submitted hand quad\n", frame);

        usleep(DEMO_FRAME_DELAY_US);
    }

    renderer_shutdown(ctx);
    return 0;
}
