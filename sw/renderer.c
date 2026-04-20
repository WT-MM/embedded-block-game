#include "renderer.h"
#include "world.h"
#include "gpu_transport.h"
#include "voxel_gpu.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define NEAR_PLANE 0.1f
/*
 * Projected screen coordinates live on pixel-box edges: the visible viewport
 * spans [0, SCREEN_WIDTH] x [0, SCREEN_HEIGHT], while raster samples sit at
 * pixel centers (x + 0.5, y + 0.5). Keep clipping in edge space, then bake the
 * 0.5 center offset into edge/depth setup before descriptors hit the FPGA.
 */
#define VIEW_X_MIN 0.0f
#define VIEW_Y_MIN 0.0f
#define VIEW_X_MAX SCREEN_WIDTH
#define VIEW_Y_MAX SCREEN_HEIGHT
#define MAX_VIEW_CLIP_VERTS 8

struct StagedQuad {
    struct quad_desc desc;
    struct quad_desc_uv uv;
    size_t descriptor_bytes;
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

static void upload_default_palette(GPUTransport *transport)
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
        { 17, 0x2f, 0x82, 0x3a }, /* grass dark */
        { 18, 0x78, 0xd2, 0x65 }, /* grass highlight */
        { 19, 0x6c, 0x3b, 0x15 }, /* dirt dark */
        { 20, 0xb0, 0x7a, 0x47 }, /* dirt light */
        { 21, 0x84, 0x54, 0x26 }, /* wood grain */
        { 22, 0x43, 0x26, 0x12 }, /* wood bark dark */
        { 23, 0x6e, 0x6e, 0x6e }, /* stone dark */
        { 24, 0xc3, 0xc3, 0xc3 }, /* stone light */
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (gpu_transport_set_palette(transport, &entries[i]) < 0)
            break;
    }
}

struct RenderContext {
    GPUTransport *transport;
    Camera current_camera;

    float cos_yaw, sin_yaw;
    float cos_pitch, sin_pitch;

    struct StagedQuad *staging;
    uint8_t *submit_buffer;
    size_t submit_capacity;
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
    float inv_w = 1.0f / in->z;
    out->x = in->x * inv_w * depth + SCREEN_WIDTH  / 2.0f;
    out->y = SCREEN_HEIGHT / 2.0f - in->y * inv_w * depth;
    /*
     * Store u/w, v/w, 1/w instead of u, v. These three quantities are linear
     * in screen space for any planar world-space quad, so viewport clipping
     * can lerp them correctly and the HW can recover true u, v by dividing
     * per pixel. This eliminates the affine-texture swim under perspective.
     */
    out->u_over_w   = in->u * inv_w;
    out->v_over_w   = in->v * inv_w;
    out->one_over_w = inv_w;
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
        .u_over_w   = a->u_over_w   + (b->u_over_w   - a->u_over_w)   * t,
        .v_over_w   = a->v_over_w   + (b->v_over_w   - a->v_over_w)   * t,
        .one_over_w = a->one_over_w + (b->one_over_w - a->one_over_w) * t,
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

static inline float snap_q24_8(float v)
{
    return (float)to_q24_8(v) / 256.0f;
}

static bool same_screen_xy(const Vertex2D *a, const Vertex2D *b)
{
    return a->x == b->x && a->y == b->y;
}

static int snap_and_compact_polygon(Vertex2D *verts, int count)
{
    Vertex2D compact[MAX_VIEW_CLIP_VERTS];
    int out_count = 0;

    for (int i = 0; i < count; i++) {
        verts[i].x = snap_q24_8(verts[i].x);
        verts[i].y = snap_q24_8(verts[i].y);

        if (out_count > 0 && same_screen_xy(&compact[out_count - 1], &verts[i]))
            continue;

        compact[out_count++] = verts[i];
    }

    if (out_count > 1 && same_screen_xy(&compact[0], &compact[out_count - 1]))
        out_count--;

    memcpy(verts, compact, (size_t)out_count * sizeof(*verts));
    return out_count;
}

static void pack_edge_coef(struct edge_coef *edge,
                           float x0, float y0, float x1, float y1)
{
    float A = y0 - y1;
    float B = x1 - x0;
    float C = -(A * x0 + B * y0);
    float dx = x1 - x0;
    float dy = y1 - y0;

    /*
     * The RTL evaluates E(x,y) at integer sample locations. Shift C so those
     * integer coordinates correspond to pixel centers (x + 0.5, y + 0.5).
     */
    C += 0.5f * (A + B);

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
     * This prevents shared-edge cracks and inconsistent side/bottom coverage.
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
    return (uint16_t)lroundf(v * 32768.0f);
}

/* Pack a float depth gradient into Q1.15 signed (clamp to [-1,1)). */
static inline int16_t to_q1_15s(float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v >  0.999f) v = 0.999f;
    return (int16_t)lroundf(v * 32768.0f);
}

