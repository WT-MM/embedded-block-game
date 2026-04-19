#include "renderer.h"
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

struct StagedQuad {
    struct quad_desc desc;
};

static void upload_default_palette(int fd)
{
    static const struct voxel_palette_entry entries[] = {
        { 0, 0x10, 0x10, 0x18 }, /* background */
        { 1, 0x4c, 0xaf, 0x50 }, /* grass */
        { 2, 0x8b, 0x45, 0x13 }, /* dirt */
        { 3, 0x65, 0x43, 0x21 }, /* wood */
        { 4, 0x80, 0x80, 0x80 }, /* stone */
        { 5, 0xff, 0xff, 0xff }, /* debug white */
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
    int n_quads;
};

typedef struct {
    float x, y, z;
    float u, v;
} CameraVertex;

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

static bool is_solid_block(BlockID id)
{
    return id != BLOCK_AIR;
}

static bool same_coord(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

static bool chunk_has_solid_block_at(const Block *blocks, int num_blocks, Vec3 pos)
{
    for (int i = 0; i < num_blocks; i++) {
        if (!is_solid_block(blocks[i].type))
            continue;
        if (!same_coord(blocks[i].position.x, pos.x))
            continue;
        if (!same_coord(blocks[i].position.y, pos.y))
            continue;
        if (!same_coord(blocks[i].position.z, pos.z))
            continue;
        return true;
    }

    return false;
}

static bool is_face_exposed(const Block *block, Vec3 normal,
                            const Block *blocks, int num_blocks)
{
    Vec3 neighbor = {
        block->position.x + normal.x,
        block->position.y + normal.y,
        block->position.z + normal.z,
    };

    return !chunk_has_solid_block_at(blocks, num_blocks, neighbor);
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

    /*
     * The RTL evaluates edge functions at integer sample locations.
     * Bias C so those integer coordinates correspond to pixel centers.
     */
    C += 0.5f * (A + B);

    edge->A = to_q24_8(A);
    edge->B = to_q24_8(B);
    edge->C = to_q24_8(C);
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
static void fit_depth_plane(const Vertex2D v[4], float sample_x, float sample_y,
                             uint16_t *z0, int16_t *dz_dx, int16_t *dz_dy)
{
    /* Use vertices 0, 1, 2. Solve the 2x2 system for gradients. */
    float ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, az = v[1].z - v[0].z;
    float bx = v[2].x - v[0].x, by = v[2].y - v[0].y, bz = v[2].z - v[0].z;

    float det = ax * by - ay * bx;
    float ddx = 0.0f, ddy = 0.0f;
    if (fabsf(det) > 1e-6f) {
        ddx = ( by * az - ay * bz) / det;
        ddy = (-bx * az + ax * bz) / det;
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

    ensure_clockwise_winding(verts);

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

bool renderer_push_quad(RenderContext *ctx, const RenderQuad *quad)
{
    if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
        return false;

    const Vertex2D *v = quad->vertices;

    /* Inclusive integer bbox over pixel-center samples. */
    float fxmin = v[0].x, fxmax = v[0].x;
    float fymin = v[0].y, fymax = v[0].y;
    for (int i = 1; i < 4; i++) {
        if (v[i].x < fxmin) fxmin = v[i].x;
        if (v[i].x > fxmax) fxmax = v[i].x;
        if (v[i].y < fymin) fymin = v[i].y;
        if (v[i].y > fymax) fymax = v[i].y;
    }
    int x_min = (int)ceilf(fxmin - 0.5f); if (x_min <   0) x_min =   0;
    int y_min = (int)ceilf(fymin - 0.5f); if (y_min <   0) y_min =   0;
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

    d->tex_or_color = quad->color_tint;
    d->flags        = QUAD_FLAG_ZTEST;

    return true;
}

static void renderer_draw_block_faces(RenderContext *ctx, const Block *block,
                                      const Block *blocks, int num_blocks)
{
    if (block->type == BLOCK_AIR) return;

    static const Vec3 normals[NUM_FACES] = {
        {0,1,0}, {0,-1,0}, {-1,0,0}, {1,0,0}, {0,0,-1}, {0,0,1}
    };
    static const Vec3 face_verts[NUM_FACES][4] = {
        {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},  /* TOP    */
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},  /* BOTTOM */
        {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},  /* LEFT   */
        {{1,0,1},{1,0,0},{1,1,0},{1,1,1}},  /* RIGHT  */
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}},  /* FRONT  */
        {{1,0,1},{1,1,1},{0,1,1},{0,0,1}},  /* BACK   */
    };

    for (int f = 0; f < NUM_FACES; f++) {
        if (!is_face_exposed(block, normals[f], blocks, num_blocks))
            continue;
        if (!is_face_visible(block->position, normals[f],
                             ctx->current_camera.position))
            continue;

        CameraVertex face_cam[4];
        for (int i = 0; i < 4; i++) {
            Vec3 wp = {
                block->position.x + face_verts[f][i].x,
                block->position.y + face_verts[f][i].y,
                block->position.z + face_verts[f][i].z,
            };
            world_to_camera(ctx, wp, &face_cam[i]);
            face_cam[i].u = (i == 1 || i == 2) ? 15.0f : 0.0f;
            face_cam[i].v = (i == 2 || i == 3) ? 15.0f : 0.0f;
        }

        CameraVertex clipped[6];
        int clipped_count = clip_face_to_near_plane(face_cam, clipped);
        emit_clipped_face(ctx, clipped, clipped_count,
                          block_palette_index(block->type), 0);
    }
}

void renderer_draw_block(RenderContext *ctx, const Block *block)
{
    renderer_draw_block_faces(ctx, block, block, 1);
}

int renderer_draw_chunk(RenderContext *ctx, const Block *blocks, int num_blocks)
{
    int before = ctx->n_quads;
    for (int i = 0; i < num_blocks; i++)
        renderer_draw_block_faces(ctx, &blocks[i], blocks, num_blocks);
    return ctx->n_quads - before;
}
