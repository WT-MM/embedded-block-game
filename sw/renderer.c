#include "renderer.h"
#include "world.h"
#include "gpu_transport.h"
#include "voxel_gpu.h"
#include "env_util.h"
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
#define PI_F 3.14159265358979323846f
#define SKY_DAY_LENGTH_SECONDS 180.0f
#define SKY_DOME_DISTANCE 512.0f
#define SKY_GRADIENT_BANDS 24
#define OCCLUSION_TILE_SIZE 4
#define OCCLUSION_TILES_X ((VOXEL_RENDER_WIDTH + OCCLUSION_TILE_SIZE - 1u) / \
                           OCCLUSION_TILE_SIZE)
#define OCCLUSION_TILES_Y ((VOXEL_RENDER_HEIGHT + OCCLUSION_TILE_SIZE - 1u) / \
                           OCCLUSION_TILE_SIZE)
#define OCCLUSION_TILE_COUNT (OCCLUSION_TILES_X * OCCLUSION_TILES_Y)
#define OCCLUSION_DEPTH_EPSILON (2.0f / 32768.0f)

/* Quantization step for the sky-gradient palette computation. Continuous
 * world_time would re-derive every frame; even sub-pixel color drift bumps
 * gpu_transport's hw_sky_epoch, which invalidates the per-band SDRAM reuse
 * cache (see PROJECT_NOTES.md "May 5 sky-band reuse epoch fix"). 1.0 s steps
 * give 180 distinct sky states across a 180 s day cycle, visually smooth
 * while cutting periodic palette/cache invalidations in half versus the old
 * 0.5 s cadence. VOXEL_SKY_PALETTE_STEP_SECONDS can tune this at runtime. */
#define DEFAULT_SKY_PALETTE_TIME_STEP_SECONDS 1.0f
#define MIN_SKY_PALETTE_TIME_STEP_SECONDS 0.1f
#define MAX_SKY_PALETTE_TIME_STEP_SECONDS 10.0f

enum {
    PAL_SKY_HIGH = 25,
    PAL_SKY_MID = 26,
    PAL_SKY_HORIZON = 27,
    PAL_CLOUD = 28,
    PAL_CLOUD_SHADOW = 29,
    PAL_SUN_CORE = 30,
    PAL_SUN_GLOW = 31,
    PAL_MOON = 32,
    PAL_MOON_SHADOW = 33,
    PAL_STAR = 34,
    PAL_GLASS = 35,
    PAL_GLASS_EDGE = 36,
    PAL_GLASS_HIGHLIGHT = 37,
    PAL_LAMP_GLOW = 38,
    PAL_LAMP_FRAME = 39,
    PAL_SKY_GRADIENT_BASE = 40,
};

typedef struct {
    int x, y, z;
    uint8_t type;
    int occupied;
} LookupEntry;

typedef struct {
    LookupEntry *entries;
    int capacity;
    int mask;
} BlockLookup;

typedef struct {
    uint8_t r, g, b;
} RGB24;

typedef struct {
    RGB24 zenith;
    RGB24 high;
    RGB24 mid;
    RGB24 horizon;
    RGB24 cloud;
    RGB24 cloud_shadow;
    RGB24 sun_core;
    RGB24 sun_glow;
    RGB24 moon;
    RGB24 moon_shadow;
    RGB24 star;
} SkyPalette;

static Vec3 sun_direction_for_time(float time_seconds);

static void upload_default_palette(GPUTransport *transport)
{
    static const struct voxel_palette_entry entries[] = {
        {  0, 0x10, 0x10, 0x18 }, /* background */
        {  1, 0x6b, 0xa4, 0x3a }, /* grass top */
        {  2, 0x8b, 0x63, 0x41 }, /* dirt side */
        {  3, 0x6f, 0x57, 0x37 }, /* wood side */
        {  4, 0x7c, 0x7c, 0x7c }, /* stone side */
        {  5, 0xff, 0xff, 0xff }, /* debug white */
        {  6, 0xff, 0x40, 0x40 }, /* debug red */
        {  7, 0x40, 0xa0, 0xff }, /* debug blue */
        {  8, 0xff, 0xd0, 0x40 }, /* debug yellow */
        {  9, 0x5c, 0x86, 0x34 }, /* grass side */
        { 10, 0x6a, 0x4a, 0x2c }, /* grass bottom / dark dirt */
        { 11, 0x9d, 0x7b, 0x4d }, /* wood top */
        { 12, 0x53, 0x38, 0x23 }, /* wood bottom */
        { 13, 0x98, 0x98, 0x98 }, /* stone top */
        { 14, 0x5c, 0x5c, 0x5c }, /* stone bottom */
        { 15, 0xa7, 0x79, 0x52 }, /* dirt top */
        { 16, 0x59, 0x41, 0x2a }, /* dirt bottom */
        { 17, 0x4f, 0x78, 0x2d }, /* grass dark */
        { 18, 0x84, 0xba, 0x57 }, /* grass highlight */
        { 19, 0x6f, 0x4f, 0x32 }, /* dirt dark */
        { 20, 0xaa, 0x81, 0x5a }, /* dirt light */
        { 21, 0x88, 0x6a, 0x44 }, /* wood grain */
        { 22, 0x50, 0x3b, 0x24 }, /* wood bark dark */
        { 23, 0x63, 0x63, 0x63 }, /* stone dark */
        { 24, 0x9a, 0x9a, 0x9a }, /* stone light */
        { 25, 0x78, 0xb4, 0xf0 }, /* sky high */
        { 26, 0xb0, 0xd8, 0xff }, /* sky mid */
        { 27, 0xe2, 0xef, 0xff }, /* sky horizon */
        { 28, 0xf5, 0xfa, 0xff }, /* cloud */
        { 29, 0xcb, 0xd8, 0xec }, /* cloud shadow */
        { 30, 0xff, 0xee, 0xaa }, /* sun core */
        { 31, 0xff, 0xbb, 0x55 }, /* sun glow */
        { 32, 0xe7, 0xeb, 0xf8 }, /* moon */
        { 33, 0x9c, 0xa4, 0xc0 }, /* moon shadow */
        { 34, 0xff, 0xff, 0xff }, /* stars */
        { 35, 0xb8, 0xe4, 0xff }, /* glass body */
        { 36, 0x5e, 0x7c, 0x98 }, /* glass edge / frame */
        { 37, 0xff, 0xff, 0xff }, /* glass highlight */
        { 38, 0xff, 0xd7, 0x79 }, /* lamp glow */
        { 39, 0x6d, 0x53, 0x30 }, /* lamp frame */
        { 40, 0x2a, 0x52, 0x9c }, /* banked water deep */
        { 41, 0x3a, 0x6c, 0xc4 }, /* banked water mid */
        { 42, 0x6f, 0x9d, 0xe4 }, /* banked water highlight */
        { 43, 0x7a, 0x20, 0x10 }, /* lava dark */
        { 44, 0xe8, 0x5c, 0x18 }, /* lava orange */
        { 45, 0xff, 0xd8, 0x5a }, /* lava hot */
        { 46, 0xd8, 0xc0, 0x74 }, /* sand */
        { 47, 0xa8, 0x90, 0x50 }, /* sand dark */
        { 48, 0xb8, 0x4d, 0x3f }, /* brick */
        { 49, 0x6e, 0x2a, 0x27 }, /* brick dark */
        { 50, 0x20, 0x1b, 0x2c }, /* obsidian */
        { 51, 0x43, 0x36, 0x58 }, /* obsidian edge */
        { 52, 0x72, 0x82, 0x91 }, /* clay */
        { 53, 0xed, 0xdc, 0x9c }, /* sand light */
        { 54, 0xc3, 0xac, 0x67 }, /* sand shadow */
        { 55, 0xd0, 0x68, 0x55 }, /* brick light */
        { 56, 0x8b, 0x56, 0x4e }, /* brick mortar */
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (gpu_transport_set_palette(transport, &entries[i]) < 0)
            break;
    }
}

typedef struct {
    const Chunk *chunk;
    const ChunkFace *face;
    float view_z;
} TranslucentFaceRef;

typedef enum {
    OCCLUSION_PASS_DISABLED = 0,
    OCCLUSION_PASS_OPAQUE_WORLD,
} OcclusionPass;

struct RenderContext {
    GPUTransport *transport;
    Camera current_camera;
    Vec3 light_dir;
    float world_daylight;

    float cos_yaw, sin_yaw;
    float cos_pitch, sin_pitch;

    uint8_t *submit_buffer;
    size_t submit_capacity;
    size_t submit_bytes;
    LookupEntry *lookup_entries;
    int lookup_capacity;
    uint16_t palette_cache[256];
    uint8_t palette_valid[256];
    struct voxel_palette_entry palette_pending[256];
    uint8_t palette_dirty[256];
    uint16_t sky_palette_cache[SKY_GRADIENT_BANDS];
    uint8_t sky_palette_valid[SKY_GRADIENT_BANDS];
    struct voxel_sky_palette_entry sky_palette_pending[SKY_GRADIENT_BANDS];
    uint8_t sky_palette_dirty[SKY_GRADIENT_BANDS];
    struct voxel_fog_state fog_cache;
    struct voxel_fog_state fog_pending;
    uint8_t fog_valid;
    uint8_t fog_dirty;
    uint8_t light_palette_key;
    uint8_t light_palette_key_valid;
    uint8_t face_light_flags[NUM_FACES];
    Vec3 last_sun_dir;
    uint8_t last_sun_dir_valid;
    int n_quads;
    uint8_t occlusion_enabled;
    uint8_t occlusion_diag;
    OcclusionPass occlusion_pass;
    uint8_t occlusion_covered[OCCLUSION_TILE_COUNT];
    float occlusion_depth[OCCLUSION_TILE_COUNT];
    uint32_t occlusion_tested;
    uint32_t occlusion_culled;
    uint32_t occlusion_tiles_written;
    uint32_t occlusion_frame_count;

    /* Scratch buffer reused across frames to back-to-front sort translucent
     * faces. Grown on demand; memory only leaves if the scene peaks higher
     * than it has before. */
    TranslucentFaceRef *translucent_scratch;
    int translucent_scratch_capacity;

};

static bool stage_prepared_quad(RenderContext *ctx, RenderQuad quad);
static bool stage_projected_quad_no_clip(RenderContext *ctx,
                                         const RenderQuad *quad);

typedef struct {
    float x, y, z;
    float u, v;
} CameraVertex;

static RGB24 rgb24(uint8_t r, uint8_t g, uint8_t b)
{
    RGB24 color = { r, g, b };
    return color;
}

static RGB24 default_palette_color(uint8_t index)
{
    switch (index) {
    case 0:  return rgb24(0x10, 0x10, 0x18);
    case 1:  return rgb24(0x6b, 0xa4, 0x3a);
    case 2:  return rgb24(0x8b, 0x63, 0x41);
    case 3:  return rgb24(0x6f, 0x57, 0x37);
    case 4:  return rgb24(0x7c, 0x7c, 0x7c);
    case 5:  return rgb24(0xff, 0xff, 0xff);
    case 6:  return rgb24(0xff, 0x40, 0x40);
    case 7:  return rgb24(0x40, 0xa0, 0xff);
    case 8:  return rgb24(0xff, 0xd0, 0x40);
    case 9:  return rgb24(0x5c, 0x86, 0x34);
    case 10: return rgb24(0x6a, 0x4a, 0x2c);
    case 11: return rgb24(0x9d, 0x7b, 0x4d);
    case 12: return rgb24(0x53, 0x38, 0x23);
    case 13: return rgb24(0x98, 0x98, 0x98);
    case 14: return rgb24(0x5c, 0x5c, 0x5c);
    case 15: return rgb24(0xa7, 0x79, 0x52);
    case 16: return rgb24(0x59, 0x41, 0x2a);
    case 17: return rgb24(0x4f, 0x78, 0x2d);
    case 18: return rgb24(0x84, 0xba, 0x57);
    case 19: return rgb24(0x6f, 0x4f, 0x32);
    case 20: return rgb24(0xaa, 0x81, 0x5a);
    case 21: return rgb24(0x88, 0x6a, 0x44);
    case 22: return rgb24(0x50, 0x3b, 0x24);
    case 23: return rgb24(0x63, 0x63, 0x63);
    case 24: return rgb24(0x9a, 0x9a, 0x9a);
    case 25: return rgb24(0x78, 0xb4, 0xf0);
    case 26: return rgb24(0xb0, 0xd8, 0xff);
    case 27: return rgb24(0xe2, 0xef, 0xff);
    case 28: return rgb24(0xf5, 0xfa, 0xff);
    case 29: return rgb24(0xcb, 0xd8, 0xec);
    case 30: return rgb24(0xff, 0xee, 0xaa);
    case 31: return rgb24(0xff, 0xbb, 0x55);
    case 32: return rgb24(0xe7, 0xeb, 0xf8);
    case 33: return rgb24(0x9c, 0xa4, 0xc0);
    case 34: return rgb24(0xff, 0xff, 0xff);
    case 35: return rgb24(0xb8, 0xe4, 0xff);
    case 36: return rgb24(0x5e, 0x7c, 0x98);
    case 37: return rgb24(0xff, 0xff, 0xff);
    case 38: return rgb24(0xff, 0xd7, 0x79);
    case 39: return rgb24(0x6d, 0x53, 0x30);
    case 40: return rgb24(0x2a, 0x52, 0x9c);
    case 41: return rgb24(0x3a, 0x6c, 0xc4);
    case 42: return rgb24(0x6f, 0x9d, 0xe4);
    case 43: return rgb24(0x7a, 0x20, 0x10);
    case 44: return rgb24(0xe8, 0x5c, 0x18);
    case 45: return rgb24(0xff, 0xd8, 0x5a);
    case 46: return rgb24(0xd8, 0xc0, 0x74);
    case 47: return rgb24(0xa8, 0x90, 0x50);
    case 48: return rgb24(0xb8, 0x4d, 0x3f);
    case 49: return rgb24(0x6e, 0x2a, 0x27);
    case 50: return rgb24(0x20, 0x1b, 0x2c);
    case 51: return rgb24(0x43, 0x36, 0x58);
    case 52: return rgb24(0x72, 0x82, 0x91);
    case 53: return rgb24(0xed, 0xdc, 0x9c);
    case 54: return rgb24(0xc3, 0xac, 0x67);
    case 55: return rgb24(0xd0, 0x68, 0x55);
    case 56: return rgb24(0x8b, 0x56, 0x4e);
    default: return rgb24(0x00, 0x00, 0x00);
    }
}