static inline int32_t to_q16_16(float v)
{
    return (int32_t)lroundf(v * 65536.0f);
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

static bool solve_attr_plane(const Vertex2D *a, const Vertex2D *b,
                             const Vertex2D *c,
                             float attr_a, float attr_b, float attr_c,
                             float *ddx, float *ddy)
{
    float ax = b->x - a->x, ay = b->y - a->y, av = attr_b - attr_a;
    float bx = c->x - a->x, by = c->y - a->y, bv = attr_c - attr_a;

    float det = ax * by - ay * bx;
    if (fabsf(det) <= 1e-6f)
        return false;

    *ddx = ( by * av - ay * bv) / det;
    *ddy = (-bx * av + ax * bv) / det;
    return true;
}

/*
 * Deterministic triple selection.
 *
 * For a planar world-space quad, depth (in the 1 - near/z mapping), u/w, v/w
 * and 1/w are all exactly linear in screen space, so any 3 of the 4 projected
 * vertices should yield the same plane. Clip vertices, Q24.8 snapping and
 * floating-point rounding can still produce tiny disagreements between
 * triples; picking the first non-degenerate triple in a fixed order removes
 * the frame-to-frame jitter where the "winning" triple flipped under
 * sub-pixel camera motion.
 */
static const int kAttrTriples[4][3] = {
    { 0, 1, 2 },
    { 0, 1, 3 },
    { 0, 2, 3 },
    { 1, 2, 3 },
};

/* Fit a depth plane z(x,y) = z0 + dz_dx*x + dz_dy*y from 3 screen vertices.
 * Stores the plane at the requested raster sample point. */
static void fit_depth_plane(const Vertex2D v[4], float sample_x, float sample_y,
                            uint16_t *z0, int16_t *dz_dx, int16_t *dz_dy)
{
    float ddx = 0.0f;
    float ddy = 0.0f;
    int basis = 0;

    for (int i = 0; i < 4; i++) {
        int a = kAttrTriples[i][0];
        int b = kAttrTriples[i][1];
        int c = kAttrTriples[i][2];
        if (solve_attr_plane(&v[a], &v[b], &v[c],
                             v[a].z, v[b].z, v[c].z,
                             &ddx, &ddy)) {
            basis = a;
            break;
        }
    }

    float z_at_origin = v[basis].z - ddx * v[basis].x - ddy * v[basis].y;
    float z_at_sample = z_at_origin + ddx * sample_x + ddy * sample_y;

    *z0    = to_q1_15u(z_at_sample);
    *dz_dx = to_q1_15s(ddx);
    *dz_dy = to_q1_15s(ddy);
}

/*
 * Fit three screen-space planes for perspective-correct texturing:
 *   u/w (x,y), v/w (x,y), 1/w (x,y)
 *
 * The rasterizer interpolates each plane linearly per pixel and then divides
 * to recover true u, v. All three attributes are exactly linear in screen
 * space for a planar world-space quad, so a single deterministic 3-vertex fit
 * captures the full plane (modulo FP noise).
 */
static void fit_uv_plane(const Vertex2D v[4], float sample_x, float sample_y,
                         struct quad_desc_uv *uv)
{
    float duw_dx = 0.0f, duw_dy = 0.0f;
    float dvw_dx = 0.0f, dvw_dy = 0.0f;
    float diw_dx = 0.0f, diw_dy = 0.0f;
    int basis = 0;

    for (int i = 0; i < 4; i++) {
        int a = kAttrTriples[i][0];
        int b = kAttrTriples[i][1];
        int c = kAttrTriples[i][2];
        if (!solve_attr_plane(&v[a], &v[b], &v[c],
                              v[a].u_over_w, v[b].u_over_w, v[c].u_over_w,
                              &duw_dx, &duw_dy))
            continue;
        if (!solve_attr_plane(&v[a], &v[b], &v[c],
                              v[a].v_over_w, v[b].v_over_w, v[c].v_over_w,
                              &dvw_dx, &dvw_dy))
            continue;
        if (!solve_attr_plane(&v[a], &v[b], &v[c],
                              v[a].one_over_w, v[b].one_over_w, v[c].one_over_w,
                              &diw_dx, &diw_dy))
            continue;
        basis = a;
        break;
    }

    float uw_origin = v[basis].u_over_w   - duw_dx * v[basis].x - duw_dy * v[basis].y;
    float vw_origin = v[basis].v_over_w   - dvw_dx * v[basis].x - dvw_dy * v[basis].y;
    float iw_origin = v[basis].one_over_w - diw_dx * v[basis].x - diw_dy * v[basis].y;
    float uw_sample = uw_origin + duw_dx * sample_x + duw_dy * sample_y;
    float vw_sample = vw_origin + dvw_dx * sample_x + dvw_dy * sample_y;
    float iw_sample = iw_origin + diw_dx * sample_x + diw_dy * sample_y;

    uv->u_over_w_0    = to_q16_16(uw_sample);
    uv->u_over_w_dx   = to_q16_16(duw_dx);
    uv->u_over_w_dy   = to_q16_16(duw_dy);
    uv->v_over_w_0    = to_q16_16(vw_sample);
    uv->v_over_w_dx   = to_q16_16(dvw_dx);
    uv->v_over_w_dy   = to_q16_16(dvw_dy);
    uv->one_over_w_0  = to_q16_16(iw_sample);
    uv->one_over_w_dx = to_q16_16(diw_dx);
    uv->one_over_w_dy = to_q16_16(diw_dy);
}

static bool emit_camera_quad(RenderContext *ctx, const CameraVertex verts_cam[4],
                             uint8_t color_tint, uint8_t texture_id)
{
    Vertex2D verts[4];
    RenderQuad q = {0};

    for (int i = 0; i < 4; i++) {
        if (!project_camera_vertex(ctx, &verts_cam[i], &verts[i]))
            return false;
    }

    memcpy(q.vertices, verts, sizeof(verts));
    q.texture_id = texture_id;
    q.color_tint = color_tint;
    q.flags = QUAD_FLAG_ZTEST | QUAD_FLAG_TEX;
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

    for (int i = 1; i + 1 < count; i++) {
        CameraVertex tri[4] = { poly[0], poly[i], poly[i + 1], poly[i + 1] };
        emit_camera_quad(ctx, tri, color_tint, texture_id);
    }
}

static void emit_block_face(RenderContext *ctx, BlockID type,
                            Vec3 block_pos, BlockFace face)
{
    static const float tile_span = 16.0f;
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
        face_cam[i].u = (i == 1 || i == 2) ? tile_span : 0.0f;
        /*
         * Atlas row 0 sits at the top of each tile image, and the side faces'
         * vertex 0 is world-bottom. Invert V so world-top maps to texture-top.
         */
        face_cam[i].v = (i == 2 || i == 3) ? 0.0f : tile_span;
    }

    CameraVertex clipped[6];
    int clipped_count = clip_face_to_near_plane(face_cam, clipped);
    emit_clipped_face(ctx, clipped, clipped_count,
                      flat_face_palette_index(type, face),
                      block_face_texture_id(type, face));
}

