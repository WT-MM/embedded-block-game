#include "renderer.h"
#include "world.h"
#include "voxel_gpu.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

#define DEV_PATH "/dev/voxel_gpu"
#define NEAR_PLANE 0.1f
#define VIEW_X_MIN 0.0f
#define VIEW_Y_MIN 0.0f
#define VIEW_X_MAX (SCREEN_WIDTH - 1.0f)
#define VIEW_Y_MAX (SCREEN_HEIGHT - 1.0f)
#define MAX_VIEW_CLIP_VERTS 8

struct StagedQuad {
    struct quad_desc desc;
};

typedef struct {
    int x, y, z;
    int occupied;
} LookupEntry;

typedef struct {
    LookupEntry *entries;
    int capacity;
    int mask;
} BlockLookup;

static void upload_default_palette(int fd)
{
    static const struct voxel_palette_entry entries[] = {
        {  0, 0x10, 0x10, 0x18 }, /* background */
        {  1, 0x4c, 0xaf, 0x50 }, /* grass top */
        {  2, 0x8b, 0x45, 0x13 }, /* dirt side */
        {  3, 0x65, 0x43, 0x21 }, /* wood side */
        {  4, 0x80, 0x80, 0x80 }, /* stone side */
        {  5, 0xff, 0xff, 0xff }, /* debug white */
        {  6, 0xff, 0x40, 0x40 }, /* debug red */
        {  7, 0x40, 0xa0, 0xff }, /* debug blue */
        {  8, 0xff, 0xd0, 0x40 }, /* debug yellow */
        {  9, 0x5e, 0x8b, 0x3d }, /* grass side */
        { 10, 0x63, 0x3a, 0x17 }, /* grass bottom / dark dirt */
        { 11, 0x96, 0x6a, 0x3c }, /* wood top */
        { 12, 0x4d, 0x31, 0x18 }, /* wood bottom */
        { 13, 0xa4, 0xa4, 0xa4 }, /* stone top */
        { 14, 0x58, 0x58, 0x58 }, /* stone bottom */
        { 15, 0xa0, 0x67, 0x35 }, /* dirt top */
        { 16, 0x5a, 0x2f, 0x12 }, /* dirt bottom */
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &entries[i]))
            perror("ioctl(SET_PALETTE)");
    }
}

struct RenderContext {
    int fd;
    Camera current_camera;

    float cos_yaw, sin_yaw;
    float cos_pitch, sin_pitch;

    struct StagedQuad *staging;
    struct quad_desc *submit_buffer;
    LookupEntry *lookup_entries;
    int lookup_capacity;
    int n_quads;
};

typedef struct {
    float x, y, z;
    float u, v;
} CameraVertex;

static const Vec3 face_normals[NUM_FACES] = {
    { 0, 1, 0 },
    { 0, -1, 0 },
    { -1, 0, 0 },
    { 1, 0, 0 },
    { 0, 0, -1 },
    { 0, 0, 1 },
};

