#include "renderer.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// --- Internal Context Definition ---
struct RenderContext {
    Camera current_camera;
    
    // In a real implementation, this might be a pointer to memory-mapped 
    // FPGA address space (e.g., via /dev/mem)
    RenderQuad* command_fifo; 
    int quads_submitted_this_frame;

    // Cached trig for the current frame to save cycles
    float cos_yaw, sin_yaw;
    float cos_pitch, sin_pitch;
};

// --- Internal Math & Projection Functions ---

// Projects a 3D world coordinate to 2D screen coordinate, applying camera transforms
static void project_point(RenderContext* ctx, Vec3 world_pos, Vertex2D* out_vert) {
    float dx = world_pos.x - ctx->current_camera.position.x;
    float dy = world_pos.y - ctx->current_camera.position.y;
    float dz = world_pos.z - ctx->current_camera.position.z;

    // Yaw
    float x_yaw = dx * ctx->cos_yaw - dz * ctx->sin_yaw;
    float z_yaw = dx * ctx->sin_yaw + dz * ctx->cos_yaw;
    float y_yaw = dy;

    // Pitch
    float x_cam = x_yaw;
    float y_cam = y_yaw * ctx->cos_pitch - z_yaw * ctx->sin_pitch;
    float z_cam = y_yaw * ctx->sin_pitch + z_yaw * ctx->cos_pitch;

    out_vert->z = z_cam;

    // Near-plane clipping check (as specified in your design doc, reject if < 0.1)
    if (z_cam > 0.1f) {
        float proj_x = (x_cam / z_cam) * ctx->current_camera.depth;
        float proj_y = (y_cam / z_cam) * ctx->current_camera.depth;
        
        out_vert->x = proj_x + (SCREEN_WIDTH / 2.0f);
        out_vert->y = proj_y + (SCREEN_HEIGHT / 2.0f);
    }
}

// Check if a block face is pointing towards the camera (Dot product of face normal and view vector)
static bool is_face_visible(Vec3 block_pos, Vec3 face_normal, Vec3 cam_pos) {
    // Vector from camera to the center of the face
    Vec3 view_dir = {
        (block_pos.x + 0.5f + face_normal.x * 0.5f) - cam_pos.x,
        (block_pos.y + 0.5f + face_normal.y * 0.5f) - cam_pos.y,
        (block_pos.z + 0.5f + face_normal.z * 0.5f) - cam_pos.z
    };
    
    // Dot product
    float dot = (view_dir.x * face_normal.x) + 
                (view_dir.y * face_normal.y) + 
                (view_dir.z * face_normal.z);
    
    return dot < 0.0f; // Visible if angle is > 90 degrees (facing opposite directions)
}

// --- Public API ---

RenderContext* renderer_init(void) {
    RenderContext* ctx = (RenderContext*)malloc(sizeof(RenderContext));
    if (!ctx) return NULL;
    
    // TODO: mmap() /dev/mem here to get the real FPGA FIFO address
    ctx->command_fifo = (RenderQuad*)malloc(sizeof(RenderQuad) * MAX_QUADS_IN_FLIGHT);
    ctx->quads_submitted_this_frame = 0;
    
    return ctx;
}

void renderer_shutdown(RenderContext* ctx) {
    if (ctx) {
        // TODO: munmap() FPGA memory here
        free(ctx->command_fifo);
        free(ctx);
    }
}

void renderer_begin_frame(RenderContext* ctx) {
    ctx->quads_submitted_this_frame = 0;
    // TODO: Write to FPGA control register to clear Z-Buffer and Screen
}

void renderer_end_frame(RenderContext* ctx) {
    // TODO: Write to FPGA control register to wait for VSYNC/FLIP
    // printf("Frame ended. Total quads pushed: %d\n", ctx->quads_submitted_this_frame);
}

void renderer_set_camera(RenderContext* ctx, Camera* camera) {
    ctx->current_camera = *camera;
    // Cache trig functions once per frame
    ctx->cos_yaw = cosf(camera->yaw);
    ctx->sin_yaw = sinf(camera->yaw);
    ctx->cos_pitch = cosf(camera->pitch);
    ctx->sin_pitch = sinf(camera->pitch);
}

bool renderer_push_quad(RenderContext* ctx, RenderQuad* quad) {
    if (ctx->quads_submitted_this_frame >= MAX_QUADS_IN_FLIGHT) {
        return false; // FIFO full/Out of budget
    }
    
    // TODO: memcpy directly to mapped FPGA FIFO address
    ctx->command_fifo[ctx->quads_submitted_this_frame] = *quad;
    ctx->quads_submitted_this_frame++;
    
    return true;
}

void renderer_draw_block(RenderContext* ctx, Block* block) {
    if (block->type == BLOCK_AIR) return;

    // Define the 6 face normals and their respective vertex offsets
    // Ordering matches BlockFace enum: TOP, BOTTOM, LEFT, RIGHT, FRONT, BACK
    Vec3 normals[NUM_FACES] = {
        {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}
    };

    // The 4 local corners for each of the 6 faces
    Vec3 face_vertices[NUM_FACES][4] = {
        {{0,1,1}, {1,1,1}, {1,1,0}, {0,1,0}}, // TOP
        {{0,0,0}, {1,0,0}, {1,0,1}, {0,0,1}}, // BOTTOM
        {{0,0,0}, {0,0,1}, {0,1,1}, {0,1,0}}, // LEFT
        {{1,0,1}, {1,0,0}, {1,1,0}, {1,1,1}}, // RIGHT
        {{0,0,0}, {0,1,0}, {1,1,0}, {1,0,0}}, // FRONT
        {{1,0,1}, {1,1,1}, {0,1,1}, {0,0,1}}  // BACK
    };

    // Standard UV mapping for a 16x16 texture
    float uvs[4][2] = { {0,15}, {15,15}, {15,0}, {0,0} };

    BlockDescriptor bd = BlockRegistry[block->type];

    for (int f = 0; f < NUM_FACES; f++) {
        // 1. Hidden-face culling: skip if face points away from camera
        if (!is_face_visible(block->position, normals[f], ctx->current_camera.position)) {
            continue;
        }

        RenderQuad quad;
        // In a real scenario, this gets an index mapping to the FPGA's BRAM layout
        quad.texture_id = bd.face_textures[f] ? 1 : 0; 
        quad.color_tint = 255;
        
        bool quad_behind_camera = false;

        // 2. Setup Vertices
        for (int v = 0; v < 4; v++) {
            Vec3 world_pos = {
                block->position.x + face_vertices[f][v].x,
                block->position.y + face_vertices[f][v].y,
                block->position.z + face_vertices[f][v].z
            };

            project_point(ctx, world_pos, &quad.vertices[v]);
            
            // Apply UVs
            quad.vertices[v].u = uvs[v][0];
            quad.vertices[v].v = uvs[v][1];

            // 3. Early Near-Plane rejection
            // (As noted in design doc: reject quad if a single vertex goes behind camera)
            if (quad.vertices[v].z < 0.1f) {
                quad_behind_camera = true;
                break;
            }
        }

        // 4. Submit to FPGA if valid
        if (!quad_behind_camera) {
            renderer_push_quad(ctx, &quad);
        }
    }
}

int renderer_draw_chunk(RenderContext* ctx, Block* blocks, int num_blocks) {
    int quads_before = ctx->quads_submitted_this_frame;
    for (int i = 0; i < num_blocks; i++) {
        renderer_draw_block(ctx, &blocks[i]);
    }
    return ctx->quads_submitted_this_frame - quads_before;
}