static float distance_to_interval(float value, float min, float max)
{
    if (value < min)
        return min - value;
    if (value > max)
        return value - max;
    return 0.0f;
}

typedef struct {
    CameraVertex center;
    float ex;
    float ey;
    float ez;
} ChunkCameraBounds;

static void chunk_camera_bounds(RenderContext *ctx, const Chunk *chunk,
                                ChunkCameraBounds *bounds)
{
    float half_x = WORLD_CHUNK_SIZE * 0.5f;
    float half_y = WORLD_CHUNK_HEIGHT * 0.5f;
    float half_z = WORLD_CHUNK_SIZE * 0.5f;
    float cy = ctx->cos_yaw;
    float sy = ctx->sin_yaw;
    float cp = ctx->cos_pitch;
    float sp = ctx->sin_pitch;
    Vec3 chunk_center = {
        (float)(chunk->chunk_x * WORLD_CHUNK_SIZE) + half_x,
        half_y,
        (float)(chunk->chunk_z * WORLD_CHUNK_SIZE) + half_z,
    };

    world_to_camera(ctx, chunk_center, &bounds->center);

    /* Project world-space half-extents into camera space using abs(R) * e. */
    bounds->ex = fabsf(cy) * half_x + fabsf(sy) * half_z;
    bounds->ey = fabsf(sy * sp) * half_x +
                 fabsf(cp) * half_y +
                 fabsf(cy * sp) * half_z;
    bounds->ez = fabsf(sy * cp) * half_x +
                 fabsf(sp) * half_y +
                 fabsf(cy * cp) * half_z;
}