static const Vec3 face_verts[NUM_FACES][4] = {
    { { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 0 } },
    { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
    { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
    { { 1, 0, 1 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 } },
    { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } },
    { { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 } },
};

static void world_to_camera(RenderContext *ctx, Vec3 world, CameraVertex *out)
{
    float dx = world.x - ctx->current_camera.position.x;
    float dy = world.y - ctx->current_camera.position.y;
    float dz = world.z - ctx->current_camera.position.z;

    float x_yaw =  dx * ctx->cos_yaw - dz * ctx->sin_yaw;
    float z_yaw =  dx * ctx->sin_yaw + dz * ctx->cos_yaw;
    float y_yaw =  dy;

    out->x = x_yaw;
    out->y =  y_yaw * ctx->cos_pitch + z_yaw * ctx->sin_pitch;
    out->z = -y_yaw * ctx->sin_pitch + z_yaw * ctx->cos_pitch;
}

/* Project a camera-space point into screen space.
 * Returns false if the point is behind the near plane. */
static bool project_camera_vertex(RenderContext *ctx, const CameraVertex *in, Vertex2D *out)
{
    if (in->z < NEAR_PLANE)
        return false;

    /*
     * Use an inverse-depth mapping so the per-quad depth plane stays affine
     * in screen space and fits inside the descriptor's Q1.15 range.
     * Smaller values remain closer, matching the hardware z compare.
     */
    out->z = 1.0f - (NEAR_PLANE / in->z);

    float depth = ctx->current_camera.depth;
    out->x = (in->x / in->z) * depth + SCREEN_WIDTH  / 2.0f;
    out->y = SCREEN_HEIGHT / 2.0f - (in->y / in->z) * depth;
    out->u = in->u;
    out->v = in->v;
    return true;
}

/* Dot-product backface cull: skip face if normal points away from camera. */
static bool is_face_visible(Vec3 block_pos, Vec3 normal, Vec3 cam_pos)
{
    Vec3 v = {
        (block_pos.x + 0.5f + normal.x * 0.5f) - cam_pos.x,
        (block_pos.y + 0.5f + normal.y * 0.5f) - cam_pos.y,
        (block_pos.z + 0.5f + normal.z * 0.5f) - cam_pos.z,
    };
    return (v.x*normal.x + v.y*normal.y + v.z*normal.z) < 0.0f;
}

static CameraVertex lerp_camera_vertex(const CameraVertex *a,
                                       const CameraVertex *b, float t)
{
    CameraVertex out = {
        .x = a->x + (b->x - a->x) * t,
        .y = a->y + (b->y - a->y) * t,
        .z = a->z + (b->z - a->z) * t,
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
    };

    return out;
}

static Vertex2D lerp_vertex2d(const Vertex2D *a, const Vertex2D *b, float t)
{
    Vertex2D out = {
        .x = a->x + (b->x - a->x) * t,
        .y = a->y + (b->y - a->y) * t,
        .z = a->z + (b->z - a->z) * t,
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
    };

    return out;
}

static int clip_face_to_near_plane(const CameraVertex in[4], CameraVertex out[6])
{
    int out_count = 0;
    CameraVertex prev = in[3];
    bool prev_inside = (prev.z >= NEAR_PLANE);

    for (int i = 0; i < 4; i++) {
        CameraVertex cur = in[i];
        bool cur_inside = (cur.z >= NEAR_PLANE);

        if (prev_inside != cur_inside) {
            float t = (NEAR_PLANE - prev.z) / (cur.z - prev.z);
            CameraVertex clipped = lerp_camera_vertex(&prev, &cur, t);
            clipped.z = NEAR_PLANE;
            out[out_count++] = clipped;
        }

        if (cur_inside)
            out[out_count++] = cur;

        prev = cur;
        prev_inside = cur_inside;
    }

    return out_count;
}

typedef enum {
    CLIP_LEFT = 0,
    CLIP_RIGHT,
    CLIP_TOP,
    CLIP_BOTTOM,
} ClipBoundary;

static bool vertex_inside_boundary(const Vertex2D *v, ClipBoundary boundary)
{
    switch (boundary) {
    case CLIP_LEFT:
        return v->x >= VIEW_X_MIN;
    case CLIP_RIGHT:
        return v->x <= VIEW_X_MAX;
    case CLIP_TOP:
        return v->y >= VIEW_Y_MIN;
    case CLIP_BOTTOM:
        return v->y <= VIEW_Y_MAX;
    default:
        return false;
    }
}

static float boundary_value(ClipBoundary boundary)
{
    switch (boundary) {
    case CLIP_LEFT:
        return VIEW_X_MIN;
    case CLIP_RIGHT:
        return VIEW_X_MAX;
    case CLIP_TOP:
        return VIEW_Y_MIN;
    case CLIP_BOTTOM:
        return VIEW_Y_MAX;
    default:
        return 0.0f;
    }
}

static int clip_polygon_against_boundary(const Vertex2D *in, int count,
                                         Vertex2D *out, ClipBoundary boundary)
{
    if (count <= 0)
        return 0;

    int out_count = 0;
    Vertex2D prev = in[count - 1];
    bool prev_inside = vertex_inside_boundary(&prev, boundary);
    float bound = boundary_value(boundary);

    for (int i = 0; i < count; i++) {
        Vertex2D cur = in[i];
        bool cur_inside = vertex_inside_boundary(&cur, boundary);

        if (prev_inside != cur_inside) {
            float denom;
            float t;
            Vertex2D clipped;

            if (boundary == CLIP_LEFT || boundary == CLIP_RIGHT)
                denom = cur.x - prev.x;
            else
                denom = cur.y - prev.y;

            if (fabsf(denom) < 1e-6f)
                t = 0.0f;
            else if (boundary == CLIP_LEFT || boundary == CLIP_RIGHT)
                t = (bound - prev.x) / denom;
            else
                t = (bound - prev.y) / denom;

            clipped = lerp_vertex2d(&prev, &cur, t);
            if (boundary == CLIP_LEFT || boundary == CLIP_RIGHT)
                clipped.x = bound;
            else
                clipped.y = bound;
            out[out_count++] = clipped;
        }

        if (cur_inside)
            out[out_count++] = cur;

        prev = cur;
        prev_inside = cur_inside;
    }

    return out_count;
}

static int clip_polygon_to_viewport(const Vertex2D in[4], Vertex2D out[MAX_VIEW_CLIP_VERTS])
{
    Vertex2D buf_a[MAX_VIEW_CLIP_VERTS];
    Vertex2D buf_b[MAX_VIEW_CLIP_VERTS];
    int count = 4;

    memcpy(buf_a, in, 4 * sizeof(Vertex2D));

    count = clip_polygon_against_boundary(buf_a, count, buf_b, CLIP_LEFT);
    if (count == 0)
        return 0;
    count = clip_polygon_against_boundary(buf_b, count, buf_a, CLIP_RIGHT);
    if (count == 0)
        return 0;
    count = clip_polygon_against_boundary(buf_a, count, buf_b, CLIP_TOP);
    if (count == 0)
        return 0;
    count = clip_polygon_against_boundary(buf_b, count, buf_a, CLIP_BOTTOM);
    if (count == 0)
        return 0;

    memcpy(out, buf_a, (size_t)count * sizeof(Vertex2D));
    return count;
}

static bool is_solid_block(BlockID id)
{
    return id != BLOCK_AIR;
}

static uint32_t hash_grid_coord(int x, int y, int z)
{
    return ((uint32_t)x * 73856093u) ^
           ((uint32_t)y * 19349663u) ^
           ((uint32_t)z * 83492791u);
}

static bool ensure_lookup_capacity(RenderContext *ctx, int num_blocks)
{
    int needed = 16;

    while (needed < num_blocks * 4)
        needed <<= 1;

    if (needed <= ctx->lookup_capacity)
        return true;

    LookupEntry *entries = realloc(ctx->lookup_entries,
                                   (size_t)needed * sizeof(*entries));
    if (!entries)
        return false;

    ctx->lookup_entries = entries;
    ctx->lookup_capacity = needed;
    return true;
}

static bool build_block_lookup(RenderContext *ctx, const Block *blocks, int num_blocks,
                               BlockLookup *lookup)
{
    if (!ensure_lookup_capacity(ctx, num_blocks))
        return false;

    memset(ctx->lookup_entries, 0,
           (size_t)ctx->lookup_capacity * sizeof(*ctx->lookup_entries));

    lookup->entries = ctx->lookup_entries;
    lookup->capacity = ctx->lookup_capacity;
    lookup->mask = ctx->lookup_capacity - 1;

    for (int i = 0; i < num_blocks; i++) {
        if (!is_solid_block(blocks[i].type))
            continue;

        int x = (int)lroundf(blocks[i].position.x);
        int y = (int)lroundf(blocks[i].position.y);
        int z = (int)lroundf(blocks[i].position.z);
        uint32_t idx = hash_grid_coord(x, y, z) & (uint32_t)lookup->mask;

        while (lookup->entries[idx].occupied) {
            if (lookup->entries[idx].x == x &&
                lookup->entries[idx].y == y &&
                lookup->entries[idx].z == z)
                break;
            idx = (idx + 1u) & (uint32_t)lookup->mask;
        }

        lookup->entries[idx].x = x;
        lookup->entries[idx].y = y;
        lookup->entries[idx].z = z;
        lookup->entries[idx].occupied = 1;
    }

    return true;
}

static bool lookup_has_solid_block_at(const BlockLookup *lookup, Vec3 pos)
{
    int x = (int)lroundf(pos.x);
    int y = (int)lroundf(pos.y);
    int z = (int)lroundf(pos.z);
    uint32_t idx = hash_grid_coord(x, y, z) & (uint32_t)lookup->mask;

    while (lookup->entries[idx].occupied) {
        if (lookup->entries[idx].x == x &&
            lookup->entries[idx].y == y &&
            lookup->entries[idx].z == z)
            return true;
        idx = (idx + 1u) & (uint32_t)lookup->mask;
    }

    return false;
}

static bool is_face_exposed(const Block *block, Vec3 normal,
                            const BlockLookup *lookup)
{
    Vec3 neighbor = {
        block->position.x + normal.x,
        block->position.y + normal.y,
        block->position.z + normal.z,
    };

    return !lookup_has_solid_block_at(lookup, neighbor);
}

/* Pack a float edge coefficient into Q24.8 (multiply by 256, round). */
static inline int32_t to_q24_8(float v)
{
    return (int32_t)roundf(v * 256.0f);
}

static void pack_edge_coef(struct edge_coef *edge,
                           float x0, float y0, float x1, float y1)
{
    float A = y0 - y1;
    float B = x1 - x0;
    float C = -(A * x0 + B * y0);
    float dx = x1 - x0;
    float dy = y1 - y0;

    edge->A = to_q24_8(A);
    edge->B = to_q24_8(B);
    edge->C = to_q24_8(C);

    /*
     * Use a top-left fill rule so adjacent quads share edges cleanly.
     * For our clockwise screen-space winding in y-down coordinates,
     * inclusive edges are:
     *   - upward edges
     *   - horizontal edges that run left-to-right
     *
     * Everything else becomes exclusive by subtracting one subpixel LSB.
     * This prevents top/side faces and coplanar neighboring quads from
     * double-covering the same shared edge.
     */
    if (fabsf(dx) < 1e-6f && fabsf(dy) < 1e-6f)
        return;

    if (!(dy < 0.0f || (fabsf(dy) < 1e-6f && dx > 0.0f)))
        edge->C -= 1;
}

static uint8_t block_palette_index(BlockID id)
{
    switch (id) {
    case BLOCK_GRASS:
        return 1;
    case BLOCK_DIRT:
        return 2;
    case BLOCK_WOOD:
        return 3;
    case BLOCK_STONE:
        return 4;
    default:
        return 5;
    }
}

/*
 * Temporary flat-shaded face palette until the texture path lands.
 * Keeping top/side/bottom distinct makes cube silhouettes much easier
 * to read at 320x240 than a single color per block type.
 */
static uint8_t flat_face_palette_index(BlockID id, BlockFace face)
{
    switch (id) {
    case BLOCK_GRASS:
        if (face == FACE_TOP)
            return 1;
        if (face == FACE_BOTTOM)
            return 10;
        return 9;
    case BLOCK_DIRT:
        if (face == FACE_TOP)
            return 15;
        if (face == FACE_BOTTOM)
            return 16;
        return 2;
    case BLOCK_WOOD:
        if (face == FACE_TOP)
            return 11;
        if (face == FACE_BOTTOM)
            return 12;
        return 3;
    case BLOCK_STONE:
        if (face == FACE_TOP)
            return 13;
        if (face == FACE_BOTTOM)
            return 14;
        return 4;
    default:
        return block_palette_index(id);
    }
}

/* Pack a float depth value into Q1.15 unsigned (clamp to [0,2)). */
static inline uint16_t to_q1_15u(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.99f) v = 1.99f;
    return (uint16_t)(v * 32768.0f);
}

