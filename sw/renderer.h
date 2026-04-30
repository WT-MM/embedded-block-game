#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include <stdbool.h>
#include "voxel_gpu.h"
#include "block_types.h" // Requires your block types and textures definitions

typedef struct VoxelWorld VoxelWorld;

// --- Constants ---
#define SCREEN_WIDTH ((float)VOXEL_RENDER_WIDTH)
#define SCREEN_HEIGHT ((float)VOXEL_RENDER_HEIGHT)
/*
 * A 3-chunk render radius over 16x16 grass terrain already exceeds 2k
 * visible quads on top faces alone, so keep enough headroom to avoid
 * silently dropping nearby chunks.
 */
#define MAX_QUADS_IN_FLIGHT 32768

// --- Core Math & Entity Structures ---
typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float x, y;
} Vec2;

typedef struct {
    Vec3 position;
    float pitch;
    float yaw;
    float depth; // Focal length / projection distance
} Camera;

typedef struct {
    BlockID type;
    Vec3 position;
} Block;

// --- Rendering Structures ---
typedef struct {
    float x, y;
    float z;             // Inverse-z mapped depth for the FPGA Z-buffer
    float u_over_w;      // Perspective-correct UV: u / w_eye
    float v_over_w;      //                         v / w_eye
    float one_over_w;    // 1 / w_eye, used by the HW reciprocal unit
} Vertex2D;

typedef struct {
    Vertex2D vertices[4];
    uint8_t texture_id;
    uint8_t color_tint;
    uint8_t flags;
} RenderQuad;

// Opaque context struct
typedef struct RenderContext RenderContext;

// --- Lifecycle Functions ---
RenderContext* renderer_init(void);
void renderer_shutdown(RenderContext* ctx);

// --- Frame Operations ---
void renderer_begin_frame(RenderContext* ctx);
void renderer_end_frame(RenderContext* ctx);

// --- Camera & Geometry ---
void renderer_set_camera(RenderContext* ctx, const Camera* camera);
int renderer_draw_chunk(RenderContext* ctx, const Block* blocks, int num_blocks);
int renderer_draw_sky(RenderContext* ctx, float time_seconds);
int renderer_draw_world(RenderContext* ctx, const VoxelWorld* world,
                        float time_seconds);
bool renderer_draw_crosshair(RenderContext* ctx);
bool renderer_draw_screen_tile(RenderContext* ctx,
                               float x0, float y0, float x1, float y1,
                               uint8_t texture_id, uint8_t extra_flags);
bool renderer_push_quad(RenderContext* ctx, const RenderQuad* quad);
bool renderer_fill_rect(RenderContext* ctx,
                        float x0, float y0, float x1, float y1,
                        uint8_t palette_index, uint8_t extra_flags);

#endif // RENDERER_H