static float clamp01(float value)
{
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static float smoothstepf(float edge0, float edge1, float x)
{
    if (edge0 == edge1)
        return x >= edge1 ? 1.0f : 0.0f;

    x = clamp01((x - edge0) / (edge1 - edge0));
    return x * x * (3.0f - 2.0f * x);
}

static RGB24 lerp_rgb24(RGB24 a, RGB24 b, float t)
{
    RGB24 out;

    t = clamp01(t);
    out.r = (uint8_t)lroundf(a.r + (b.r - a.r) * t);
    out.g = (uint8_t)lroundf(a.g + (b.g - a.g) * t);
    out.b = (uint8_t)lroundf(a.b + (b.b - a.b) * t);
    return out;
}

static RGB24 mix_rgb24(RGB24 base, RGB24 tint, float amount)
{
    return lerp_rgb24(base, tint, amount);
}

static RGB24 scale_rgb24(RGB24 color, float factor)
{
    return rgb24((uint8_t)lroundf(color.r * factor),
                 (uint8_t)lroundf(color.g * factor),
                 (uint8_t)lroundf(color.b * factor));
}

static uint16_t rgb24_to_rgb565(RGB24 color)
{
    return (uint16_t)(((uint16_t)(color.r & 0xF8) << 8) |
                      ((uint16_t)(color.g & 0xFC) << 3) |
                      ((uint16_t)color.b >> 3));
}

static bool renderer_set_palette_rgb(RenderContext *ctx, uint8_t index, RGB24 color)
{
    uint16_t packed = rgb24_to_rgb565(color);

    if (ctx->palette_valid[index] && ctx->palette_cache[index] == packed)
        return true;

    ctx->palette_valid[index] = 1;
    ctx->palette_cache[index] = packed;
    ctx->palette_pending[index] = (struct voxel_palette_entry){
        .index = index,
        .r = color.r,
        .g = color.g,
        .b = color.b,
    };
    ctx->palette_dirty[index] = 1;
    return true;
}

static bool renderer_set_sky_palette_rgb(RenderContext *ctx, uint8_t index, RGB24 color)
{
    uint16_t packed;

    if (!ctx || index >= SKY_GRADIENT_BANDS)
        return false;

    packed = rgb24_to_rgb565(color);
    if (ctx->sky_palette_valid[index] &&
        ctx->sky_palette_cache[index] == packed)
        return true;

    ctx->sky_palette_valid[index] = 1;
    ctx->sky_palette_cache[index] = packed;
    ctx->sky_palette_pending[index] = (struct voxel_sky_palette_entry){
        .index = index,
        .r = color.r,
        .g = color.g,
        .b = color.b,
    };
    ctx->sky_palette_dirty[index] = 1;
    return true;
}

static bool same_fog_state(const struct voxel_fog_state *a,
                           const struct voxel_fog_state *b)
{
    return a->start_dist == b->start_dist &&
           a->end_dist == b->end_dist &&
           a->color_index == b->color_index &&
           a->enabled == b->enabled &&
           a->inv_proj_sq == b->inv_proj_sq;
}

static bool renderer_set_fog_state(RenderContext *ctx,
                                   const struct voxel_fog_state *fog)
{
    if (ctx->fog_valid && same_fog_state(&ctx->fog_cache, fog))
        return true;

    ctx->fog_cache = *fog;
    ctx->fog_pending = *fog;
    ctx->fog_valid = 1;
    ctx->fog_dirty = 1;
    return true;
}

static bool renderer_flush_gpu_state(RenderContext *ctx)
{
    for (int i = 0; i < SKY_GRADIENT_BANDS; i++) {
        if (!ctx->sky_palette_dirty[i])
            continue;
        if (gpu_transport_set_sky_palette(ctx->transport,
                                          &ctx->sky_palette_pending[i]) < 0)
            return false;
        ctx->sky_palette_dirty[i] = 0;
    }

    for (int i = 0; i < 256; i++) {
        if (!ctx->palette_dirty[i])
            continue;
        if (gpu_transport_set_palette(ctx->transport,
                                      &ctx->palette_pending[i]) < 0)
            return false;
        ctx->palette_dirty[i] = 0;
    }

    if (ctx->fog_dirty) {
        if (gpu_transport_set_fog(ctx->transport, &ctx->fog_pending) < 0)
            return false;
        ctx->fog_dirty = 0;
    }

    return true;
}

static void upload_light_palette_banks(RenderContext *ctx, float daylight)
{
    static const float bank_scale_day[4] = { 1.00f, 0.82f, 0.64f, 0.50f };
    static const float bank_scale_night[4] = { 1.00f, 0.34f, 0.20f, 0.10f };
    int daylight_key;

    if (!ctx)
        return;

    daylight = clamp01(daylight);
    daylight_key = (int)lroundf(daylight * 31.0f);
    if (ctx->light_palette_key_valid &&
        ctx->light_palette_key == (uint8_t)daylight_key)
        return;

    daylight = (float)daylight_key / 31.0f;

    for (int bank = 1; bank < 4; bank++) {
        float bank_scale = bank_scale_night[bank] +
                           (bank_scale_day[bank] - bank_scale_night[bank]) *
                           daylight;

        for (int index = 1; index < 64; index++) {
            RGB24 color = scale_rgb24(default_palette_color((uint8_t)index),
                                      bank_scale);

            if (!renderer_set_palette_rgb(ctx,
                                          (uint8_t)(bank * 64 + index),
                                          color))
                return;
        }
    }

    ctx->light_palette_key = (uint8_t)daylight_key;
    ctx->light_palette_key_valid = 1;
}

static Vec3 normalize_vec3(Vec3 v)
{
    float len_sq = v.x * v.x + v.y * v.y + v.z * v.z;

    if (len_sq <= 0.0f)
        return (Vec3){ 0.0f, 0.0f, 1.0f };

    float inv_len = 1.0f / sqrtf(len_sq);
    return (Vec3){ v.x * inv_len, v.y * inv_len, v.z * inv_len };
}

static Vec3 direction_from_azimuth_elevation(float azimuth, float elevation)
{
    float cos_elevation = cosf(elevation);

    return (Vec3){
        cos_elevation * sinf(azimuth),
        sinf(elevation),
        cos_elevation * cosf(azimuth),
    };
}

static const Vec3 face_normals[NUM_FACES] = {
    { 0, 1, 0 },
    { 0, -1, 0 },
    { -1, 0, 0 },
    { 1, 0, 0 },
    { 0, 0, -1 },
    { 0, 0, 1 },
};

static uint8_t light_flags_for_normal(Vec3 light_dir, Vec3 normal)
{
    float ndotl = normal.x * light_dir.x +
                  normal.y * light_dir.y +
                  normal.z * light_dir.z;
    unsigned bank;

    if (ndotl >= 0.65f)
        bank = 0;
    else if (ndotl >= 0.15f)
        bank = 1;
    else if (ndotl >= -0.25f)
        bank = 2;
    else
        bank = 3;

    return QUAD_LIGHT_LEVEL(bank);
}

static void update_face_light_flags(RenderContext *ctx)
{
    for (int face = 0; face < NUM_FACES; face++)
        ctx->face_light_flags[face] =
            light_flags_for_normal(ctx->light_dir, face_normals[face]);
}

static uint8_t light_bank_for_level(uint8_t light_level)
{
    if (light_level >= 12)
        return 0;
    if (light_level >= 8)
        return 1;
    if (light_level >= 4)
        return 2;
    return 3;
}

static uint8_t sky_light_level_for_face(const RenderContext *ctx,
                                        BlockFace face,
                                        uint8_t sky_light)
{
    float ndotl;
    float facing;
    float intensity;
    int level;

    if (!ctx || face < 0 || face >= NUM_FACES || sky_light == 0)
        return 0;

    ndotl = face_normals[face].x * ctx->light_dir.x +
            face_normals[face].y * ctx->light_dir.y +
            face_normals[face].z * ctx->light_dir.z;
    facing = clamp01(0.5f + 0.5f * ndotl);
    intensity = 0.12f + ctx->world_daylight * (0.25f + 0.63f * facing);
    level = (int)lroundf((float)sky_light * clamp01(intensity));
    if (level < 0)
        level = 0;
    if (level > 15)
        level = 15;
    return (uint8_t)level;
}

static uint8_t choose_chunk_face_light_flags(const RenderContext *ctx,
                                             BlockID type, BlockFace face,
                                             uint8_t sky_light,
                                             uint8_t block_light)
{
    uint8_t effective_level = block_light;
    uint8_t sky_level = sky_light_level_for_face(ctx, face, sky_light);

    if (block_is_self_lit(type))
        effective_level = 15;
    if (sky_level > effective_level)
        effective_level = sky_level;

    return QUAD_LIGHT_LEVEL(light_bank_for_level(effective_level));
}

static float daylight_strength_for_sun_direction(Vec3 sun_dir)
{
    return smoothstepf(-0.18f, 0.08f, sun_dir.y);
}

static Vec3 terrain_light_direction_for_sun(Vec3 sun_dir)
{
    if (sun_dir.y < 0.18f)
        sun_dir.y = 0.18f;
    return normalize_vec3(sun_dir);
}

static void update_world_light_from_sun(RenderContext *ctx, Vec3 sun_dir)
{
    if (!ctx)
        return;

    if (ctx->last_sun_dir_valid &&
        ctx->last_sun_dir.x == sun_dir.x &&
        ctx->last_sun_dir.y == sun_dir.y &&
        ctx->last_sun_dir.z == sun_dir.z)
        return;

    ctx->world_daylight = daylight_strength_for_sun_direction(sun_dir);
    ctx->light_dir = terrain_light_direction_for_sun(sun_dir);
    update_face_light_flags(ctx);
    upload_light_palette_banks(ctx, ctx->world_daylight);
    ctx->last_sun_dir = sun_dir;
    ctx->last_sun_dir_valid = 1;
}

static float palette_time_for(float time_seconds)
{
    static int initialized;
    static float step_seconds = DEFAULT_SKY_PALETTE_TIME_STEP_SECONDS;

    if (!initialized) {
        step_seconds =
            env_float_or_default("VOXEL_SKY_PALETTE_STEP_SECONDS",
                                 DEFAULT_SKY_PALETTE_TIME_STEP_SECONDS,
                                 MIN_SKY_PALETTE_TIME_STEP_SECONDS,
                                 MAX_SKY_PALETTE_TIME_STEP_SECONDS);
        initialized = 1;
    }

    return floorf(time_seconds / step_seconds) * step_seconds;
}

static void update_world_light_state(RenderContext *ctx, float time_seconds)
{
    update_world_light_from_sun(ctx,
                                sun_direction_for_time(palette_time_for(time_seconds)));
}

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

static bool camera_quad_outside_view(RenderContext *ctx, const CameraVertex v[4])
{
    float x_slope = (SCREEN_WIDTH * 0.5f) / ctx->current_camera.depth;
    float y_slope = (SCREEN_HEIGHT * 0.5f) / ctx->current_camera.depth;
    bool outside_near = true;
    bool outside_left = true;
    bool outside_right = true;
    bool outside_top = true;
    bool outside_bottom = true;

    /*
     * The side-plane tests (x + x_slope*z < 0, etc.) are valid only for
     * vertices in front of the near plane. For z <= 0, x_slope*z is non-positive,
     * so the test can flip sign and false-positive — a vertex behind the camera
     * with any x looks "to the left of left" by this math, which used to cull
     * any face that had a vertex behind the player. Gate side-plane votes on
     * z >= NEAR_PLANE; a partially-behind face only gets culled if every vertex
     * is behind the near plane.
     */
    for (int i = 0; i < 4; i++) {
        bool z_in_front = (v[i].z >= NEAR_PLANE);
        outside_near &= !z_in_front;
        outside_left &= z_in_front && (v[i].x + x_slope * v[i].z < 0.0f);
        outside_right &= z_in_front && (-v[i].x + x_slope * v[i].z < 0.0f);
        outside_top &= z_in_front && (y_slope * v[i].z - v[i].y < 0.0f);
        outside_bottom &= z_in_front && (v[i].y + y_slope * v[i].z < 0.0f);
    }

    return outside_near || outside_left || outside_right ||
           outside_top || outside_bottom;
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
        if (blocks[i].type == BLOCK_AIR)
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
        lookup->entries[idx].type = (uint8_t)blocks[i].type;
        lookup->entries[idx].occupied = 1;
    }

    return true;
}

static BlockID lookup_block_at(const BlockLookup *lookup, Vec3 pos)
{
    int x = (int)lroundf(pos.x);
    int y = (int)lroundf(pos.y);
    int z = (int)lroundf(pos.z);
    uint32_t idx = hash_grid_coord(x, y, z) & (uint32_t)lookup->mask;

    while (lookup->entries[idx].occupied) {
        if (lookup->entries[idx].x == x &&
            lookup->entries[idx].y == y &&
            lookup->entries[idx].z == z)
            return (BlockID)lookup->entries[idx].type;
        idx = (idx + 1u) & (uint32_t)lookup->mask;
    }

    return BLOCK_AIR;
}

static bool is_face_exposed(const Block *block, BlockFace face,
                            const BlockLookup *lookup)
{
    Vec3 neighbor = {
        block->position.x + face_normals[face].x,
        block->position.y + face_normals[face].y,
        block->position.z + face_normals[face].z,
    };

    return block_face_should_render(block->type, lookup_block_at(lookup, neighbor));
}

/* Pack a float edge coefficient into Q24.8 (multiply by 256, round). */
static inline int32_t to_q24_8(float v)
{
    float scaled = v * 256.0f;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static inline float snap_q24_8(float v)
{
    return (float)to_q24_8(v) / 256.0f;
}

static inline int floor_div_256_i32(int32_t v)
{
    int q = v / 256;
    int r = v % 256;

    return (r && v < 0) ? q - 1 : q;
}

static inline int ceil_div_256_i32(int32_t v)
{
    int q = v / 256;
    int r = v % 256;

    return (r && v > 0) ? q + 1 : q;
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

/* Pack a float depth value into Q1.15 unsigned (clamp to [0,2)). */
static inline uint16_t to_q1_15u(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.99f) v = 1.99f;
    return (uint16_t)(v * 32768.0f + 0.5f);
}

/* Pack a float depth gradient into Q1.15 signed (clamp to [-1,1)). */
static inline int16_t to_q1_15s(float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v >  0.999f) v = 0.999f;
    {
        float scaled = v * 32768.0f;
        return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    }
}

static inline uint16_t to_q8_8u(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 255.996f) v = 255.996f;
    return (uint16_t)(v * 256.0f + 0.5f);
}

static inline uint16_t to_q0_16u(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 0.9999847f) v = 0.9999847f;
    return (uint16_t)(v * 65536.0f + 0.5f);
}