/* Pack a float depth gradient into Q1.15 signed (clamp to [-1,1)). */
static inline int16_t to_q1_15s(float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v >  0.999f) v = 0.999f;
    return (int16_t)(v * 32768.0f);
}

static float quad_signed_area(const Vertex2D v[4])
{
    float area = 0.0f;

    for (int i = 0; i < 4; i++) {
        const Vertex2D *a = &v[i];
        const Vertex2D *b = &v[(i + 1) % 4];
        area += a->x * b->y - b->x * a->y;
    }

    return 0.5f * area;
}

static void ensure_clockwise_winding(Vertex2D v[4])
{
    if (quad_signed_area(v) >= 0.0f)
        return;

    Vertex2D tmp = v[1];
    v[1] = v[3];
    v[3] = tmp;
}

/* Fit a depth plane z(x,y) = z0 + dz_dx*x + dz_dy*y from 3 screen vertices.
 * Stores the plane at the raster sample point for (x_min, y_min). */
static bool solve_depth_plane(const Vertex2D *a, const Vertex2D *b,
                              const Vertex2D *c, float *ddx, float *ddy)
{
    float ax = b->x - a->x, ay = b->y - a->y, az = b->z - a->z;
    float bx = c->x - a->x, by = c->y - a->y, bz = c->z - a->z;

    float det = ax * by - ay * bx;
    if (fabsf(det) <= 1e-6f)
        return false;

    *ddx = ( by * az - ay * bz) / det;
    *ddy = (-bx * az + ax * bz) / det;
    return true;
}