static bool chunk_within_render_distance(RenderContext *ctx,
                                         const VoxelWorld *world,
                                         const Chunk *chunk)
{
    float max_depth;
    ChunkCameraBounds bounds;

    if (world->render_distance_chunks <= 0)
        return true;

    max_depth = (float)(world->render_distance_chunks * WORLD_CHUNK_SIZE);
    chunk_camera_bounds(ctx, chunk, &bounds);

    /* Render-distance should track view depth, not spherical distance from
     * the camera, otherwise high-altitude views punch holes in the terrain. */
    return bounds.center.z + bounds.ez >= NEAR_PLANE &&
           bounds.center.z - bounds.ez <= max_depth;
}

static bool chunk_intersects_frustum(RenderContext *ctx, const Chunk *chunk)
{
    float x_slope = (SCREEN_WIDTH * 0.5f) / ctx->current_camera.depth;
    float y_slope = (SCREEN_HEIGHT * 0.5f) / ctx->current_camera.depth;
    ChunkCameraBounds bounds;

    chunk_camera_bounds(ctx, chunk, &bounds);

    if (bounds.center.z + bounds.ez < NEAR_PLANE)
        return false;
    if (bounds.center.x + x_slope * bounds.center.z +
        (bounds.ex + x_slope * bounds.ez) < 0.0f)
        return false;
    if (-bounds.center.x + x_slope * bounds.center.z +
        (bounds.ex + x_slope * bounds.ez) < 0.0f)
        return false;
    if (bounds.center.y + y_slope * bounds.center.z +
        (bounds.ey + y_slope * bounds.ez) < 0.0f)
        return false;
    if (-bounds.center.y + y_slope * bounds.center.z +
        (bounds.ey + y_slope * bounds.ez) < 0.0f)
        return false;

    return true;
}

typedef struct {
    const Chunk *chunk;
    float distance_sq;
} ChunkDrawCandidate;

static float chunk_distance_sq_to_camera(RenderContext *ctx, const Chunk *chunk)
{
    float min_x = (float)(chunk->chunk_x * WORLD_CHUNK_SIZE);
    float max_x = min_x + (float)WORLD_CHUNK_SIZE;
    float min_y = 0.0f;
    float max_y = (float)WORLD_CHUNK_HEIGHT;
    float min_z = (float)(chunk->chunk_z * WORLD_CHUNK_SIZE);
    float max_z = min_z + (float)WORLD_CHUNK_SIZE;
    float dx = distance_to_interval(ctx->current_camera.position.x, min_x, max_x);
    float dy = distance_to_interval(ctx->current_camera.position.y, min_y, max_y);
    float dz = distance_to_interval(ctx->current_camera.position.z, min_z, max_z);

    return dx * dx + dy * dy + dz * dz;
}

/* --- Public API --- */