static inline int32_t to_q16_16(float v)
{
    float scaled = v * 65536.0f;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
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

typedef struct {
    int a;
    int b;
    int c;
    float ax;
    float ay;
    float bx;
    float by;
    float det;
    bool valid;
} PlaneBasis;

static PlaneBasis choose_plane_basis(const Vertex2D v[4])
{
    for (int i = 0; i < 4; i++) {
        int a = kAttrTriples[i][0];
        int b = kAttrTriples[i][1];
        int c = kAttrTriples[i][2];
        float ax = v[b].x - v[a].x;
        float ay = v[b].y - v[a].y;
        float bx = v[c].x - v[a].x;
        float by = v[c].y - v[a].y;
        float det = ax * by - ay * bx;

        if (fabsf(det) > 1e-6f)
            return (PlaneBasis){ a, b, c, ax, ay, bx, by, det, true };
    }

    return (PlaneBasis){ 0 };
}

static void solve_attr_with_basis(const PlaneBasis *basis,
                                  float attr_a, float attr_b, float attr_c,
                                  float *ddx, float *ddy)
{
    if (!basis->valid) {
        *ddx = 0.0f;
        *ddy = 0.0f;
        return;
    }

    float av = attr_b - attr_a;
    float bv = attr_c - attr_a;

    *ddx = ( basis->by * av - basis->ay * bv) / basis->det;
    *ddy = (-basis->bx * av + basis->ax * bv) / basis->det;
}

/* Fit a depth plane z(x,y) = z0 + dz_dx*x + dz_dy*y from 3 screen vertices.
 * Stores the plane at the requested raster sample point. */
static void fit_depth_plane(const Vertex2D v[4], const PlaneBasis *basis,
                            float sample_x, float sample_y,
                            uint16_t *z0, int16_t *dz_dx, int16_t *dz_dy)
{
    float ddx = 0.0f;
    float ddy = 0.0f;
    int a = basis->a;

    solve_attr_with_basis(basis, v[basis->a].z, v[basis->b].z, v[basis->c].z,
                          &ddx, &ddy);

    float z_at_origin = v[a].z - ddx * v[a].x - ddy * v[a].y;
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
static void fit_uv_plane(const Vertex2D v[4], const PlaneBasis *basis,
                         float sample_x, float sample_y,
                         struct quad_desc_uv *uv)
{
    float duw_dx = 0.0f, duw_dy = 0.0f;
    float dvw_dx = 0.0f, dvw_dy = 0.0f;
    float diw_dx = 0.0f, diw_dy = 0.0f;
    int a = basis->a;

    solve_attr_with_basis(basis,
                          v[basis->a].u_over_w,
                          v[basis->b].u_over_w,
                          v[basis->c].u_over_w,
                          &duw_dx, &duw_dy);
    solve_attr_with_basis(basis,
                          v[basis->a].v_over_w,
                          v[basis->b].v_over_w,
                          v[basis->c].v_over_w,
                          &dvw_dx, &dvw_dy);
    solve_attr_with_basis(basis,
                          v[basis->a].one_over_w,
                          v[basis->b].one_over_w,
                          v[basis->c].one_over_w,
                          &diw_dx, &diw_dy);

    float uw_origin = v[a].u_over_w   - duw_dx * v[a].x - duw_dy * v[a].y;
    float vw_origin = v[a].v_over_w   - dvw_dx * v[a].x - dvw_dy * v[a].y;
    float iw_origin = v[a].one_over_w - diw_dx * v[a].x - diw_dy * v[a].y;
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

/* Conservative software occlusion:
 * a tile is considered covered only when one opaque, z-tested quad contains
 * all four extreme pixel centers. A later quad is culled only when every tile
 * in its bbox is covered by an occluder whose farthest vertex is still closer
 * than the candidate's nearest vertex. */
static void occlusion_reset(RenderContext *ctx)
{
    memset(ctx->occlusion_covered, 0, sizeof(ctx->occlusion_covered));
    ctx->occlusion_pass = OCCLUSION_PASS_DISABLED;
    ctx->occlusion_tested = 0;
    ctx->occlusion_culled = 0;
    ctx->occlusion_tiles_written = 0;
}

static bool occlusion_quad_eligible(uint8_t flags)
{
    return (flags & QUAD_FLAG_ZTEST) &&
           !(flags & QUAD_FLAG_ALPHA_KEY) &&
           ((flags & QUAD_ALPHA_MASK) == QUAD_ALPHA_OPAQUE);
}

static int occlusion_tile_index(int tx, int ty)
{
    return ty * (int)OCCLUSION_TILES_X + tx;
}

static bool point_inside_convex_quad(const Vertex2D v[4], float px, float py)
{
    bool saw_positive = false;
    bool saw_negative = false;

    for (int i = 0; i < 4; i++) {
        const Vertex2D *a = &v[i];
        const Vertex2D *b = &v[(i + 1) & 3];
        float dx = b->x - a->x;
        float dy = b->y - a->y;
        float cross;

        if (fabsf(dx) < 1e-5f && fabsf(dy) < 1e-5f)
            continue;

        cross = dx * (py - a->y) - dy * (px - a->x);
        if (fabsf(cross) <= 1e-4f)
            return false;
        if (cross > 0.0f)
            saw_positive = true;
        else
            saw_negative = true;

        if (saw_positive && saw_negative)
            return false;
    }

    return true;
}

static float quad_min_depth(const Vertex2D v[4])
{
    float z = v[0].z;

    for (int i = 1; i < 4; i++) {
        if (v[i].z < z)
            z = v[i].z;
    }
    return z;
}

static float quad_max_depth(const Vertex2D v[4])
{
    float z = v[0].z;

    for (int i = 1; i < 4; i++) {
        if (v[i].z > z)
            z = v[i].z;
    }
    return z;
}

static void occlusion_tile_range(int x_min, int y_min, int x_max, int y_max,
                                 int *tx_min, int *ty_min,
                                 int *tx_max, int *ty_max)
{
    *tx_min = x_min / OCCLUSION_TILE_SIZE;
    *ty_min = y_min / OCCLUSION_TILE_SIZE;
    *tx_max = x_max / OCCLUSION_TILE_SIZE;
    *ty_max = y_max / OCCLUSION_TILE_SIZE;

    if (*tx_min < 0) *tx_min = 0;
    if (*ty_min < 0) *ty_min = 0;
    if (*tx_max >= (int)OCCLUSION_TILES_X)
        *tx_max = (int)OCCLUSION_TILES_X - 1;
    if (*ty_max >= (int)OCCLUSION_TILES_Y)
        *ty_max = (int)OCCLUSION_TILES_Y - 1;
}

static bool occlusion_quad_hidden(RenderContext *ctx, const Vertex2D v[4],
                                  int x_min, int y_min, int x_max, int y_max)
{
    int tx_min, ty_min, tx_max, ty_max;
    float nearest_z = quad_min_depth(v);

    occlusion_tile_range(x_min, y_min, x_max, y_max,
                         &tx_min, &ty_min, &tx_max, &ty_max);

    for (int ty = ty_min; ty <= ty_max; ty++) {
        for (int tx = tx_min; tx <= tx_max; tx++) {
            int idx = occlusion_tile_index(tx, ty);

            if (!ctx->occlusion_covered[idx])
                return false;
            if (ctx->occlusion_depth[idx] + OCCLUSION_DEPTH_EPSILON >= nearest_z)
                return false;
        }
    }

    return true;
}

static bool occlusion_quad_covers_tile(const Vertex2D v[4], int tx, int ty)
{
    float x0 = (float)(tx * OCCLUSION_TILE_SIZE) + 0.5f;
    float y0 = (float)(ty * OCCLUSION_TILE_SIZE) + 0.5f;
    float x1 = (float)((tx + 1) * OCCLUSION_TILE_SIZE) - 0.5f;
    float y1 = (float)((ty + 1) * OCCLUSION_TILE_SIZE) - 0.5f;

    if (x1 > SCREEN_WIDTH - 0.5f)
        x1 = SCREEN_WIDTH - 0.5f;
    if (y1 > SCREEN_HEIGHT - 0.5f)
        y1 = SCREEN_HEIGHT - 0.5f;

    return point_inside_convex_quad(v, x0, y0) &&
           point_inside_convex_quad(v, x1, y0) &&
           point_inside_convex_quad(v, x0, y1) &&
           point_inside_convex_quad(v, x1, y1);
}

static void occlusion_record_quad(RenderContext *ctx, const Vertex2D v[4],
                                  int x_min, int y_min, int x_max, int y_max)
{
    int tx_min, ty_min, tx_max, ty_max;
    float farthest_z = quad_max_depth(v);

    occlusion_tile_range(x_min, y_min, x_max, y_max,
                         &tx_min, &ty_min, &tx_max, &ty_max);

    for (int ty = ty_min; ty <= ty_max; ty++) {
        for (int tx = tx_min; tx <= tx_max; tx++) {
            int idx;

            if (!occlusion_quad_covers_tile(v, tx, ty))
                continue;

            idx = occlusion_tile_index(tx, ty);
            if (!ctx->occlusion_covered[idx] ||
                farthest_z < ctx->occlusion_depth[idx]) {
                ctx->occlusion_covered[idx] = 1;
                ctx->occlusion_depth[idx] = farthest_z;
                ctx->occlusion_tiles_written++;
            }
        }
    }
}

static bool projected_quad_fully_inside_viewport(const Vertex2D v[4])
{
    for (int i = 0; i < 4; i++) {
        if (v[i].x < VIEW_X_MIN || v[i].x > VIEW_X_MAX ||
            v[i].y < VIEW_Y_MIN || v[i].y > VIEW_Y_MAX)
            return false;
    }
    return true;
}

static bool stage_projected_quad_no_clip(RenderContext *ctx,
                                         const RenderQuad *quad)
{
    Vertex2D verts[4];
    int count;

    memcpy(verts, quad->vertices, sizeof(verts));
    count = snap_and_compact_polygon(verts, 4);
    if (count < 3)
        return false;

    if (count == 3) {
        RenderQuad tri = {
            .vertices = { verts[0], verts[1], verts[2], verts[2] },
            .texture_id = quad->texture_id,
            .color_tint = quad->color_tint,
            .flags = quad->flags,
        };
        return stage_prepared_quad(ctx, tri);
    }

    RenderQuad unclipped = {
        .vertices = { verts[0], verts[1], verts[2], verts[3] },
        .texture_id = quad->texture_id,
        .color_tint = quad->color_tint,
        .flags = quad->flags,
    };
    return stage_prepared_quad(ctx, unclipped);
}

static bool emit_camera_quad(RenderContext *ctx, const CameraVertex verts_cam[4],
                             uint8_t texture_id, uint8_t extra_flags)
{
    Vertex2D verts[4];
    uint8_t flags = QUAD_FLAG_ZTEST | QUAD_FLAG_TEX | extra_flags;

    for (int i = 0; i < 4; i++) {
        if (!project_camera_vertex(ctx, &verts_cam[i], &verts[i]))
            return false;
    }

    /* Most world quads are already on-screen; avoid the general clipper. */
    if (projected_quad_fully_inside_viewport(verts)) {
        RenderQuad q = {0};

        memcpy(q.vertices, verts, sizeof(verts));
        q.texture_id = texture_id;
        q.flags = flags;
        return stage_projected_quad_no_clip(ctx, &q);
    }

    RenderQuad q = {0};
    memcpy(q.vertices, verts, sizeof(verts));
    q.texture_id = texture_id;
    q.flags = flags;
    return renderer_push_quad(ctx, &q);
}

static bool emit_camera_quad_color(RenderContext *ctx,
                                   const CameraVertex verts_cam[4],
                                   uint8_t color,
                                   uint8_t extra_flags)
{
    Vertex2D verts[4];
    uint8_t flags = QUAD_FLAG_ZTEST | extra_flags;

    for (int i = 0; i < 4; i++) {
        if (!project_camera_vertex(ctx, &verts_cam[i], &verts[i]))
            return false;
    }

    if (projected_quad_fully_inside_viewport(verts)) {
        RenderQuad q = {0};

        memcpy(q.vertices, verts, sizeof(verts));
        q.color_tint = color;
        q.flags = flags;
        return stage_projected_quad_no_clip(ctx, &q);
    }

    RenderQuad q = {0};
    memcpy(q.vertices, verts, sizeof(verts));
    q.color_tint = color;
    q.flags = flags;
    return renderer_push_quad(ctx, &q);
}

static void emit_clipped_face(RenderContext *ctx, const CameraVertex *poly,
                              int count, uint8_t texture_id, uint8_t extra_flags)
{
    if (count < 3)
        return;

    if (count == 3) {
        CameraVertex tri[4] = { poly[0], poly[1], poly[2], poly[2] };
        emit_camera_quad(ctx, tri, texture_id, extra_flags);
        return;
    }

    if (count == 4) {
        CameraVertex quad[4] = { poly[0], poly[1], poly[2], poly[3] };
        emit_camera_quad(ctx, quad, texture_id, extra_flags);
        return;
    }

    for (int i = 1; i + 1 < count; i++) {
        CameraVertex tri[4] = { poly[0], poly[i], poly[i + 1], poly[i + 1] };
        emit_camera_quad(ctx, tri, texture_id, extra_flags);
    }
}

static void emit_clipped_face_color(RenderContext *ctx,
                                    const CameraVertex *poly,
                                    int count,
                                    uint8_t color,
                                    uint8_t extra_flags)
{
    if (count < 3)
        return;

    if (count == 3) {
        CameraVertex tri[4] = { poly[0], poly[1], poly[2], poly[2] };
        emit_camera_quad_color(ctx, tri, color, extra_flags);
        return;
    }

    if (count == 4) {
        CameraVertex quad[4] = { poly[0], poly[1], poly[2], poly[3] };
        emit_camera_quad_color(ctx, quad, color, extra_flags);
        return;
    }

    for (int i = 1; i + 1 < count; i++) {
        CameraVertex tri[4] = { poly[0], poly[i], poly[i + 1], poly[i + 1] };
        emit_camera_quad_color(ctx, tri, color, extra_flags);
    }
}

static uint8_t choose_face_texture_lod(const RenderContext *ctx,
                                       uint8_t base_tile,
                                       const CameraVertex face_cam[4])
{
    (void)ctx;
    float nearest_z = face_cam[0].z;
    int lod = 0;

    for (int i = 1; i < 4; i++) {
        if (face_cam[i].z < nearest_z)
            nearest_z = face_cam[i].z;
    }

    /*
     * Use a conservative forward-distance rule instead of projected-span
     * math. The old span-based selector was more expensive and could choose
     * low-res tiles for nearby but highly oblique faces because one projected
     * axis collapsed. These thresholds push mip usage farther out so nearby
     * surfaces stay crisp and runtime overhead stays tiny.
     */
    if (nearest_z > 44.0f)
        lod = 2;
    else if (nearest_z > 28.0f)
        lod = 1;

    return texture_lod_tile_id(base_tile, lod);
}

static uint8_t choose_face_light_flags(const RenderContext *ctx, BlockFace face)
{
    return ctx->face_light_flags[face];
}

static void merged_face_vertices(Vec3 block_pos, BlockFace face,
                                 int u_size, int v_size,
                                 uint8_t height_eighths, Vec3 out[4])
{
    float u = (float)u_size;
    float v = (float)v_size;
    float x = block_pos.x;
    float y = block_pos.y;
    float z = block_pos.z;
    /* "Top" Y of the face. For full-height blocks (height_eighths == 8)
     * this is exactly y + v, which keeps the existing greedy-merge math
     * untouched. For partial-height water (height_eighths < 8) v_size is
     * guaranteed to be 1 (translucent never merges), so the partial top
     * sits at y + height/8. */
    float top_y = y + (v - 1.0f) + (float)height_eighths * (1.0f / 8.0f);
    float top_y_only = y + (float)height_eighths * (1.0f / 8.0f);

    switch (face) {
    case FACE_TOP:
        out[0] = (Vec3){ x,     top_y_only, z + v };
        out[1] = (Vec3){ x + u, top_y_only, z + v };
        out[2] = (Vec3){ x + u, top_y_only, z     };
        out[3] = (Vec3){ x,     top_y_only, z     };
        break;
    case FACE_BOTTOM:
        out[0] = (Vec3){ x,     y, z     };
        out[1] = (Vec3){ x + u, y, z     };
        out[2] = (Vec3){ x + u, y, z + v };
        out[3] = (Vec3){ x,     y, z + v };
        break;
    case FACE_LEFT:
        out[0] = (Vec3){ x, y,     z     };
        out[1] = (Vec3){ x, y,     z + u };
        out[2] = (Vec3){ x, top_y, z + u };
        out[3] = (Vec3){ x, top_y, z     };
        break;
    case FACE_RIGHT:
        out[0] = (Vec3){ x + 1, y,     z + u };
        out[1] = (Vec3){ x + 1, y,     z     };
        out[2] = (Vec3){ x + 1, top_y, z     };
        out[3] = (Vec3){ x + 1, top_y, z + u };
        break;
    case FACE_FRONT:
        out[0] = (Vec3){ x,     y,     z };
        out[1] = (Vec3){ x + u, y,     z };
        out[2] = (Vec3){ x + u, top_y, z };
        out[3] = (Vec3){ x,     top_y, z };
        break;
    case FACE_BACK:
        out[0] = (Vec3){ x + u, y,     z + 1 };
        out[1] = (Vec3){ x,     y,     z + 1 };
        out[2] = (Vec3){ x,     top_y, z + 1 };
        out[3] = (Vec3){ x + u, top_y, z + 1 };
        break;
    default:
        for (int i = 0; i < 4; i++)
            out[i] = block_pos;
        break;
    }
}

/*
 * Runtime toggle for merged quad emission with QUAD_TEX_REPEAT_UV. Default ON:
 * far chunks were greedily meshed specifically to keep the descriptor stream
 * small enough for steady input/frame pacing. Set BLOCK_GAME_MERGE_FAR_QUADS=0
 * to fall back to unit-quad expansion for visual A/B testing.
 */
static bool merged_emit_repeat_uv_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = env_flag("BLOCK_GAME_MERGE_FAR_QUADS", true) ? 1 : 0;
    return cached != 0;
}

static Vec3 merged_face_sub_origin(Vec3 block_pos, BlockFace face,
                                   int du, int dv)
{
    switch (face) {
    case FACE_TOP:
    case FACE_BOTTOM:
        block_pos.x += (float)du;
        block_pos.z += (float)dv;
        break;
    case FACE_LEFT:
    case FACE_RIGHT:
        block_pos.z += (float)du;
        block_pos.y += (float)dv;
        break;
    case FACE_FRONT:
    case FACE_BACK:
        block_pos.x += (float)du;
        block_pos.y += (float)dv;
        break;
    default:
        break;
    }
    return block_pos;
}

static void emit_merged_block_face_lit(RenderContext *ctx, BlockID type,
                                       Vec3 block_pos, BlockFace face,
                                       int u_size, int v_size,
                                       uint8_t height_eighths,
                                       uint8_t light_flags);

static void expand_merged_face_to_unit_quads(RenderContext *ctx, BlockID type,
                                             Vec3 block_pos, BlockFace face,
                                             int u_size, int v_size,
                                             uint8_t light_flags)
{
    for (int dv = 0; dv < v_size; dv++) {
        for (int du = 0; du < u_size; du++) {
            Vec3 unit = merged_face_sub_origin(block_pos, face, du, dv);
            emit_merged_block_face_lit(ctx, type, unit, face,
                                       1, 1, 8, light_flags);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
                return;
        }
    }
}

static void emit_merged_block_face_lit(RenderContext *ctx, BlockID type,
                                       Vec3 block_pos, BlockFace face,
                                       int u_size, int v_size,
                                       uint8_t height_eighths,
                                       uint8_t light_flags)
{
    static const float tile_span = 16.0f;
    Vec3 face_world[4];
    CameraVertex face_cam[4];
    uint8_t face_flags;
    uint8_t texture_id;
    uint8_t base_tile;

    if (type == BLOCK_AIR)
        return;
    if (face < 0 || face >= NUM_FACES)
        return;

    /*
     * A/B fallback: decomposing a merged run avoids QUAD_TEX_REPEAT_UV entirely,
     * but it is intentionally not the default because it can multiply the far
     * terrain descriptor count and make input pacing noticeably worse.
     */
    if ((u_size > 1 || v_size > 1) && !merged_emit_repeat_uv_enabled()) {
        /*
         * Unit emits below re-run is_face_visible() with their own centers,
         * so we don't pre-cull the anchor here: a merged run can straddle a
         * face whose center is behind the camera while individual cells are
         * not, and vice versa. The per-unit check is the same one used in
         * the non-merged path.
         */
        expand_merged_face_to_unit_quads(ctx, type, block_pos, face,
                                         u_size, v_size, light_flags);
        return;
    }

    if (!is_face_visible(block_pos, face_normals[face],
                         ctx->current_camera.position))
        return;

    merged_face_vertices(block_pos, face, u_size, v_size, height_eighths, face_world);
    for (int i = 0; i < 4; i++) {
        world_to_camera(ctx, face_world[i], &face_cam[i]);
        face_cam[i].u = (i == 1 || i == 2) ? tile_span * (float)u_size : 0.0f;
        /*
         * Atlas row 0 sits at the top of each tile image, and the side faces'
         * vertex 0 is world-bottom. Invert V so world-top maps to texture-top.
         */
        face_cam[i].v = (i == 2 || i == 3) ? 0.0f : tile_span * (float)v_size;
    }
    if (camera_quad_outside_view(ctx, face_cam))
        return;

    base_tile = block_face_texture_id(type, face);
    texture_id = choose_face_texture_lod(ctx, base_tile, face_cam);
    if (u_size > 1 || v_size > 1)
        texture_id |= QUAD_TEX_REPEAT_UV;
    face_flags = light_flags | QUAD_FLAG_FOG;
    /*
     * Translucent blocks (glass) ride the hardware alpha-blend path. 50% src
     * / 50% dst gives an obviously see-through look while still leaving the
     * glass texture itself clearly readable. We keep ZTEST on so the glass
     * occludes things behind it at the correct depth; because chunks are
     * drawn nearest-first the destination pixel will usually already hold an
     * opaque fragment that the blend can mix against.
     */
    if (block_is_translucent(type))
        face_flags |= QUAD_ALPHA_50;
    /*
     * Cutout textures (leaves, flowers, doors, torches) carry palette index 0
     * for their holes. Without ALPHA_KEY those texels render as the palette-0
     * background color and look like solid sky-tinted blobs instead of holes.
     */
    if (block_is_alpha_keyed(type))
        face_flags |= QUAD_FLAG_ALPHA_KEY;

    CameraVertex clipped[6];
    int clipped_count = clip_face_to_near_plane(face_cam, clipped);
    emit_clipped_face(ctx, clipped, clipped_count, texture_id, face_flags);
}

static void cross_face_vertices(Vec3 block_pos, uint8_t face,
                                Vec3 out[4])
{
    const float inset = 0.08f;
    float x = block_pos.x;
    float y = block_pos.y;
    float z = block_pos.z;

    if (face == CHUNK_FACE_CROSS_B) {
        out[0] = (Vec3){ x + 1.0f - inset, y,        z + inset };
        out[1] = (Vec3){ x + inset,        y,        z + 1.0f - inset };
        out[2] = (Vec3){ x + inset,        y + 1.0f, z + 1.0f - inset };
        out[3] = (Vec3){ x + 1.0f - inset, y + 1.0f, z + inset };
        return;
    }

    out[0] = (Vec3){ x + inset,        y,        z + inset };
    out[1] = (Vec3){ x + 1.0f - inset, y,        z + 1.0f - inset };
    out[2] = (Vec3){ x + 1.0f - inset, y + 1.0f, z + 1.0f - inset };
    out[3] = (Vec3){ x + inset,        y + 1.0f, z + inset };
}

static void emit_cross_block_face_lit(RenderContext *ctx, BlockID type,
                                      Vec3 block_pos, uint8_t face,
                                      uint8_t light_flags)
{
    static const float tile_span = 16.0f;
    Vec3 face_world[4];
    CameraVertex face_cam[4];
    uint8_t texture_id;
    uint8_t face_flags = light_flags | QUAD_FLAG_FOG | QUAD_FLAG_ALPHA_KEY;

    if (type == BLOCK_AIR)
        return;

    cross_face_vertices(block_pos, face, face_world);
    for (int i = 0; i < 4; i++) {
        world_to_camera(ctx, face_world[i], &face_cam[i]);
        face_cam[i].u = (i == 1 || i == 2) ? tile_span : 0.0f;
        face_cam[i].v = (i == 2 || i == 3) ? 0.0f : tile_span;
    }
    if (camera_quad_outside_view(ctx, face_cam))
        return;

    texture_id = choose_face_texture_lod(ctx,
                                         block_face_texture_id(type, FACE_FRONT),
                                         face_cam);

    CameraVertex clipped[6];
    int clipped_count = clip_face_to_near_plane(face_cam, clipped);
    emit_clipped_face(ctx, clipped, clipped_count, texture_id, face_flags);
}

static void emit_model_quad_lit(RenderContext *ctx,
                                Vec3 face_world[4], uint8_t texture_tile,
                                uint8_t light_flags);
static void emit_model_quad_lit_rotated(RenderContext *ctx,
                                        Vec3 face_world[4],
                                        uint8_t texture_tile,
                                        uint8_t light_flags,
                                        int uv_rotation);

static void flat_face_vertices(Vec3 block_pos, Vec3 out[4])
{
    const float inset = 0.05f;
    const float y = block_pos.y + 0.025f;

    out[0] = (Vec3){ block_pos.x + inset,        y,
                     block_pos.z + inset };
    out[1] = (Vec3){ block_pos.x + 1.0f - inset, y,
                     block_pos.z + inset };
    out[2] = (Vec3){ block_pos.x + 1.0f - inset, y,
                     block_pos.z + 1.0f - inset };
    out[3] = (Vec3){ block_pos.x + inset,        y,
                     block_pos.z + 1.0f - inset };
}

enum {
    REDSTONE_WIRE_N = 1u << 0,
    REDSTONE_WIRE_E = 1u << 1,
    REDSTONE_WIRE_S = 1u << 2,
    REDSTONE_WIRE_W = 1u << 3,
};

static bool render_block_is_redstone_wire(BlockID id)
{
    return id == BLOCK_REDSTONE_WIRE_UNCONNECTED ||
           id == BLOCK_REDSTONE_WIRE_OFF ||
           id == BLOCK_REDSTONE_WIRE_ON;
}

static bool render_block_is_redstone_component(BlockID id)
{
    return render_block_is_redstone_wire(id) ||
           id == BLOCK_REDSTONE_TORCH_OFF ||
           id == BLOCK_REDSTONE_TORCH_ON ||
           id == BLOCK_REPEATER_OFF ||
           id == BLOCK_REPEATER_ON ||
           id == BLOCK_LAMP_OFF ||
           id == BLOCK_LAMP ||
           id == BLOCK_REDSTONE_BLOCK ||
           id == BLOCK_BUTTON;
}

static bool render_redstone_wire_connects_to(BlockID id)
{
    if (render_block_is_redstone_component(id))
        return true;
    return id != BLOCK_AIR &&
           block_render_model(id) == BLOCK_RENDER_CUBE &&
           !block_is_passable(id);
}

static uint8_t render_redstone_wire_mask(const VoxelWorld *world,
                                         int wx,
                                         int wy,
                                         int wz)
{
    uint8_t mask = 0;

    if (render_redstone_wire_connects_to(world_get_block(world, wx, wy, wz - 1)))
        mask |= REDSTONE_WIRE_N;
    if (render_redstone_wire_connects_to(world_get_block(world, wx + 1, wy, wz)))
        mask |= REDSTONE_WIRE_E;
    if (render_redstone_wire_connects_to(world_get_block(world, wx, wy, wz + 1)))
        mask |= REDSTONE_WIRE_S;
    if (render_redstone_wire_connects_to(world_get_block(world, wx - 1, wy, wz)))
        mask |= REDSTONE_WIRE_W;
    return mask;
}

static int redstone_mask_bit_count(uint8_t mask)
{
    int count = 0;

    for (int bit = 0; bit < 4; bit++) {
        if (mask & (1u << bit))
            count++;
    }
    return count;
}

static uint8_t redstone_wire_texture_for_mask(BlockID type,
                                              uint8_t mask,
                                              int *uv_rotation_out)
{
    bool powered = type == BLOCK_REDSTONE_WIRE_ON;
    int count = redstone_mask_bit_count(mask);

    if (uv_rotation_out)
        *uv_rotation_out = 0;

    if (count == 0)
        return powered ? TEX_TILE_REDSTONE_WIRE_DOT_ON :
                         TEX_TILE_REDSTONE_WIRE_UNCONNECTED;

    if (count == 1 ||
        mask == (REDSTONE_WIRE_E | REDSTONE_WIRE_W) ||
        mask == (REDSTONE_WIRE_N | REDSTONE_WIRE_S)) {
        if ((mask & (REDSTONE_WIRE_N | REDSTONE_WIRE_S)) &&
            !(mask & (REDSTONE_WIRE_E | REDSTONE_WIRE_W)) &&
            uv_rotation_out)
            *uv_rotation_out = 1;
        return powered ? TEX_TILE_REDSTONE_WIRE_ON :
                         TEX_TILE_REDSTONE_WIRE_OFF;
    }

    if (count == 2) {
        if (uv_rotation_out) {
            if (mask == (REDSTONE_WIRE_E | REDSTONE_WIRE_S))
                *uv_rotation_out = 1;
            else if (mask == (REDSTONE_WIRE_S | REDSTONE_WIRE_W))
                *uv_rotation_out = 2;
            else if (mask == (REDSTONE_WIRE_W | REDSTONE_WIRE_N))
                *uv_rotation_out = 3;
        }
        return powered ? TEX_TILE_REDSTONE_WIRE_CORNER_ON :
                         TEX_TILE_REDSTONE_WIRE_CORNER_OFF;
    }

    if (count == 3) {
        if (uv_rotation_out) {
            uint8_t missing = (uint8_t)(~mask & 0x0f);

            if (missing & REDSTONE_WIRE_W)
                *uv_rotation_out = 1;
            else if (missing & REDSTONE_WIRE_N)
                *uv_rotation_out = 2;
            else if (missing & REDSTONE_WIRE_E)
                *uv_rotation_out = 3;
        }
        return powered ? TEX_TILE_REDSTONE_WIRE_T_ON :
                         TEX_TILE_REDSTONE_WIRE_T_OFF;
    }

    return powered ? TEX_TILE_REDSTONE_WIRE_CROSS_ON :
                     TEX_TILE_REDSTONE_WIRE_CROSS_OFF;
}

static void emit_flat_block_face_lit(RenderContext *ctx, BlockID type,
                                     const VoxelWorld *world,
                                     Vec3 block_pos,
                                     int wx,
                                     int wy,
                                     int wz,
                                     uint8_t light_flags)
{
    Vec3 face_world[4];
    uint8_t texture_tile;
    int uv_rotation = 0;

    if (type == BLOCK_AIR)
        return;

    flat_face_vertices(block_pos, face_world);
    texture_tile = block_face_texture_id(type, FACE_TOP);
    if (render_block_is_redstone_wire(type)) {
        uint8_t mask = render_redstone_wire_mask(world, wx, wy, wz);

        texture_tile = redstone_wire_texture_for_mask(type, mask,
                                                      &uv_rotation);
    }

    emit_model_quad_lit_rotated(ctx, face_world, texture_tile, light_flags,
                                uv_rotation);
}

static void torch_face_vertices(Vec3 block_pos, uint8_t face,
                                Vec3 out[4])
{
    const float half = 0.24f;
    const float top = 0.86f;
    float cx = block_pos.x + 0.5f;
    float y = block_pos.y;
    float cz = block_pos.z + 0.5f;

    if (face == CHUNK_FACE_CROSS_B) {
        out[0] = (Vec3){ cx + half, y,       cz - half };
        out[1] = (Vec3){ cx - half, y,       cz + half };
        out[2] = (Vec3){ cx - half, y + top, cz + half };
        out[3] = (Vec3){ cx + half, y + top, cz - half };
        return;
    }

    out[0] = (Vec3){ cx - half, y,       cz - half };
    out[1] = (Vec3){ cx + half, y,       cz + half };
    out[2] = (Vec3){ cx + half, y + top, cz + half };
    out[3] = (Vec3){ cx - half, y + top, cz - half };
}

static void door_slab_planes(BlockID type,
                             bool *span_x_out,
                             float *outer_plane_out,
                             float *inner_plane_out)
{
    const float thickness = 0.10f;
    const float near_edge = 0.05f;
    const float far_edge = 0.95f;
    BlockDoorFacing facing = block_door_facing(type);
    bool open = block_is_door_open(type);
    bool span_x = true;
    float plane = near_edge;

    if (!open) {
        switch (facing) {
        case BLOCK_DOOR_FACING_EAST:
            span_x = false;
            plane = far_edge;
            break;
        case BLOCK_DOOR_FACING_SOUTH:
            span_x = true;
            plane = far_edge;
            break;
        case BLOCK_DOOR_FACING_WEST:
            span_x = false;
            plane = near_edge;
            break;
        case BLOCK_DOOR_FACING_NORTH:
        default:
            span_x = true;
            plane = near_edge;
            break;
        }
    } else {
        switch (facing) {
        case BLOCK_DOOR_FACING_EAST:
            span_x = true;
            plane = near_edge;
            break;
        case BLOCK_DOOR_FACING_SOUTH:
            span_x = false;
            plane = far_edge;
            break;
        case BLOCK_DOOR_FACING_WEST:
            span_x = true;
            plane = far_edge;
            break;
        case BLOCK_DOOR_FACING_NORTH:
        default:
            span_x = false;
            plane = near_edge;
            break;
        }
    }

    if (span_x_out)
        *span_x_out = span_x;
    if (outer_plane_out)
        *outer_plane_out = plane;
    if (inner_plane_out)
        *inner_plane_out = plane + (plane < 0.5f ? thickness : -thickness);
}

static void door_face_vertices(Vec3 block_pos, BlockID type, uint8_t face,
                               Vec3 out[4])
{
    bool span_x;
    float outer_plane;
    float inner_plane;
    float plane;

    door_slab_planes(type, &span_x, &outer_plane, &inner_plane);
    plane = face == CHUNK_FACE_CROSS_B ? inner_plane : outer_plane;

    if (span_x) {
        float z = block_pos.z + plane;

        out[0] = (Vec3){ block_pos.x,        block_pos.y,        z };
        out[1] = (Vec3){ block_pos.x + 1.0f, block_pos.y,        z };
        out[2] = (Vec3){ block_pos.x + 1.0f, block_pos.y + 1.0f, z };
        out[3] = (Vec3){ block_pos.x,        block_pos.y + 1.0f, z };
        return;
    }

    {
        float x = block_pos.x + plane;

        out[0] = (Vec3){ x, block_pos.y,        block_pos.z + 1.0f };
        out[1] = (Vec3){ x, block_pos.y,        block_pos.z };
        out[2] = (Vec3){ x, block_pos.y + 1.0f, block_pos.z };
        out[3] = (Vec3){ x, block_pos.y + 1.0f, block_pos.z + 1.0f };
    }
}

static void door_edge_vertices(Vec3 block_pos, BlockID type, int edge,
                               Vec3 out[4])
{
    bool span_x;
    float outer_plane;
    float inner_plane;
    float min_plane;
    float max_plane;
    float x0 = block_pos.x;
    float x1 = block_pos.x + 1.0f;
    float y0 = block_pos.y;
    float y1 = block_pos.y + 1.0f;
    float z0 = block_pos.z;
    float z1 = block_pos.z + 1.0f;

    door_slab_planes(type, &span_x, &outer_plane, &inner_plane);
    min_plane = outer_plane < inner_plane ? outer_plane : inner_plane;
    max_plane = outer_plane < inner_plane ? inner_plane : outer_plane;

    if (span_x) {
        float za = block_pos.z + min_plane;
        float zb = block_pos.z + max_plane;

        switch (edge) {
        case 0:
            out[0] = (Vec3){ x0, y0, za };
            out[1] = (Vec3){ x0, y0, zb };
            out[2] = (Vec3){ x0, y1, zb };
            out[3] = (Vec3){ x0, y1, za };
            return;
        case 1:
            out[0] = (Vec3){ x1, y0, zb };
            out[1] = (Vec3){ x1, y0, za };
            out[2] = (Vec3){ x1, y1, za };
            out[3] = (Vec3){ x1, y1, zb };
            return;
        case 2:
            out[0] = (Vec3){ x0, y0, za };
            out[1] = (Vec3){ x1, y0, za };
            out[2] = (Vec3){ x1, y0, zb };
            out[3] = (Vec3){ x0, y0, zb };
            return;
        default:
            out[0] = (Vec3){ x0, y1, zb };
            out[1] = (Vec3){ x1, y1, zb };
            out[2] = (Vec3){ x1, y1, za };
            out[3] = (Vec3){ x0, y1, za };
            return;
        }
    }

    {
        float xa = block_pos.x + min_plane;
        float xb = block_pos.x + max_plane;

        switch (edge) {
        case 0:
            out[0] = (Vec3){ xa, y0, z0 };
            out[1] = (Vec3){ xb, y0, z0 };
            out[2] = (Vec3){ xb, y1, z0 };
            out[3] = (Vec3){ xa, y1, z0 };
            return;
        case 1:
            out[0] = (Vec3){ xb, y0, z1 };
            out[1] = (Vec3){ xa, y0, z1 };
            out[2] = (Vec3){ xa, y1, z1 };
            out[3] = (Vec3){ xb, y1, z1 };
            return;
        case 2:
            out[0] = (Vec3){ xa, y0, z0 };
            out[1] = (Vec3){ xb, y0, z0 };
            out[2] = (Vec3){ xb, y0, z1 };
            out[3] = (Vec3){ xa, y0, z1 };
            return;
        default:
            out[0] = (Vec3){ xa, y1, z1 };
            out[1] = (Vec3){ xb, y1, z1 };
            out[2] = (Vec3){ xb, y1, z0 };
            out[3] = (Vec3){ xa, y1, z0 };
            return;
        }
    }
}

static void emit_model_quad_lit(RenderContext *ctx,
                                Vec3 face_world[4], uint8_t texture_tile,
                                uint8_t light_flags)
{
    emit_model_quad_lit_rotated(ctx, face_world, texture_tile, light_flags, 0);
}

static void emit_model_quad_lit_rotated(RenderContext *ctx,
                                        Vec3 face_world[4],
                                        uint8_t texture_tile,
                                        uint8_t light_flags,
                                        int uv_rotation)
{
    static const float tile_span = 16.0f;
    static const float base_uv[4][2] = {
        { 0.0f,      tile_span },
        { tile_span, tile_span },
        { tile_span, 0.0f      },
        { 0.0f,      0.0f      },
    };
    CameraVertex face_cam[4];
    uint8_t texture_id;
    uint8_t face_flags = light_flags | QUAD_FLAG_FOG | QUAD_FLAG_ALPHA_KEY;

    uv_rotation &= 3;
    for (int i = 0; i < 4; i++) {
        int uv_index = (i + uv_rotation) & 3;

        world_to_camera(ctx, face_world[i], &face_cam[i]);
        face_cam[i].u = base_uv[uv_index][0];
        face_cam[i].v = base_uv[uv_index][1];
    }
    if (camera_quad_outside_view(ctx, face_cam))
        return;

    texture_id = choose_face_texture_lod(ctx, texture_tile, face_cam);

    CameraVertex clipped[6];
    int clipped_count = clip_face_to_near_plane(face_cam, clipped);
    emit_clipped_face(ctx, clipped, clipped_count, texture_id, face_flags);
}

static void emit_torch_block_face_lit(RenderContext *ctx, BlockID type,
                                      Vec3 block_pos, uint8_t face,
                                      uint8_t light_flags)
{
    Vec3 face_world[4];

    if (type == BLOCK_AIR)
        return;

    torch_face_vertices(block_pos, face, face_world);
    emit_model_quad_lit(ctx, face_world,
                        block_face_texture_id(type, FACE_FRONT),
                        light_flags);
}

static void emit_door_block_face_lit(RenderContext *ctx, BlockID type,
                                     Vec3 block_pos, uint8_t face,
                                     uint8_t light_flags)
{
    Vec3 face_world[4];

    if (!block_is_door(type))
        return;

    door_face_vertices(block_pos, type, face, face_world);
    emit_model_quad_lit(ctx, face_world,
                        block_face_texture_id(type, FACE_FRONT),
                        light_flags);

    if (face == CHUNK_FACE_CROSS_A) {
        for (int edge = 0; edge < 4; edge++) {
            door_edge_vertices(block_pos, type, edge, face_world);
            emit_model_quad_lit(ctx, face_world, TEX_TILE_WOOD_PLANK,
                                light_flags);
        }
    }
}

static void emit_merged_block_face(RenderContext *ctx, BlockID type,
                                   Vec3 block_pos, BlockFace face,
                                   int u_size, int v_size)
{
    uint8_t light_flags = block_is_self_lit(type)
                              ? QUAD_LIGHT_LEVEL(0)
                              : choose_face_light_flags(ctx, face);

    emit_merged_block_face_lit(ctx, type, block_pos, face, u_size, v_size,
                               8, light_flags);
}

static void emit_block_face(RenderContext *ctx, BlockID type,
                            Vec3 block_pos, BlockFace face)
{
    emit_merged_block_face(ctx, type, block_pos, face, 1, 1);
}

typedef struct {
    int unlock_stage;
    float x0;
    float y0;
    float x1;
    float y1;
} BreakCrackSegment;

static const BreakCrackSegment BREAK_CRACK_SEGMENTS[] = {
    { 0,  7.5f,  7.5f,  7.5f,  4.8f },
    { 1,  7.5f,  5.4f,  4.8f,  4.0f },
    { 2,  7.3f,  6.0f, 10.5f,  4.2f },
    { 3,  7.5f,  7.2f,  5.3f, 10.4f },
    { 4,  8.0f,  7.5f, 11.6f,  9.7f },
    { 5,  5.6f, 10.0f,  3.4f, 13.2f },
    { 6, 10.8f,  4.5f, 13.5f,  2.2f },
    { 6, 10.9f,  9.4f, 13.7f, 12.2f },
    { 7,  4.9f,  4.2f,  2.2f,  2.0f },
    { 7,  7.5f,  4.8f,  7.0f,  1.7f },
    { 8, 13.2f, 12.0f, 15.0f, 14.5f },
    { 8,  3.5f, 13.0f,  1.2f, 15.0f },
    { 9,  2.5f,  2.4f,  0.4f,  0.9f },
    { 9, 13.0f,  2.6f, 15.0f,  0.8f },
};

static Vec3 vec3_add(Vec3 a, Vec3 b)
{
    return (Vec3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static Vec3 vec3_sub(Vec3 a, Vec3 b)
{
    return (Vec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static Vec3 vec3_scale(Vec3 v, float scale)
{
    return (Vec3){ v.x * scale, v.y * scale, v.z * scale };
}

static Vec3 break_face_point_from_uv(const Vec3 corners[4], float u, float v)
{
    Vec3 du = vec3_sub(corners[1], corners[0]);
    Vec3 dv = vec3_sub(corners[3], corners[0]);
    Vec3 p = corners[0];

    p = vec3_add(p, vec3_scale(du, u / 16.0f));
    p = vec3_add(p, vec3_scale(dv, (16.0f - v) / 16.0f));
    return p;
}

static void emit_break_crack_segment(RenderContext *ctx,
                                     const Vec3 corners[4],
                                     const BreakCrackSegment *segment,
                                     float width,
                                     uint8_t color,
                                     uint8_t alpha)
{
    float dx = segment->x1 - segment->x0;
    float dy = segment->y1 - segment->y0;
    float len = sqrtf(dx * dx + dy * dy);
    float px;
    float py;
    Vec3 strip_world[4];
    CameraVertex strip_cam[4];
    CameraVertex clipped[6];

    if (len <= 0.0001f)
        return;

    px = -dy / len * width;
    py = dx / len * width;

    strip_world[0] = break_face_point_from_uv(corners,
                                              segment->x0 + px,
                                              segment->y0 + py);
    strip_world[1] = break_face_point_from_uv(corners,
                                              segment->x1 + px,
                                              segment->y1 + py);
    strip_world[2] = break_face_point_from_uv(corners,
                                              segment->x1 - px,
                                              segment->y1 - py);
    strip_world[3] = break_face_point_from_uv(corners,
                                              segment->x0 - px,
                                              segment->y0 - py);

    for (int i = 0; i < 4; i++)
        world_to_camera(ctx, strip_world[i], &strip_cam[i]);
    if (camera_quad_outside_view(ctx, strip_cam))
        return;

    int clipped_count = clip_face_to_near_plane(strip_cam, clipped);
    emit_clipped_face_color(ctx, clipped, clipped_count, color, alpha);
}

int renderer_draw_block_break_overlay(RenderContext *ctx,
                                      int block_x, int block_y, int block_z,
                                      float progress)
{
    const float face_epsilon = 0.004f;
    int before;
    int stage;
    uint8_t color;
    uint8_t alpha;
    float width;
    Vec3 block_pos;

    if (!ctx)
        return 0;

    progress = clamp01(progress);
    if (progress <= 0.0f)
        return 0;

    before = ctx->n_quads;
    stage = (int)floorf(progress * 10.0f);
    if (stage < 0)
        stage = 0;
    if (stage > 9)
        stage = 9;
    color = 14;
    alpha = progress < 0.33f ? QUAD_ALPHA_25 :
            (progress < 0.66f ? QUAD_ALPHA_50 : QUAD_ALPHA_75);
    width = 0.20f + progress * 0.18f;
    block_pos = (Vec3){ (float)block_x, (float)block_y, (float)block_z };

    for (int face = 0; face < NUM_FACES; face++) {
        Vec3 corners[4];
        Vec3 normal = face_normals[face];

        if (!is_face_visible(block_pos, normal, ctx->current_camera.position))
            continue;

        merged_face_vertices(block_pos, (BlockFace)face, 1, 1, 8, corners);
        for (int i = 0; i < 4; i++) {
            corners[i].x += normal.x * face_epsilon;
            corners[i].y += normal.y * face_epsilon;
            corners[i].z += normal.z * face_epsilon;
        }

        for (size_t i = 0;
             i < sizeof(BREAK_CRACK_SEGMENTS) / sizeof(BREAK_CRACK_SEGMENTS[0]);
             i++) {
            if (BREAK_CRACK_SEGMENTS[i].unlock_stage > stage)
                continue;
            emit_break_crack_segment(ctx, corners, &BREAK_CRACK_SEGMENTS[i],
                                     width, color, alpha);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
                return ctx->n_quads - before;
        }
    }

    return ctx->n_quads - before;
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

static float chunk_distance_sq_to_camera(RenderContext *ctx, const Chunk *chunk);

static bool ensure_translucent_scratch(RenderContext *ctx, int needed)
{
    int cap;
    TranslucentFaceRef *buf;

    if (needed <= ctx->translucent_scratch_capacity)
        return true;

    cap = ctx->translucent_scratch_capacity ? ctx->translucent_scratch_capacity : 64;
    while (cap < needed)
        cap *= 2;

    buf = realloc(ctx->translucent_scratch, (size_t)cap * sizeof(*buf));
    if (!buf)
        return false;

    ctx->translucent_scratch = buf;
    ctx->translucent_scratch_capacity = cap;
    return true;
}

static int compare_translucent_back_to_front(const void *a, const void *b)
{
    const TranslucentFaceRef *ra = a;
    const TranslucentFaceRef *rb = b;

    /* Larger view_z (farther) comes first. */
    if (ra->view_z > rb->view_z) return -1;
    if (ra->view_z < rb->view_z) return 1;
    return 0;
}

static bool chunk_within_render_distance(RenderContext *ctx,
                                         const VoxelWorld *world,
                                         const Chunk *chunk)
{
    float max_distance;
    float max_distance_sq;

    if (world->render_distance_chunks <= 0)
        return true;

    max_distance = (float)(world->render_distance_chunks * WORLD_CHUNK_SIZE);
    max_distance_sq = max_distance * max_distance;
    return chunk_distance_sq_to_camera(ctx, chunk) <= max_distance_sq;
}

static void configure_world_fog(RenderContext *ctx, const VoxelWorld *world)
{
    struct voxel_fog_state fog = {0};

    if (!ctx || !world || world->render_distance_chunks <= 0) {
        if (ctx)
            renderer_set_fog_state(ctx, &fog);
        return;
    }

    float end_distance = (float)(world->render_distance_chunks * WORLD_CHUNK_SIZE);
    float fade_span = (float)WORLD_CHUNK_SIZE;
    float start_distance;
    float inv_proj_sq;

    start_distance = end_distance - fade_span;
    if (start_distance < NEAR_PLANE)
        start_distance = NEAR_PLANE;

    inv_proj_sq = 1.0f / (ctx->current_camera.depth * ctx->current_camera.depth);

    fog.start_dist = to_q8_8u(start_distance);
    fog.end_dist = to_q8_8u(end_distance);
    fog.color_index = PAL_SKY_HORIZON;
    fog.enabled = fog.end_dist > fog.start_dist;
    fog.inv_proj_sq = to_q0_16u(inv_proj_sq);
    renderer_set_fog_state(ctx, &fog);
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
    /* Cached at the start of draw_world so the entire frame iterates a
     * stable mesh pointer even if a future worker thread publishes a new
     * one mid-draw. Lifetime guarantee: with single-slot retired_mesh
     * and at-most-one publish per chunk per frame, the pointer survives
     * until the next frame's drain sweep. */
    const ChunkMesh *mesh;
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

    ctx->submit_capacity = MAX_QUADS_IN_FLIGHT *
                            (sizeof(struct quad_desc) + sizeof(struct quad_desc_uv));
    ctx->submit_buffer = malloc(ctx->submit_capacity);
    ctx->lookup_entries = NULL;
    ctx->lookup_capacity = 0;
    ctx->translucent_scratch = NULL;
    ctx->translucent_scratch_capacity = 0;
    ctx->occlusion_enabled = env_flag("VOXEL_OCCLUSION_CULL", false) ? 1 : 0;
    ctx->occlusion_diag = env_flag("VOXEL_DIAG_OCCLUSION", false) ? 1 : 0;
    if (!ctx->submit_buffer) {
        free(ctx->submit_buffer);
        gpu_transport_close(ctx->transport);
        free(ctx);
        return NULL;
    }

    ctx->light_dir = normalize_vec3((Vec3){ 0.35f, 0.92f, 0.20f });
    ctx->world_daylight = 1.0f;
    update_face_light_flags(ctx);
    upload_default_palette(ctx->transport);
    upload_light_palette_banks(ctx, ctx->world_daylight);
    renderer_set_fog_state(ctx, &(struct voxel_fog_state){0});
    return ctx;
}

void renderer_shutdown(RenderContext *ctx)
{
    if (!ctx) return;
    free(ctx->translucent_scratch);
    free(ctx->lookup_entries);
    free(ctx->submit_buffer);
    gpu_transport_close(ctx->transport);
    free(ctx);
}

void renderer_begin_frame(RenderContext *ctx)
{
    ctx->n_quads = 0;
    ctx->submit_bytes = 0;
    occlusion_reset(ctx);
    /* Front-load the per-quad band binning into emit time: stage_prepared_quad
     * will hand each finished descriptor to gpu_transport_bin_descriptor as
     * soon as it is built (descriptor still hot in L1), and the corresponding
     * second-pass walk inside submit_hw_binned is skipped. */
    gpu_transport_begin_descriptors(ctx->transport);
}

void renderer_end_frame(RenderContext *ctx)
{
    if (ctx->occlusion_diag && ((ctx->occlusion_frame_count++ % 60u) == 0u)) {
        fprintf(stderr,
                "renderer: occlusion tested=%u culled=%u tiles=%u enabled=%u\n",
                ctx->occlusion_tested,
                ctx->occlusion_culled,
                ctx->occlusion_tiles_written,
                (unsigned)ctx->occlusion_enabled);
    }

    if (gpu_transport_clear(ctx->transport) < 0)
        return;
    if (!renderer_flush_gpu_state(ctx))
        return;

    if (ctx->submit_bytes == 0) {
        gpu_transport_flip(ctx->transport);
        return;
    }

    if (gpu_transport_submit_descriptors(ctx->transport,
                                         ctx->submit_buffer,
                                         ctx->submit_bytes) < 0)
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
    const bool input_textured = (quad.flags & QUAD_FLAG_TEX) != 0;
    const size_t textured_descriptor_bytes =
        gpu_transport_textured_descriptor_size(ctx->transport);
    const size_t max_descriptor_bytes =
        input_textured ? textured_descriptor_bytes : sizeof(struct quad_desc);

    if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
        return false;
    if (ctx->submit_bytes + max_descriptor_bytes > ctx->submit_capacity)
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
    PlaneBasis basis = choose_plane_basis(v);

    /* Inclusive integer bbox over pixel-center samples. */
    int32_t qxmin = to_q24_8(v[0].x), qxmax = qxmin;
    int32_t qymin = to_q24_8(v[0].y), qymax = qymin;
    for (int i = 1; i < 4; i++) {
        int32_t qx = to_q24_8(v[i].x);
        int32_t qy = to_q24_8(v[i].y);

        if (qx < qxmin) qxmin = qx;
        if (qx > qxmax) qxmax = qx;
        if (qy < qymin) qymin = qy;
        if (qy > qymax) qymax = qy;
    }
    int x_min = ceil_div_256_i32(qxmin - 128);  if (x_min <   0) x_min =   0;
    int y_min = ceil_div_256_i32(qymin - 128);  if (y_min <   0) y_min =   0;
    int x_max = floor_div_256_i32(qxmax - 128);
    int y_max = floor_div_256_i32(qymax - 128);
    if (x_max > (int)VOXEL_RENDER_WIDTH - 1)
        x_max = (int)VOXEL_RENDER_WIDTH - 1;
    if (y_max > (int)VOXEL_RENDER_HEIGHT - 1)
        y_max = (int)VOXEL_RENDER_HEIGHT - 1;

    if (x_min > x_max || y_min > y_max)
        return false;   /* degenerate / off-screen */

    if (ctx->occlusion_enabled &&
        ctx->occlusion_pass == OCCLUSION_PASS_OPAQUE_WORLD &&
        occlusion_quad_eligible(quad.flags)) {
        ctx->occlusion_tested++;
        if (occlusion_quad_hidden(ctx, v, x_min, y_min, x_max, y_max)) {
            ctx->occlusion_culled++;
            return false;
        }
    }

    struct quad_desc *d = (struct quad_desc *)(ctx->submit_buffer + ctx->submit_bytes);
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
    fit_depth_plane(v, &basis, (float)x_min + 0.5f, (float)y_min + 0.5f,
                    &z0, &dz_dx, &dz_dy);
    d->z0 = z0;
    d->dz_dx = dz_dx;
    d->dz_dy = dz_dy;

    d->tex_or_color = (quad.flags & QUAD_FLAG_TEX) ? quad.texture_id : quad.color_tint;
    d->flags = quad.flags;

#ifdef DEBUG_FLAT_COLOR
    /*
     * Diagnostic mode: force every quad to render as a flat palette
     * entry (bypasses the texture ROM + UV interpolation + light-bank
     * indirection) while keeping rasterization, z-test, fog and
     * alpha-blending paths intact.
     *
     * Use this to narrow down where "chromatic aberration" lives: if
     * every block becomes a uniform gray/whatever-you-set with no
     * colored edges in this mode, the culprit is the texture sampling
     * path (ROM latency, UV step, tex0_tex_addr). If fringes persist,
     * look further downstream (palette, fog blend, framebuffer RMW,
     * VGA scanout).
     *
     * DEBUG_FLAT_COLOR_INDEX defaults to PAL_STONE_LIGHT (24) which is
     * a neutral mid-gray -- any palette index works.
     */
#ifndef DEBUG_FLAT_COLOR_INDEX
#define DEBUG_FLAT_COLOR_INDEX 24
#endif
    d->flags = (uint16_t)(quad.flags & ~(QUAD_FLAG_TEX));
    d->tex_or_color = (uint8_t)DEBUG_FLAT_COLOR_INDEX;
#endif

    size_t emitted_size;
    if (d->flags & QUAD_FLAG_TEX) {
        struct quad_desc_uv *uv = (struct quad_desc_uv *)
            (ctx->submit_buffer + ctx->submit_bytes + sizeof(*d));
        memset(uv, 0, sizeof(*uv));
        fit_uv_plane(v, &basis, (float)x_min + 0.5f, (float)y_min + 0.5f, uv);
        emitted_size = textured_descriptor_bytes;
    } else {
        emitted_size = sizeof(*d);
    }
    ctx->submit_bytes += emitted_size;
    ctx->n_quads++;

    /* Bin-during-emit: route this just-built descriptor into the per-band
     * bins now while it is still in L1, instead of having submit_hw_binned
     * walk the contiguous stream a second time. No-op when the transport is
     * not in HW mode. */
    gpu_transport_bin_descriptor(ctx->transport, d, emitted_size);

    if (ctx->occlusion_enabled &&
        ctx->occlusion_pass == OCCLUSION_PASS_OPAQUE_WORLD &&
        occlusion_quad_eligible(d->flags)) {
        occlusion_record_quad(ctx, v, x_min, y_min, x_max, y_max);
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
        if (!is_face_exposed(block, (BlockFace)f, lookup))
            continue;
        emit_block_face(ctx, block->type, block->position, (BlockFace)f);
    }
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

static bool falling_block_hides_mesh_cell(const VoxelWorld *world,
                                          int wx, int wy, int wz)
{
    for (int i = 0; i < WORLD_MAX_FALLING_BLOCKS; i++) {
        const FallingBlock *falling = &world->falling_blocks[i];

        if (!falling->active)
            continue;
        if (falling->wx == wx &&
            falling->origin_y == wy &&
            falling->wz == wz)
            return true;
    }

    return false;
}

static int renderer_draw_falling_blocks(RenderContext *ctx,
                                        const VoxelWorld *world)
{
    int before = ctx->n_quads;

    for (int i = 0; i < WORLD_MAX_FALLING_BLOCKS; i++) {
        const FallingBlock *falling = &world->falling_blocks[i];
        if (!falling->active)
            continue;

        Vec3 block_pos = {
            (float)falling->wx,
            falling->y,
            (float)falling->wz,
        };
        for (int f = 0; f < NUM_FACES; f++) {
            emit_block_face(ctx, falling->type, block_pos, (BlockFace)f);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
                return ctx->n_quads - before;
        }
    }

    return ctx->n_quads - before;
}

int renderer_draw_world(RenderContext *ctx, const VoxelWorld *world,
                        float time_seconds)
{
    int before = ctx->n_quads;
    int candidate_count = 0;

    if (!world || world->chunk_count <= 0)
        return 0;

    update_world_light_state(ctx, time_seconds);
    configure_world_fog(ctx, world);

    ChunkDrawCandidate candidates[world->chunk_count];

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];
        const ChunkMesh *mesh;

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        mesh = atomic_load_explicit(&chunk->live_mesh, memory_order_acquire);
        if (!mesh || mesh->face_count <= 0)
            continue;
        if (!chunk_within_render_distance(ctx, world, chunk))
            continue;
        if (!chunk_intersects_frustum(ctx, chunk))
            continue;

        candidates[candidate_count++] = (ChunkDrawCandidate){
            .chunk = chunk,
            .mesh = mesh,
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

    /*
     * Opaque pass: emit all non-translucent faces first, chunks nearest
     * first. Glass/translucent faces also write Z through QUAD_FLAG_ZTEST,
     * so emitting them first would cause opaque geometry behind the glass
     * to be Z-rejected and we'd blend glass against sky instead of against
     * the stone behind it.
     *
     * Early back-face cull: face normals are axis-aligned (exactly one
     * component ±1, rest zero). The full dot-product in is_face_visible
     * reduces to a single comparison per face direction:
     *   sign > 0 axis: skip if (block_pos[axis] + 1.0) - cam[axis] >= 0
     *   sign < 0 axis: skip if   block_pos[axis]        - cam[axis] <= 0
     * Doing this before choose_chunk_face_light_flags saves the light
     * lookup + function call for all back-facing faces (~50% of faces). */
    const Vec3 cam_pos = ctx->current_camera.position;
    ctx->occlusion_pass = ctx->occlusion_enabled ?
                          OCCLUSION_PASS_OPAQUE_WORLD :
                          OCCLUSION_PASS_DISABLED;
    for (int i = 0; i < candidate_count; i++) {
        const Chunk *chunk = candidates[i].chunk;
        const ChunkMesh *mesh = candidates[i].mesh;

        for (int face_index = 0; face_index < mesh->face_count; face_index++) {
            const ChunkFace *face = &mesh->faces[face_index];
            BlockID id = (BlockID)face->type;
            int wxi = chunk->chunk_x * WORLD_CHUNK_SIZE + (int)face->x;
            int wyi = (int)face->y;
            int wzi = chunk->chunk_z * WORLD_CHUNK_SIZE + (int)face->z;

            if (block_is_translucent(id))
                continue;
            if (falling_block_hides_mesh_cell(world, wxi, wyi, wzi))
                continue;

            float wx = (float)wxi;
            float wy = (float)wyi;
            float wz = (float)wzi;
            BlockRenderModel model = block_render_model(id);

            if (model == BLOCK_RENDER_CROSS ||
                model == BLOCK_RENDER_TORCH ||
                model == BLOCK_RENDER_DOOR ||
                model == BLOCK_RENDER_FLAT) {
                Vec3 block_pos = { wx, wy, wz };
                uint8_t light_flags = choose_chunk_face_light_flags(ctx, id,
                                                                    FACE_TOP,
                                                                    face->sky_light,
                                                                    face->block_light);

                if (model == BLOCK_RENDER_CROSS)
                    emit_cross_block_face_lit(ctx, id, block_pos,
                                              face->face, light_flags);
                else if (model == BLOCK_RENDER_TORCH)
                    emit_torch_block_face_lit(ctx, id, block_pos,
                                              face->face, light_flags);
                else if (model == BLOCK_RENDER_DOOR)
                    emit_door_block_face_lit(ctx, id, block_pos,
                                             face->face, light_flags);
                else
                    emit_flat_block_face_lit(ctx, id, world, block_pos,
                                             wxi, wyi, wzi, light_flags);
                if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT) {
                    ctx->occlusion_pass = OCCLUSION_PASS_DISABLED;
                    return ctx->n_quads - before;
                }
                continue;
            }

            /* Early back-face cull using axis-aligned normal. */
            switch ((BlockFace)face->face) {
            case FACE_TOP:    if (cam_pos.y < wy + 1.0f) continue; break;
            case FACE_BOTTOM: if (cam_pos.y > wy)         continue; break;
            case FACE_RIGHT:  if (cam_pos.x < wx + 1.0f) continue; break;
            case FACE_LEFT:   if (cam_pos.x > wx)         continue; break;
            case FACE_BACK:   if (cam_pos.z < wz + 1.0f) continue; break;
            case FACE_FRONT:  if (cam_pos.z > wz)         continue; break;
            default: break;
            }

            Vec3 block_pos = { wx, wy, wz };
            uint8_t light_flags = choose_chunk_face_light_flags(ctx, id,
                                                                (BlockFace)face->face,
                                                                face->sky_light,
                                                                face->block_light);

            emit_merged_block_face_lit(ctx, id, block_pos,
                                       (BlockFace)face->face,
                                       face->u_size ? face->u_size : 1,
                                       face->v_size ? face->v_size : 1,
                                       face->height ? face->height : 8,
                                       light_flags);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT) {
                ctx->occlusion_pass = OCCLUSION_PASS_DISABLED;
                return ctx->n_quads - before;
            }
        }
    }
    ctx->occlusion_pass = OCCLUSION_PASS_DISABLED;

    renderer_draw_falling_blocks(ctx, world);
    if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
        return ctx->n_quads - before;

    /*
     * Translucent pass: collect every glass face, sort back-to-front by
     * view-space depth of its block center, then emit. Two alpha-blended
     * quads at the same pixel must be drawn farthest-first so each blend
     * mixes against the correctly-composited destination. Block-center as
     * the sort key is fine because we back-face cull: the three (or fewer)
     * faces of a single glass block visible at once never overlap in
     * screen space, so their within-block order doesn't matter.
     */
    int w = 0;
    for (int i = 0; i < candidate_count; i++) {
        const Chunk *chunk = candidates[i].chunk;
        const ChunkMesh *mesh = candidates[i].mesh;
        const float chunk_ox = (float)(chunk->chunk_x * WORLD_CHUNK_SIZE);
        const float chunk_oz = (float)(chunk->chunk_z * WORLD_CHUNK_SIZE);

        for (int face_index = 0; face_index < mesh->face_count; face_index++) {
            const ChunkFace *face = &mesh->faces[face_index];
            if (!block_is_translucent((BlockID)face->type))
                continue;

            float wx = chunk_ox + (float)face->x;
            float wy = (float)face->y;
            float wz = chunk_oz + (float)face->z;
            switch ((BlockFace)face->face) {
            case FACE_TOP:    if (cam_pos.y < wy + 1.0f) continue; break;
            case FACE_BOTTOM: if (cam_pos.y > wy)         continue; break;
            case FACE_RIGHT:  if (cam_pos.x < wx + 1.0f) continue; break;
            case FACE_LEFT:   if (cam_pos.x > wx)         continue; break;
            case FACE_BACK:   if (cam_pos.z < wz + 1.0f) continue; break;
            case FACE_FRONT:  if (cam_pos.z > wz)         continue; break;
            default: break;
            }

            if (!ensure_translucent_scratch(ctx, w + 1))
                continue;

            Vec3 block_center = { wx + 0.5f, wy + 0.5f, wz + 0.5f };
            CameraVertex cv;
            world_to_camera(ctx, block_center, &cv);

            ctx->translucent_scratch[w++] = (TranslucentFaceRef){
                .chunk = chunk,
                .face = face,
                .view_z = cv.z,
            };
        }
    }

    if (w > 0) {
        if (w > 1) {
            qsort(ctx->translucent_scratch, (size_t)w,
                  sizeof(*ctx->translucent_scratch),
                  compare_translucent_back_to_front);
        }

        for (int i = 0; i < w; i++) {
            const TranslucentFaceRef *ref = &ctx->translucent_scratch[i];
            Vec3 block_pos = {
                (float)(ref->chunk->chunk_x * WORLD_CHUNK_SIZE + ref->face->x),
                (float)ref->face->y,
                (float)(ref->chunk->chunk_z * WORLD_CHUNK_SIZE + ref->face->z),
            };
            uint8_t light_flags = choose_chunk_face_light_flags(
                ctx,
                (BlockID)ref->face->type,
                (BlockFace)ref->face->face,
                ref->face->sky_light,
                ref->face->block_light);

            emit_merged_block_face_lit(ctx, (BlockID)ref->face->type,
                                       block_pos, (BlockFace)ref->face->face,
                                       ref->face->u_size ? ref->face->u_size : 1,
                                       ref->face->v_size ? ref->face->v_size : 1,
                                       ref->face->height ? ref->face->height : 8,
                                       light_flags);
            if (ctx->n_quads >= MAX_QUADS_IN_FLIGHT)
                return ctx->n_quads - before;
        }
    }

    return ctx->n_quads - before;
}

static bool push_screen_textured_quad(RenderContext *ctx,
                                      float x0, float y0,
                                      float x1, float y1,
                                      float u0, float v0,
                                      float u1, float v1,
                                      uint8_t texture_id,
                                      uint8_t extra_flags)
{
    RenderQuad quad = {0};

    quad.texture_id = texture_id;
    quad.flags = QUAD_FLAG_TEX | extra_flags;
    quad.vertices[0] = (Vertex2D){ x0, y0, 0.0f, u0, v0, 1.0f };
    quad.vertices[1] = (Vertex2D){ x1, y0, 0.0f, u1, v0, 1.0f };
    quad.vertices[2] = (Vertex2D){ x1, y1, 0.0f, u1, v1, 1.0f };
    quad.vertices[3] = (Vertex2D){ x0, y1, 0.0f, u0, v1, 1.0f };
    if (projected_quad_fully_inside_viewport(quad.vertices))
        return stage_projected_quad_no_clip(ctx, &quad);
    return renderer_push_quad(ctx, &quad);
}

static bool push_screen_flat_quad(RenderContext *ctx,
                                  float x0, float y0,
                                  float x1, float y1,
                                  uint8_t palette_index)
{
    return renderer_fill_rect(ctx, x0, y0, x1, y1, palette_index, 0);
}

bool renderer_draw_screen_tile(RenderContext *ctx,
                               float x0, float y0, float x1, float y1,
                               uint8_t texture_id, uint8_t extra_flags)
{
    return push_screen_textured_quad(ctx,
                                     x0, y0, x1, y1,
                                     0.0f, 0.0f, 16.0f, 16.0f,
                                     texture_id,
                                     extra_flags);
}

bool renderer_draw_custom_screen_quad(RenderContext *ctx,
                                      float x0, float y0,
                                      float x1, float y1,
                                      float x2, float y2,
                                      float x3, float y3,
                                      uint8_t texture_id, uint8_t extra_flags)
{
    RenderQuad quad = {0};

    quad.texture_id = texture_id;
    quad.flags = QUAD_FLAG_TEX | extra_flags;
    quad.vertices[0] = (Vertex2D){ x0, y0, 0.0f, 0.0f,  0.0f,  1.0f };
    quad.vertices[1] = (Vertex2D){ x1, y1, 0.0f, 16.0f, 0.0f,  1.0f };
    quad.vertices[2] = (Vertex2D){ x2, y2, 0.0f, 16.0f, 16.0f, 1.0f };
    quad.vertices[3] = (Vertex2D){ x3, y3, 0.0f, 0.0f,  16.0f, 1.0f };
    if (projected_quad_fully_inside_viewport(quad.vertices))
        return stage_projected_quad_no_clip(ctx, &quad);
    return renderer_push_quad(ctx, &quad);
}

bool renderer_draw_world_billboard_tile(RenderContext *ctx,
                                        Vec3 center,
                                        float size_world,
                                        uint8_t texture_id,
                                        uint8_t extra_flags)
{
    CameraVertex camera;
    Vertex2D projected;
    RenderQuad quad = {0};
    float inv_w;
    float half_px;

    if (!ctx || size_world <= 0.0f)
        return false;

    world_to_camera(ctx, center, &camera);
    if (!project_camera_vertex(ctx, &camera, &projected))
        return false;

    inv_w = 1.0f / camera.z;
    half_px = ctx->current_camera.depth * size_world * 0.5f * inv_w;
    if (half_px < 1.0f)
        half_px = 1.0f;

    quad.texture_id = texture_id;
    quad.flags = QUAD_FLAG_TEX | extra_flags;
    quad.vertices[0] = (Vertex2D){
        projected.x - half_px, projected.y - half_px, projected.z,
        0.0f * inv_w, 0.0f * inv_w, inv_w,
    };
    quad.vertices[1] = (Vertex2D){
        projected.x + half_px, projected.y - half_px, projected.z,
        16.0f * inv_w, 0.0f * inv_w, inv_w,
    };
    quad.vertices[2] = (Vertex2D){
        projected.x + half_px, projected.y + half_px, projected.z,
        16.0f * inv_w, 16.0f * inv_w, inv_w,
    };
    quad.vertices[3] = (Vertex2D){
        projected.x - half_px, projected.y + half_px, projected.z,
        0.0f * inv_w, 16.0f * inv_w, inv_w,
    };

    if (projected_quad_fully_inside_viewport(quad.vertices))
        return stage_projected_quad_no_clip(ctx, &quad);
    return renderer_push_quad(ctx, &quad);
}

bool renderer_fill_rect(RenderContext *ctx,
                        float x0, float y0, float x1, float y1,
                        uint8_t palette_index, uint8_t extra_flags)
{
    RenderQuad quad = {0};

    quad.color_tint = palette_index;
    quad.flags = extra_flags;
    quad.vertices[0] = (Vertex2D){ x0, y0, 0.0f, 0.0f, 0.0f, 1.0f };
    quad.vertices[1] = (Vertex2D){ x1, y0, 0.0f, 0.0f, 0.0f, 1.0f };
    quad.vertices[2] = (Vertex2D){ x1, y1, 0.0f, 0.0f, 0.0f, 1.0f };
    quad.vertices[3] = (Vertex2D){ x0, y1, 0.0f, 0.0f, 0.0f, 1.0f };
    if (projected_quad_fully_inside_viewport(quad.vertices))
        return stage_projected_quad_no_clip(ctx, &quad);
    return renderer_push_quad(ctx, &quad);
}

static bool project_sky_direction(RenderContext *ctx, Vec3 dir, float *screen_x, float *screen_y)
{
    Vec3 world = {
        ctx->current_camera.position.x + dir.x * SKY_DOME_DISTANCE,
        ctx->current_camera.position.y + dir.y * SKY_DOME_DISTANCE,
        ctx->current_camera.position.z + dir.z * SKY_DOME_DISTANCE,
    };
    CameraVertex camera = {0};
    Vertex2D projected;

    world_to_camera(ctx, world, &camera);
    if (!project_camera_vertex(ctx, &camera, &projected))
        return false;

    *screen_x = projected.x;
    *screen_y = projected.y;
    return true;
}

static bool draw_sky_sprite(RenderContext *ctx, Vec3 dir, float size_px, uint8_t texture_id)
{
    float center_x;
    float center_y;
    float half = size_px * 0.5f;

    if (!project_sky_direction(ctx, dir, &center_x, &center_y))
        return false;

    return push_screen_textured_quad(ctx,
                                     center_x - half, center_y - half,
                                     center_x + half, center_y + half,
                                     0.0f, 0.0f, 16.0f, 16.0f,
                                     texture_id,
                                     QUAD_FLAG_ALPHA_KEY);
}

static RGB24 sample_sky_gradient(const SkyPalette *palette, float t)
{
    if (t <= 0.22f)
        return lerp_rgb24(palette->zenith, palette->high, smoothstepf(0.0f, 0.22f, t));
    if (t <= 0.62f)
        return lerp_rgb24(palette->high, palette->mid, smoothstepf(0.22f, 0.62f, t));
    return lerp_rgb24(palette->mid, palette->horizon, smoothstepf(0.62f, 1.0f, t));
}

static void draw_sky_gradient(RenderContext *ctx, const SkyPalette *palette)
{
    for (int band = 0; band < SKY_GRADIENT_BANDS; band++) {
        float y0 = SCREEN_HEIGHT * (float)band / (float)SKY_GRADIENT_BANDS;
        float y1 = SCREEN_HEIGHT * (float)(band + 1) / (float)SKY_GRADIENT_BANDS;
        float t = ((float)band + 0.5f) / (float)SKY_GRADIENT_BANDS;
        uint8_t palette_index = (uint8_t)(PAL_SKY_GRADIENT_BASE + band);
        RGB24 color = sample_sky_gradient(palette, t);

        renderer_set_sky_palette_rgb(ctx, (uint8_t)band, color);
        push_screen_flat_quad(ctx, 0.0f, y0, SCREEN_WIDTH, y1, palette_index);
    }
}

static Vec3 sun_direction_for_time(float time_seconds)
{
    float cycle = fmodf(time_seconds / SKY_DAY_LENGTH_SECONDS + 0.18f, 1.0f);

    if (cycle < 0.0f)
        cycle += 1.0f;

    float orbit = cycle * (2.0f * PI_F);
    return normalize_vec3((Vec3){
        cosf(orbit),
        0.92f * sinf(orbit),
        0.35f,
    });
}

static SkyPalette make_sky_palette(Vec3 sun_dir)
{
    float daylight = smoothstepf(-0.18f, 0.08f, sun_dir.y);
    float twilight = clamp01(1.0f - fabsf(sun_dir.y) * 4.5f);
    float night = 1.0f - daylight;
    SkyPalette palette = {
        .zenith = lerp_rgb24(rgb24(0x08, 0x0a, 0x16), rgb24(0x58, 0x96, 0xdb), daylight),
        .high = lerp_rgb24(rgb24(0x15, 0x18, 0x31), rgb24(0x7f, 0xbd, 0xff), daylight),
        .mid = lerp_rgb24(rgb24(0x22, 0x24, 0x46), rgb24(0xad, 0xd8, 0xff), daylight),
        .horizon = lerp_rgb24(rgb24(0x2c, 0x1f, 0x2d), rgb24(0xdf, 0xee, 0xff), daylight),
        .cloud = lerp_rgb24(rgb24(0x54, 0x5c, 0x79), rgb24(0xf5, 0xfa, 0xff), daylight),
        .cloud_shadow = lerp_rgb24(rgb24(0x34, 0x3b, 0x53), rgb24(0xcb, 0xd8, 0xec), daylight),
        .sun_core = rgb24(0xff, 0xee, 0xaa),
        .sun_glow = rgb24(0xff, 0xbd, 0x58),
        .moon = lerp_rgb24(rgb24(0xb9, 0xc3, 0xe0), rgb24(0xe7, 0xeb, 0xf8), night),
        .moon_shadow = lerp_rgb24(rgb24(0x5d, 0x67, 0x85), rgb24(0x9c, 0xa4, 0xc0), night),
    };
    RGB24 sunset = rgb24(0xff, 0xab, 0x61);

    twilight *= 0.35f + 0.65f * night;
    palette.zenith = mix_rgb24(palette.zenith, rgb24(0x52, 0x3d, 0x5d), twilight * 0.22f);
    palette.high = mix_rgb24(palette.high, rgb24(0xa0, 0x5f, 0x62), twilight * 0.28f);
    palette.mid = mix_rgb24(palette.mid, rgb24(0xe7, 0x88, 0x56), twilight * 0.45f);
    palette.horizon = mix_rgb24(palette.horizon, sunset, twilight * 0.78f);
    palette.cloud = mix_rgb24(palette.cloud, rgb24(0xff, 0xc6, 0x8c), twilight * 0.18f);
    palette.cloud_shadow = mix_rgb24(palette.cloud_shadow, rgb24(0xc7, 0x8d, 0x58), twilight * 0.22f);
    palette.sun_core = mix_rgb24(palette.sun_core, rgb24(0xff, 0xdb, 0x91), twilight * 0.35f);
    palette.sun_glow = mix_rgb24(palette.sun_glow, rgb24(0xff, 0x97, 0x4a), twilight * 0.50f);
    palette.star = lerp_rgb24(palette.zenith, rgb24(0xff, 0xff, 0xff),
                              smoothstepf(0.15f, 0.95f, night));

    return palette;
}

static void upload_sky_palette(RenderContext *ctx, const SkyPalette *palette)
{
    renderer_set_palette_rgb(ctx, 0, palette->zenith);
    renderer_set_palette_rgb(ctx, PAL_SKY_HIGH, palette->high);
    renderer_set_palette_rgb(ctx, PAL_SKY_MID, palette->mid);
    renderer_set_palette_rgb(ctx, PAL_SKY_HORIZON, palette->horizon);
    renderer_set_palette_rgb(ctx, PAL_CLOUD, palette->cloud);
    renderer_set_palette_rgb(ctx, PAL_CLOUD_SHADOW, palette->cloud_shadow);
    renderer_set_palette_rgb(ctx, PAL_SUN_CORE, palette->sun_core);
    renderer_set_palette_rgb(ctx, PAL_SUN_GLOW, palette->sun_glow);
    renderer_set_palette_rgb(ctx, PAL_MOON, palette->moon);
    renderer_set_palette_rgb(ctx, PAL_MOON_SHADOW, palette->moon_shadow);
    renderer_set_palette_rgb(ctx, PAL_STAR, palette->star);
}

int renderer_draw_sky(RenderContext *ctx, float time_seconds)
{
    typedef struct {
        float azimuth;
        float elevation;
        float size_px;
        float drift;
        float wobble;
    } SkySpriteDef;

    static const SkySpriteDef star_defs[] = {
        { 0.20f,  0.95f, 34.0f, 0.0f, 0.0f },
        { 1.30f,  0.72f, 40.0f, 0.0f, 0.0f },
        { 2.45f,  0.84f, 36.0f, 0.0f, 0.0f },
        { 3.60f,  0.66f, 44.0f, 0.0f, 0.0f },
        { 4.55f,  0.90f, 34.0f, 0.0f, 0.0f },
        { 5.60f,  0.76f, 38.0f, 0.0f, 0.0f },
    };
    static const SkySpriteDef cloud_defs[] = {
        { 0.15f, 0.48f, 62.0f,  0.014f, 0.23f },
        { 1.65f, 0.41f, 54.0f, -0.010f, 0.19f },
        { 3.20f, 0.52f, 68.0f,  0.012f, 0.17f },
        { 4.85f, 0.37f, 58.0f, -0.008f, 0.21f },
    };
    int before = ctx->n_quads;
    Vec3 sun_dir;
    Vec3 moon_dir;
    SkyPalette palette;
    float daylight;
    float night;

    if (!ctx)
        return 0;

    /* Continuous time drives sprite drift below; quantized time drives every
     * palette computation so hw_sky_epoch stays stable for ~15 frames at
     * 30 FPS and sky_band_reuse can hit on primer-only bands. Both this path
     * and renderer_draw_world use palette_time_for() so they cannot disagree
     * across a 32-step daylight_key boundary and re-flush the palette. */
    float palette_time = palette_time_for(time_seconds);
    /* Palette and lighting use quantized time so hw_sky_epoch stays stable. */
    Vec3 palette_sun_dir = sun_direction_for_time(palette_time);
    update_world_light_from_sun(ctx, palette_sun_dir);
    daylight = ctx->world_daylight;
    night = 1.0f - daylight;
    palette = make_sky_palette(palette_sun_dir);
    upload_sky_palette(ctx, &palette);
    draw_sky_gradient(ctx, &palette);

    /* Sprite positions use continuous time so movement is smooth. */
    sun_dir = sun_direction_for_time(time_seconds);
    moon_dir = (Vec3){ -sun_dir.x, -sun_dir.y, -sun_dir.z };

    if (night > 0.2f) {
        for (size_t i = 0; i < sizeof(star_defs) / sizeof(star_defs[0]); i++) {
            draw_sky_sprite(ctx,
                            direction_from_azimuth_elevation(star_defs[i].azimuth,
                                                             star_defs[i].elevation),
                            star_defs[i].size_px,
                            TEX_TILE_STARS);
        }
    }

    if (sun_dir.y > 0.0f)
        draw_sky_sprite(ctx, sun_dir, 34.0f, TEX_TILE_SUN);
    if (moon_dir.y > 0.0f)
        draw_sky_sprite(ctx, moon_dir, 30.0f, TEX_TILE_MOON);

    for (size_t i = 0; i < sizeof(cloud_defs) / sizeof(cloud_defs[0]); i++) {
        float azimuth = cloud_defs[i].azimuth + cloud_defs[i].drift * time_seconds;
        float elevation = cloud_defs[i].elevation +
                          0.04f * sinf(time_seconds * cloud_defs[i].wobble + (float)i * 1.7f);

        draw_sky_sprite(ctx,
                        direction_from_azimuth_elevation(azimuth, elevation),
                        cloud_defs[i].size_px,
                        TEX_TILE_CLOUD);
    }

    return ctx->n_quads - before;
}

/* Project a world-space vertical line segment to screen and draw it as a
 * 1-pixel-wide line using a screen-space quad. Returns true if visible. */
static bool draw_projected_line(RenderContext *ctx,
                                float wx0, float wy0, float wz0,
                                float wx1, float wy1, float wz1,
                                uint8_t palette_index)
{
    CameraVertex cam0, cam1;
    Vertex2D scr0, scr1;

    world_to_camera(ctx, (Vec3){wx0, wy0, wz0}, &cam0);
    world_to_camera(ctx, (Vec3){wx1, wy1, wz1}, &cam1);
    if (!project_camera_vertex(ctx, &cam0, &scr0) ||
        !project_camera_vertex(ctx, &cam1, &scr1))
        return false;

    float dx = scr1.x - scr0.x;
    float dy = scr1.y - scr0.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.1f) return false;

    /* Normal vector to the line in screen space */
    float nx = -dy / len;
    float ny = dx / len;
    
    /* 0.5px half-width for a 1-pixel wide line */
    float hw = 0.5f;
    nx *= hw;
    ny *= hw;

    /* Build a quad connecting the two points, 1px thick.
     * Force z=0.0f (nearest possible depth) so the debug lines
     * draw cleanly over all scene blocks. */
    RenderQuad quad = {0};
    quad.color_tint = palette_index;
    quad.flags = 0;
    
    float inv_w0 = scr0.one_over_w;
    float inv_w1 = scr1.one_over_w;
    
    quad.vertices[0] = (Vertex2D){ scr0.x - nx, scr0.y - ny, 0.0f, 0.0f, inv_w0, 0.0f };
    quad.vertices[1] = (Vertex2D){ scr0.x + nx, scr0.y + ny, 0.0f, 0.0f, inv_w0, 0.0f };
    quad.vertices[2] = (Vertex2D){ scr1.x + nx, scr1.y + ny, 0.0f, 0.0f, inv_w1, 0.0f };
    quad.vertices[3] = (Vertex2D){ scr1.x - nx, scr1.y - ny, 0.0f, 0.0f, inv_w1, 0.0f };

    if (projected_quad_fully_inside_viewport(quad.vertices))
        return stage_projected_quad_no_clip(ctx, &quad);
    return renderer_push_quad(ctx, &quad);
}

int renderer_draw_chunk_borders(RenderContext *ctx,
                                float player_x, float player_z,
                                int render_distance)
{
    if (!ctx || render_distance <= 0)
        return 0;

    int drawn = 0;
    int player_cx = (int)floorf(player_x / (float)WORLD_CHUNK_SIZE);
    int player_cz = (int)floorf(player_z / (float)WORLD_CHUNK_SIZE);

    /* Draw vertical lines at chunk boundaries within render distance.
     * Lines run from y=0 to y=WORLD_CHUNK_HEIGHT at each chunk edge. */
    float y_bot = 0.0f;
    float y_top = (float)WORLD_CHUNK_HEIGHT;

    for (int dx = -render_distance; dx <= render_distance + 1; dx++) {
        float wx = (float)((player_cx + dx) * WORLD_CHUNK_SIZE);
        /* Highlight the player's chunk borders in yellow (palette 8),
         * other chunk borders in white (palette 5). */
        bool is_player_edge = (dx == 0 || dx == 1);
        uint8_t color = is_player_edge ? 8 : 5;

        for (int dz = -render_distance; dz <= render_distance + 1; dz++) {
            float wz = (float)((player_cz + dz) * WORLD_CHUNK_SIZE);
            bool is_player_z = (dz == 0 || dz == 1);
            uint8_t line_color = (is_player_edge && is_player_z) ? 8 : color;

            /* Vertical line at this grid intersection. */
            if (draw_projected_line(ctx, wx, y_bot, wz, wx, y_top, wz,
                                    line_color))
                drawn++;
        }
    }

    return drawn;
}

bool renderer_draw_crosshair(RenderContext *ctx)
{
    const float cx = SCREEN_WIDTH * 0.5f;
    const float cy = SCREEN_HEIGHT * 0.5f;
    const float half = 8.0f * HUD_SCALE;
    const float thickness = 1.25f * HUD_SCALE;
    const uint8_t white = 5;
    bool ok = true;

    ok &= renderer_fill_rect(ctx,
                             cx - half, cy - thickness * 0.5f,
                             cx + half, cy + thickness * 0.5f,
                             white, QUAD_ALPHA_75);
    ok &= renderer_fill_rect(ctx,
                             cx - thickness * 0.5f, cy - half,
                             cx + thickness * 0.5f, cy + half,
                             white, QUAD_ALPHA_75);
    return ok;
}
