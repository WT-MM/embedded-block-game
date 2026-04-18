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

    struct quad_desc *staging;
    int n_quads;
};

/* Project a world-space point into screen space.
 * Returns false if the point is behind the near plane. */
static bool project_point(RenderContext *ctx, Vec3 world, Vertex2D *out)
{
    float dx = world.x - ctx->current_camera.position.x;
    float dy = world.y - ctx->current_camera.position.y;
    float dz = world.z - ctx->current_camera.position.z;

    float x_yaw =  dx * ctx->cos_yaw - dz * ctx->sin_yaw;
    float z_yaw =  dx * ctx->sin_yaw + dz * ctx->cos_yaw;
    float y_yaw =  dy;

    float x_cam = x_yaw;
    float y_cam = y_yaw * ctx->cos_pitch - z_yaw * ctx->sin_pitch;
    float z_cam = y_yaw * ctx->sin_pitch + z_yaw * ctx->cos_pitch;

    out->z = z_cam;

    if (z_cam <= 0.1f)
        return false;

    float depth = ctx->current_camera.depth;
    out->x = (x_cam / z_cam) * depth + SCREEN_WIDTH  / 2.0f;
    out->y = (y_cam / z_cam) * depth + SCREEN_HEIGHT / 2.0f;
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

/* Pack a float edge coefficient into Q24.8 (multiply by 256, round). */
static inline int32_t to_q24_8(float v)
{
    return (int32_t)roundf(v * 256.0f);
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

/* Fit a depth plane z(x,y) = z0 + dz_dx*x + dz_dy*y from 3 screen vertices.
 * Stores the plane at (x_min, y_min). */
static void fit_depth_plane(Vertex2D v[4], int x_min, int y_min,
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
    float z_at_min    = z_at_origin + ddx * x_min + ddy * y_min;

    *z0    = to_q1_15u(z_at_min);
    *dz_dx = to_q1_15s(ddx);
    *dz_dy = to_q1_15s(ddy);
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

    ctx->staging = malloc(MAX_QUADS_IN_FLIGHT * sizeof(struct quad_desc));
    if (!ctx->staging) {
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

    size_t bytes = (size_t)ctx->n_quads * sizeof(struct quad_desc);
    ssize_t n = write(ctx->fd, ctx->staging, bytes);
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

void renderer_set_camera(RenderContext *ctx, Camera *camera)
{
    ctx->current_camera = *camera;
    ctx->cos_yaw   = cosf(camera->yaw);
    ctx->sin_yaw   = sinf(camera->yaw);
    ctx->cos_pitch = cosf(camera->pitch);
    ctx->sin_pitch = sinf(camera->pitch);
}

bool renderer_push_quad(RenderContext *ctx, RenderQuad *quad)
{
    if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
        return false;

    Vertex2D *v = quad->vertices;

    /* Bounding box, clamped to viewport */
    float fxmin = v[0].x, fxmax = v[0].x;
    float fymin = v[0].y, fymax = v[0].y;
    for (int i = 1; i < 4; i++) {
        if (v[i].x < fxmin) fxmin = v[i].x;
        if (v[i].x > fxmax) fxmax = v[i].x;
        if (v[i].y < fymin) fymin = v[i].y;
        if (v[i].y > fymax) fymax = v[i].y;
    }
    int x_min = (int)fxmin; if (x_min <   0) x_min =   0;
    int y_min = (int)fymin; if (y_min <   0) y_min =   0;
    int x_max = (int)fxmax; if (x_max > 319) x_max = 319;
    int y_max = (int)fymax; if (y_max > 239) y_max = 239;

    if (x_min >= x_max || y_min >= y_max)
        return false;   /* degenerate / off-screen */

    struct quad_desc *d = &ctx->staging[ctx->n_quads++];
    memset(d, 0, sizeof(*d));

    d->x_min = (int16_t)x_min;
    d->y_min = (int16_t)y_min;
    d->x_max = (int16_t)x_max;
    d->y_max = (int16_t)y_max;

    /* Edge coefficients: vertices in clockwise screen order.
     * For edge from (x0,y0) to (x1,y1):
     *   A = y0 - y1,  B = x1 - x0,  C = -(A*x0 + B*y0)         */
    for (int i = 0; i < 4; i++) {
        float x0 = v[i].x,         y0 = v[i].y;
        float x1 = v[(i+1)%4].x,   y1 = v[(i+1)%4].y;
        float A  =  y0 - y1;
        float B  =  x1 - x0;
        float C  = -(A * x0 + B * y0);
        d->edges[i].A = to_q24_8(A);
        d->edges[i].B = to_q24_8(B);
        d->edges[i].C = to_q24_8(C);
    }

    /* Compute into locals first to avoid taking addresses of packed members. */
    uint16_t z0;
    int16_t dz_dx;
    int16_t dz_dy;
    fit_depth_plane(v, x_min, y_min, &z0, &dz_dx, &dz_dy);
    d->z0 = z0;
    d->dz_dx = dz_dx;
    d->dz_dy = dz_dy;

    d->tex_or_color = quad->color_tint;
    d->flags        = 0;

    return true;
}

void renderer_draw_block(RenderContext *ctx, Block *block)
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
        if (!is_face_visible(block->position, normals[f],
                             ctx->current_camera.position))
            continue;

        Vertex2D verts[4];
        bool behind = false;
        for (int i = 0; i < 4; i++) {
            Vec3 wp = {
                block->position.x + face_verts[f][i].x,
                block->position.y + face_verts[f][i].y,
                block->position.z + face_verts[f][i].z,
            };
            if (!project_point(ctx, wp, &verts[i])) {
                behind = true;
                break;
            }
            verts[i].u = (i == 1 || i == 2) ? 15.0f : 0.0f;
            verts[i].v = (i == 2 || i == 3) ? 15.0f : 0.0f;
        }
        if (behind) continue;

        RenderQuad q;
        memcpy(q.vertices, verts, sizeof(verts));
        q.texture_id = 0;
        q.color_tint = block_palette_index(block->type);
        renderer_push_quad(ctx, &q);
    }
}

int renderer_draw_chunk(RenderContext *ctx, Block *blocks, int num_blocks)
{
    int before = ctx->n_quads;
    for (int i = 0; i < num_blocks; i++)
        renderer_draw_block(ctx, &blocks[i]);
    return ctx->n_quads - before;
}
