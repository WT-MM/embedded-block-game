/*
 * renderer_edge_test.c — shared-edge raster test using hand-placed quads.
 *
 * This bypasses world/camera projection and submits several screen-space
 * quads that should meet along slanted edges with no gaps or color stealing.
 * If this looks wrong, the bug is in quad setup / edge rules / rasterization,
 * not in the 3D camera transform.
 */

#include <stdio.h>
#include <unistd.h>

#include "renderer.h"
#include "voxel_gpu.h"

#define DEMO_FRAMES 120
#define DEMO_FRAME_DELAY_US 33000

static void submit_plane(RenderContext *ctx)
{
    RenderQuad q = {0};

    q.texture_id = 0;
    q.flags = QUAD_FLAG_ZTEST;

    q.color_tint = 6;
    q.vertices[0] = (Vertex2D){  30.0f, 205.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[1] = (Vertex2D){ 120.0f, 165.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[2] = (Vertex2D){ 185.0f, 195.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[3] = (Vertex2D){  95.0f, 235.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    renderer_push_quad(ctx, &q);

    q.color_tint = 7;
    q.vertices[0] = (Vertex2D){ 120.0f, 165.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[1] = (Vertex2D){ 210.0f, 125.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[2] = (Vertex2D){ 275.0f, 155.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[3] = (Vertex2D){ 185.0f, 195.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    renderer_push_quad(ctx, &q);

    q.color_tint = 8;
    q.vertices[0] = (Vertex2D){  95.0f, 235.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[1] = (Vertex2D){ 185.0f, 195.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[2] = (Vertex2D){ 250.0f, 225.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[3] = (Vertex2D){ 160.0f, 265.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    renderer_push_quad(ctx, &q);

    q.color_tint = 5;
    q.vertices[0] = (Vertex2D){ 185.0f, 195.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[1] = (Vertex2D){ 275.0f, 155.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[2] = (Vertex2D){ 340.0f, 185.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    q.vertices[3] = (Vertex2D){ 250.0f, 225.0f, 0.72f, 0.0f, 0.0f, 0.0f };
    renderer_push_quad(ctx, &q);
}

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

    printf("renderer_edge_test: slanted shared-edge test (%d frames)\n", DEMO_FRAMES);
    printf("expected image:\n");
    printf("  four colored quads should read as one continuous slanted plane near the bottom\n");
    printf("  internal seams should be clean straight lines, not ragged cracks or colors stealing area\n");

    for (int frame = 0; frame < DEMO_FRAMES; frame++) {
        renderer_set_camera(ctx, &camera);
        renderer_begin_frame(ctx);
        submit_plane(ctx);
        renderer_end_frame(ctx);

        if (frame == 0)
            printf("frame %3d: submitted 4 screen-space quads\n", frame);

        usleep(DEMO_FRAME_DELAY_US);
    }

    renderer_shutdown(ctx);
    return 0;
}