RenderContext *renderer_init(void)
{
    RenderContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->transport = gpu_transport_open();
    if (!ctx->transport) {
        free(ctx);
        return NULL;
    }

    ctx->staging = malloc(MAX_QUADS_IN_FLIGHT * sizeof(*ctx->staging));
    ctx->submit_capacity = MAX_QUADS_IN_FLIGHT *
                           (sizeof(struct quad_desc) + sizeof(struct quad_desc_uv));
    ctx->submit_buffer = malloc(ctx->submit_capacity);
    ctx->lookup_entries = NULL;
    ctx->lookup_capacity = 0;
    if (!ctx->staging || !ctx->submit_buffer) {
        free(ctx->submit_buffer);
        free(ctx->staging);
        gpu_transport_close(ctx->transport);
        free(ctx);
        return NULL;
    }

    upload_default_palette(ctx->transport);
    return ctx;
}

void renderer_shutdown(RenderContext *ctx)
{
    if (!ctx) return;
    free(ctx->lookup_entries);
    free(ctx->submit_buffer);
    free(ctx->staging);
    gpu_transport_close(ctx->transport);
    free(ctx);
}

void renderer_begin_frame(RenderContext *ctx)
{
    ctx->n_quads = 0;
    gpu_transport_clear(ctx->transport);
}

void renderer_end_frame(RenderContext *ctx)
{
    if (ctx->n_quads == 0) {
        gpu_transport_flip(ctx->transport);
        return;
    }

    size_t submit_bytes = 0;
    for (int i = 0; i < ctx->n_quads; i++) {
        memcpy(ctx->submit_buffer + submit_bytes,
               &ctx->staging[i].desc, sizeof(ctx->staging[i].desc));
        submit_bytes += sizeof(ctx->staging[i].desc);
        if (ctx->staging[i].descriptor_bytes > sizeof(ctx->staging[i].desc)) {
            memcpy(ctx->submit_buffer + submit_bytes,
                   &ctx->staging[i].uv, sizeof(ctx->staging[i].uv));
            submit_bytes += sizeof(ctx->staging[i].uv);
        }
    }

    if (gpu_transport_submit_descriptors(ctx->transport,
                                         ctx->submit_buffer,
                                         submit_bytes) < 0)
        return;

    gpu_transport_flip(ctx->transport);
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

    /*
     * The rasterizer only sees Q24.8 edge math. Snap screen-space vertices to
     * that same grid before bbox, edge, and depth setup so all three stages
     * agree on which pixels a thin quad should cover.
     */
    for (int i = 0; i < 4; i++) {
        quad.vertices[i].x = snap_q24_8(quad.vertices[i].x);
        quad.vertices[i].y = snap_q24_8(quad.vertices[i].y);
    }

    ensure_clockwise_winding(quad.vertices);

    const Vertex2D *v = quad.vertices;

    /* Inclusive integer bbox over pixel-center samples. */
    float fxmin = v[0].x, fxmax = v[0].x;
    float fymin = v[0].y, fymax = v[0].y;
    for (int i = 1; i < 4; i++) {
        if (v[i].x < fxmin) fxmin = v[i].x;
        if (v[i].x > fxmax) fxmax = v[i].x;
        if (v[i].y < fymin) fymin = v[i].y;
        if (v[i].y > fymax) fymax = v[i].y;
    }
    int x_min = (int)ceilf(fxmin - 0.5f);  if (x_min <   0) x_min =   0;
    int y_min = (int)ceilf(fymin - 0.5f);  if (y_min <   0) y_min =   0;
    int x_max = (int)floorf(fxmax - 0.5f); if (x_max > 319) x_max = 319;
    int y_max = (int)floorf(fymax - 0.5f); if (y_max > 239) y_max = 239;

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
    fit_depth_plane(v, (float)x_min + 0.5f, (float)y_min + 0.5f,
                    &z0, &dz_dx, &dz_dy);
    d->z0 = z0;
    d->dz_dx = dz_dx;
    d->dz_dy = dz_dy;

    d->tex_or_color = (quad.flags & QUAD_FLAG_TEX) ? quad.texture_id : quad.color_tint;
    d->flags = quad.flags;

    if (quad.flags & QUAD_FLAG_TEX) {
        fit_uv_plane(v, (float)x_min + 0.5f, (float)y_min + 0.5f, &staged->uv);
        staged->descriptor_bytes = sizeof(staged->desc) + sizeof(staged->uv);
    } else {
        staged->descriptor_bytes = sizeof(staged->desc);
    }

    return true;
}