static void fit_depth_plane(const Vertex2D v[4], float sample_x, float sample_y,
                            uint16_t *z0, int16_t *dz_dx, int16_t *dz_dy)
{
    static const int triples[4][3] = {
        { 0, 1, 2 },
        { 0, 1, 3 },
        { 0, 2, 3 },
        { 1, 2, 3 },
    };

    float ddx = 0.0f;
    float ddy = 0.0f;

    for (int i = 0; i < 4; i++) {
        if (solve_depth_plane(&v[triples[i][0]],
                              &v[triples[i][1]],
                              &v[triples[i][2]],
                              &ddx, &ddy))
            break;
    }

    float z_at_origin = v[0].z - ddx * v[0].x - ddy * v[0].y;
    float z_at_sample = z_at_origin + ddx * sample_x + ddy * sample_y;

    *z0    = to_q1_15u(z_at_sample);
    *dz_dx = to_q1_15s(ddx);
    *dz_dy = to_q1_15s(ddy);
}

static bool emit_camera_quad(RenderContext *ctx, const CameraVertex verts_cam[4],
                             uint8_t color_tint, uint8_t texture_id)
{
    Vertex2D verts[4];
    RenderQuad q;

    for (int i = 0; i < 4; i++) {
        if (!project_camera_vertex(ctx, &verts_cam[i], &verts[i]))
            return false;
    }

    memcpy(q.vertices, verts, sizeof(verts));
    q.texture_id = texture_id;
    q.color_tint = color_tint;
    return renderer_push_quad(ctx, &q);
}

