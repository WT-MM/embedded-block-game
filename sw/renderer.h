#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include <stdbool.h>
#include "block_types.h" // Requires your block types and textures definitions

// --- Constants ---
#define SCREEN_WIDTH 320.0f
#define SCREEN_HEIGHT 240.0f
#define MAX_QUADS_IN_FLIGHT 2048

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
void renderer_set_camera(RenderContext* ctx, Camera* camera);
void renderer_draw_block(RenderContext* ctx, Block* block);
int renderer_draw_chunk(RenderContext* ctx, Block* blocks, int num_blocks);
bool renderer_push_quad(RenderContext* ctx, RenderQuad* quad);

#endif // RENDERER_H