bool renderer_push_quad(RenderContext *ctx, const RenderQuad *quad)
{
    Vertex2D clipped[MAX_VIEW_CLIP_VERTS];
    int count = clip_polygon_to_viewport(quad->vertices, clipped);

    /*
     * Viewport clipping can create duplicate boundary vertices once we snap to
     * the Q24.8 raster grid. Compact first, then triangulate any larger n-gons
     * so clipped slivers do not turn into missing wedges at the screen edge.
     */
    count = snap_and_compact_polygon(clipped, count);
    if (count < 3)
        return false;

    if (count == 3) {
        RenderQuad tri = {
            .vertices = { clipped[0], clipped[1], clipped[2], clipped[2] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
            .flags = quad->flags,
        };
        return stage_prepared_quad(ctx, tri);
    }

    if (count == 4) {
        RenderQuad clipped_quad = {
            .vertices = { clipped[0], clipped[1], clipped[2], clipped[3] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
            .flags = quad->flags,
        };
        return stage_prepared_quad(ctx, clipped_quad);
    }

    bool emitted = false;
    for (int i = 1; i + 1 < count; i++) {
        RenderQuad tri = {
            .vertices = { clipped[0], clipped[i], clipped[i + 1], clipped[i + 1] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
            .flags = quad->flags,
        };
        if (stage_prepared_quad(ctx, tri))
            emitted = true;
    }

    return emitted;
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
    int candidate_count = 0;

    if (!world || world->chunk_count <= 0)
        return 0;

    ChunkDrawCandidate candidates[world->chunk_count];

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!chunk->faces || chunk->face_count <= 0)
            continue;
        if (!chunk_within_render_distance(ctx, world, chunk))
            continue;
        if (!chunk_intersects_frustum(ctx, chunk))
            continue;

        candidates[candidate_count++] = (ChunkDrawCandidate){
            .chunk = chunk,
            .distance_sq = chunk_distance_sq_to_camera(ctx, chunk),
        };
    }

    /* Draw nearest chunks first so any future budget limit degrades
     * gracefully instead of dropping arbitrary sides of the world. */
    for (int i = 1; i < candidate_count; i++) {
        ChunkDrawCandidate key = candidates[i];
        int j = i - 1;

        while (j >= 0 && candidates[j].distance_sq > key.distance_sq) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    for (int i = 0; i < candidate_count; i++) {
        const Chunk *chunk = candidates[i].chunk;

        for (int face_index = 0; face_index < chunk->face_count; face_index++) {
            const ChunkFace *face = &chunk->faces[face_index];
            Vec3 block_pos = {
                (float)(chunk->chunk_x * WORLD_CHUNK_SIZE + face->x),
                (float)face->y,
                (float)(chunk->chunk_z * WORLD_CHUNK_SIZE + face->z),
            };

            emit_block_face(ctx, (BlockID)face->type,
                            block_pos, (BlockFace)face->face);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
                return ctx->n_quads - before;
        }
    }

    return ctx->n_quads - before;
}

bool renderer_draw_crosshair(RenderContext *ctx)
{
    const float cx = SCREEN_WIDTH * 0.5f;
    const float cy = SCREEN_HEIGHT * 0.5f;
    const float half = 8.0f;
    const float x0 = cx - half, y0 = cy - half;
    const float x1 = cx + half, y1 = cy + half;
    const float tile = 16.0f;

    RenderQuad quad = {0};
    quad.texture_id = TEX_TILE_CROSSHAIR;
    quad.flags = QUAD_FLAG_TEX | QUAD_FLAG_ALPHA_KEY;
    quad.vertices[0] = (Vertex2D){ x0, y0, 0.0f, 0.0f,  0.0f,  1.0f };
    quad.vertices[1] = (Vertex2D){ x1, y0, 0.0f, tile,  0.0f,  1.0f };
    quad.vertices[2] = (Vertex2D){ x1, y1, 0.0f, tile,  tile,  1.0f };
    quad.vertices[3] = (Vertex2D){ x0, y1, 0.0f, 0.0f,  tile,  1.0f };

    return renderer_push_quad(ctx, &quad);
}