static void emit_clipped_face(RenderContext *ctx, const CameraVertex *poly,
                              int count, uint8_t color_tint, uint8_t texture_id)
{
    if (count < 3)
        return;

    if (count == 3) {
        CameraVertex tri[4] = { poly[0], poly[1], poly[2], poly[2] };
        emit_camera_quad(ctx, tri, color_tint, texture_id);
        return;
    }

    if (count == 4) {
        CameraVertex quad[4] = { poly[0], poly[1], poly[2], poly[3] };
        emit_camera_quad(ctx, quad, color_tint, texture_id);
        return;
    }

    if (count == 5) {
        CameraVertex tri[4]  = { poly[0], poly[1], poly[2], poly[2] };
        CameraVertex quad[4] = { poly[0], poly[2], poly[3], poly[4] };
        emit_camera_quad(ctx, tri, color_tint, texture_id);
        emit_camera_quad(ctx, quad, color_tint, texture_id);
    }
}

static void emit_block_face(RenderContext *ctx, BlockID type,
                            Vec3 block_pos, BlockFace face)
{
    CameraVertex face_cam[4];

    if (type == BLOCK_AIR)
        return;
    if (face < 0 || face >= NUM_FACES)
        return;
    if (!is_face_visible(block_pos, face_normals[face],
                         ctx->current_camera.position))
        return;

    for (int i = 0; i < 4; i++) {
        Vec3 wp = {
            block_pos.x + face_verts[face][i].x,
            block_pos.y + face_verts[face][i].y,
            block_pos.z + face_verts[face][i].z,
        };
        world_to_camera(ctx, wp, &face_cam[i]);
        face_cam[i].u = (i == 1 || i == 2) ? 15.0f : 0.0f;
        face_cam[i].v = (i == 2 || i == 3) ? 15.0f : 0.0f;
    }

    CameraVertex clipped[6];
    int clipped_count = clip_face_to_near_plane(face_cam, clipped);
    emit_clipped_face(ctx, clipped, clipped_count,
                      flat_face_palette_index(type, face), 0);
}

