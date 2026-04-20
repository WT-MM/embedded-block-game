#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include <stdbool.h>
#include "block_types.h" // Requires your block types and textures definitions

typedef struct VoxelWorld VoxelWorld;

// --- Constants ---
#define SCREEN_WIDTH 320.0f
#define SCREEN_HEIGHT 240.0f
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
    float z;    // Used by FPGA for Z-buffering
    float u, v; // Texture coordinates
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
void renderer_draw_block(RenderContext* ctx, const Block* block);
int renderer_draw_chunk(RenderContext* ctx, const Block* blocks, int num_blocks);
int renderer_draw_world(RenderContext* ctx, const VoxelWorld* world);
bool renderer_draw_crosshair(RenderContext* ctx);
bool renderer_push_quad(RenderContext* ctx, const RenderQuad* quad);

#endif // RENDERER_H