static float distance_to_interval(float value, float min, float max)
{
    if (value < min)
        return min - value;
    if (value > max)
        return value - max;
    return 0.0f;
}

static bool chunk_within_render_distance(RenderContext *ctx,
                                         const VoxelWorld *world,
                                         const Chunk *chunk)
{
    float max_distance;
    float min_x;
    float max_x;
    float min_y = 0.0f;
    float max_y = (float)WORLD_CHUNK_HEIGHT;
    float min_z;
    float max_z;
    float dx;
    float dy;
    float dz;

    if (world->render_distance_chunks <= 0)
        return true;

    max_distance = (float)(world->render_distance_chunks * WORLD_CHUNK_SIZE);
    min_x = (float)(chunk->chunk_x * WORLD_CHUNK_SIZE);
    max_x = min_x + (float)WORLD_CHUNK_SIZE;
    min_z = (float)(chunk->chunk_z * WORLD_CHUNK_SIZE);
    max_z = min_z + (float)WORLD_CHUNK_SIZE;

    dx = distance_to_interval(ctx->current_camera.position.x, min_x, max_x);
    dy = distance_to_interval(ctx->current_camera.position.y, min_y, max_y);
    dz = distance_to_interval(ctx->current_camera.position.z, min_z, max_z);

    return dx * dx + dy * dy + dz * dz <= max_distance * max_distance;
}

static bool chunk_intersects_frustum(RenderContext *ctx, const Chunk *chunk)
{
    float min_x = (float)(chunk->chunk_x * WORLD_CHUNK_SIZE);
    float max_x = min_x + (float)WORLD_CHUNK_SIZE;
    float min_y = 0.0f;
    float max_y = (float)WORLD_CHUNK_HEIGHT;
    float min_z = (float)(chunk->chunk_z * WORLD_CHUNK_SIZE);
    float max_z = min_z + (float)WORLD_CHUNK_SIZE;
    float x_slope = (SCREEN_WIDTH * 0.5f) / ctx->current_camera.depth;
    float y_slope = (SCREEN_HEIGHT * 0.5f) / ctx->current_camera.depth;
    float plane_scale_x = sqrtf(1.0f + x_slope * x_slope);
    float plane_scale_y = sqrtf(1.0f + y_slope * y_slope);
    float radius_x = WORLD_CHUNK_SIZE * 0.5f;
    float radius_y = WORLD_CHUNK_HEIGHT * 0.5f;
    float radius_z = WORLD_CHUNK_SIZE * 0.5f;
    float radius = sqrtf(radius_x * radius_x +
                         radius_y * radius_y +
                         radius_z * radius_z);
    Vec3 camera_pos = ctx->current_camera.position;
    Vec3 chunk_center = {
        min_x + radius_x,
        min_y + radius_y,
        min_z + radius_z,
    };
    CameraVertex cam_center;

    /* A chunk containing the camera must never be frustum-culled. */
    if (camera_pos.x >= min_x && camera_pos.x <= max_x &&
        camera_pos.y >= min_y && camera_pos.y <= max_y &&
        camera_pos.z >= min_z && camera_pos.z <= max_z)
        return true;

    world_to_camera(ctx, chunk_center, &cam_center);

    if (cam_center.z + radius < NEAR_PLANE)
        return false;
    if (cam_center.x + x_slope * cam_center.z < -radius * plane_scale_x)
        return false;
    if (-cam_center.x + x_slope * cam_center.z < -radius * plane_scale_x)
        return false;
    if (-cam_center.y + y_slope * cam_center.z < -radius * plane_scale_y)
        return false;
    if (cam_center.y + y_slope * cam_center.z < -radius * plane_scale_y)
        return false;

    return true;
}

/* --- Public API --- */

RenderContext *renderer_init(void)
{
    RenderContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->fd = open(DEV_PATH, O_RDWR);
    if (ctx->fd < 0) {
        perror("open " DEV_PATH);
        free(ctx);
        return NULL;
    }

    ctx->staging = malloc(MAX_QUADS_IN_FLIGHT * sizeof(*ctx->staging));
    ctx->submit_buffer = malloc(MAX_QUADS_IN_FLIGHT * sizeof(*ctx->submit_buffer));
    ctx->lookup_entries = NULL;
    ctx->lookup_capacity = 0;
    if (!ctx->staging || !ctx->submit_buffer) {
        free(ctx->submit_buffer);
        free(ctx->staging);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    upload_default_palette(ctx->fd);
    return ctx;
}

void renderer_shutdown(RenderContext *ctx)
{
    if (!ctx) return;
    free(ctx->lookup_entries);
    free(ctx->submit_buffer);
    free(ctx->staging);
    close(ctx->fd);
    free(ctx);
}

void renderer_begin_frame(RenderContext *ctx)
{
    ctx->n_quads = 0;
    if (ioctl(ctx->fd, VOXEL_IOC_CLEAR_FRAME))
        perror("ioctl(CLEAR_FRAME)");
}

void renderer_end_frame(RenderContext *ctx)
{
    if (ctx->n_quads == 0) {
        if (ioctl(ctx->fd, VOXEL_IOC_FLIP))
            perror("ioctl(FLIP)");
        return;
    }

    for (int i = 0; i < ctx->n_quads; i++)
        ctx->submit_buffer[i] = ctx->staging[i].desc;

    size_t bytes = (size_t)ctx->n_quads * sizeof(struct quad_desc);
    ssize_t n = write(ctx->fd, ctx->submit_buffer, bytes);
    if (n < 0) {
        perror("write(quads)");
        return;
    }
    if ((size_t)n != bytes) {
        fprintf(stderr, "short write(quads): %zd / %zu\n", n, bytes);
        return;
    }

    if (ioctl(ctx->fd, VOXEL_IOC_FLIP))
        perror("ioctl(FLIP)");
}

void renderer_set_camera(RenderContext *ctx, const Camera *camera)
{
    ctx->current_camera = *camera;
    ctx->cos_yaw   = cosf(camera->yaw);
    ctx->sin_yaw   = sinf(camera->yaw);
    ctx->cos_pitch = cosf(camera->pitch);
    ctx->sin_pitch = sinf(camera->pitch);
}

static bool stage_prepared_quad(RenderContext *ctx, RenderQuad quad)
{
    if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
        return false;

    ensure_clockwise_winding(quad.vertices);

    const Vertex2D *v = quad.vertices;

    /* Inclusive integer bbox over the RTL's integer sample grid. */
    float fxmin = v[0].x, fxmax = v[0].x;
    float fymin = v[0].y, fymax = v[0].y;
    for (int i = 1; i < 4; i++) {
        if (v[i].x < fxmin) fxmin = v[i].x;
        if (v[i].x > fxmax) fxmax = v[i].x;
        if (v[i].y < fymin) fymin = v[i].y;
        if (v[i].y > fymax) fymax = v[i].y;
    }
    int x_min = (int)ceilf(fxmin);  if (x_min <   0) x_min =   0;
    int y_min = (int)ceilf(fymin);  if (y_min <   0) y_min =   0;
    int x_max = (int)floorf(fxmax); if (x_max > 319) x_max = 319;
    int y_max = (int)floorf(fymax); if (y_max > 239) y_max = 239;

    if (x_min > x_max || y_min > y_max)
        return false;   /* degenerate / off-screen */

    struct StagedQuad *staged = &ctx->staging[ctx->n_quads];
    struct quad_desc *d = &staged->desc;
    memset(staged, 0, sizeof(*staged));
    ctx->n_quads++;

    memset(d, 0, sizeof(*d));

    d->x_min = (int16_t)x_min;
    d->y_min = (int16_t)y_min;
    d->x_max = (int16_t)x_max;
    d->y_max = (int16_t)y_max;

    for (int i = 0; i < 4; i++) {
        float x0 = v[i].x,         y0 = v[i].y;
        float x1 = v[(i+1)%4].x,   y1 = v[(i+1)%4].y;
        pack_edge_coef(&d->edges[i], x0, y0, x1, y1);
    }

    /* Compute into locals first to avoid taking addresses of packed members. */
    uint16_t z0;
    int16_t dz_dx;
    int16_t dz_dy;
    fit_depth_plane(v, (float)x_min, (float)y_min,
                    &z0, &dz_dx, &dz_dy);
    d->z0 = z0;
    d->dz_dx = dz_dx;
    d->dz_dy = dz_dy;

    d->tex_or_color = quad.color_tint;
    d->flags        = QUAD_FLAG_ZTEST;

    return true;
}

bool renderer_push_quad(RenderContext *ctx, const RenderQuad *quad)
{
    Vertex2D clipped[MAX_VIEW_CLIP_VERTS];
    int count = clip_polygon_to_viewport(quad->vertices, clipped);

    if (count < 3)
        return false;

    if (count == 3) {
        RenderQuad tri = {
            .vertices = { clipped[0], clipped[1], clipped[2], clipped[2] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
        };
        return stage_prepared_quad(ctx, tri);
    }

    if (count == 4) {
        RenderQuad clipped_quad = {
            .vertices = { clipped[0], clipped[1], clipped[2], clipped[3] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
        };
        return stage_prepared_quad(ctx, clipped_quad);
    }

    if (count == 5) {
        RenderQuad tri = {
            .vertices = { clipped[0], clipped[1], clipped[2], clipped[2] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
        };
        RenderQuad clipped_quad = {
            .vertices = { clipped[0], clipped[2], clipped[3], clipped[4] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
        };
        return stage_prepared_quad(ctx, tri) &&
               stage_prepared_quad(ctx, clipped_quad);
    }

    for (int i = 1; i + 1 < count; i++) {
        RenderQuad tri = {
            .vertices = { clipped[0], clipped[i], clipped[i + 1], clipped[i + 1] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
        };
        if (!stage_prepared_quad(ctx, tri))
            return false;
    }

    return true;
}

static void renderer_draw_block_faces(RenderContext *ctx, const Block *block,
                                      const BlockLookup *lookup)
{
    if (block->type == BLOCK_AIR) return;

    for (int f = 0; f < NUM_FACES; f++) {
        if (!is_face_exposed(block, face_normals[f], lookup))
            continue;
        emit_block_face(ctx, block->type, block->position, (BlockFace)f);
    }
}

void renderer_draw_block(RenderContext *ctx, const Block *block)
{
    BlockLookup lookup;

    if (!build_block_lookup(ctx, block, 1, &lookup))
        return;

    renderer_draw_block_faces(ctx, block, &lookup);
}

int renderer_draw_chunk(RenderContext *ctx, const Block *blocks, int num_blocks)
{
    BlockLookup lookup;
    int before = ctx->n_quads;

    if (!build_block_lookup(ctx, blocks, num_blocks, &lookup))
        return 0;

    for (int i = 0; i < num_blocks; i++)
        renderer_draw_block_faces(ctx, &blocks[i], &lookup);

    return ctx->n_quads - before;
}

int renderer_draw_world(RenderContext *ctx, const VoxelWorld *world)
{
    int before = ctx->n_quads;

    if (!world)
        return 0;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!chunk->faces || chunk->face_count <= 0)
            continue;
        if (!chunk_within_render_distance(ctx, world, chunk))
            continue;
        if (!chunk_intersects_frustum(ctx, chunk))
            continue;

        for (int face_index = 0; face_index < chunk->face_count; face_index++) {
            const ChunkFace *face = &chunk->faces[face_index];
            Vec3 block_pos = {
                (float)(chunk->chunk_x * WORLD_CHUNK_SIZE + face->x),
                (float)face->y,
                (float)(chunk->chunk_z * WORLD_CHUNK_SIZE + face->z),
            };

            emit_block_face(ctx, (BlockID)face->type,
                            block_pos, (BlockFace)face->face);
        }
    }

    return ctx->n_quads - before;
}
