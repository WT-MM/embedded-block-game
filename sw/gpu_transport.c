#include "gpu_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#define DEV_PATH "/dev/voxel_gpu"
#define SOCKET_MAGIC "VGPU"
#define HW_BAND_HASH_OFFSET UINT64_C(14695981039346656037)
#define HW_BAND_HASH_PRIME  UINT64_C(1099511628211)
#define HW_RECT_STRIDE_MAX_WIDTH 320u

typedef enum {
    GPU_BACKEND_HW = 0,
    GPU_BACKEND_SOCKET,
    GPU_BACKEND_TEE,
} GPUBackendMode;

struct pipeline_submit_job;

struct hw_band_cache_entry {
    uint64_t hash;
    size_t bytes;
    uint32_t render_epoch;
    uint32_t sky_epoch;
    int has_scene_rect;
    uint16_t scene_x_min;
    uint16_t scene_x_max;
    uint8_t scene_y_min;
    uint8_t scene_y_max;
};

struct GPUTransport {
    GPUBackendMode mode;
    int hw_fd;
    int socket_fd;
    int hw_flip_pending;
    int hw_async_flip_supported;
    int hw_sky_gradient_clear_enabled;
    uint32_t hw_render_epoch;
    uint32_t hw_sky_epoch;
    uint32_t hw_sky_band_epoch[2][VOXEL_BAND_COUNT];
    struct hw_band_cache_entry hw_band_cache[2][VOXEL_BAND_COUNT];
    /* Diag: epoch-bump counters since last frame log. Reset by
     * submit_descriptors after the per-frame line is emitted. */
    uint32_t hw_render_epoch_bumps;
    uint32_t hw_sky_epoch_bumps;
    uint8_t hw_sky_epoch_bump_indices[8];
    uint32_t hw_sky_epoch_bump_count;
    /* Bin-during-emit staging flag. Set by gpu_transport_begin_descriptors
     * and cleared by submit_hw_binned after it consumes the staged bins. */
    int staged_active;
    int pipeline_enabled;
    int pipeline_thread_started;
    int pipeline_shutdown;
    int pipeline_job_ready;
    int pipeline_job_running;
    int pipeline_flip_owned;
    int pipeline_ret;
    pthread_t pipeline_thread;
    pthread_mutex_t pipeline_mutex;
    pthread_cond_t pipeline_cond;
    struct pipeline_submit_job *pipeline_job;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct descriptor_bin {
    /* Flat buffer for binned descriptors */
    uint8_t *flat_buf;
    size_t flat_used;
    size_t flat_capacity;
    int has_scene_rect;
    uint16_t scene_x_min;
    uint16_t scene_x_max;
    uint8_t scene_y_min;
    uint8_t scene_y_max;
};

/* Two parallel sets of per-band bins. With frame pipelining enabled, the
 * submit worker reads the set it was handed
 * (`worker_bin_set`) while the main thread has already swapped to the other
 * set (`g_main_bin_set`) for the next frame. Pipelining off: only set 0 is
 * ever used. Functions that touch bins take the set as a parameter so the
 * worker thread reads the correct set without racing on a global. */
static struct descriptor_bin g_bins_pool[2][VOXEL_BAND_COUNT];
static int g_main_bin_set = 0;

/* Per-band "primer" quad. Submitted as the first descriptor of every band so
 * the very first pixel through the rasterizer pipeline after BEGIN_BAND lands
 * on a known, hidden 1-px write at column 0 of the band's first scanline.
 *
 * The motivation is the family of palette-pipeline staleness bugs documented
 * in PROJECT_NOTES.md: after the FIFO drains and rasterization restarts, the
 * first pixel through the pipeline can latch a stale palette index from the
 * previous frame's tail (red `0x06` was historically the most-visible value).
 * This was patched on the per-quad seam with the pal_rd+plr stages, but the
 * SDRAM banding upgrade introduced a new "first-quad-of-band" idle restart
 * which manifests as occasional red speckles on the absolute left edge.
 *
 * The primer absorbs that first-pixel state into a write at (0, band_start)
 * with palette index 0. Sky/world quads then overdraw that pixel, so the
 * primer is invisible in steady state but always paints the leading edge
 * with a known color even if the pipeline is glitchy on its first cycle. */
static struct quad_desc g_band_primers[VOXEL_BAND_COUNT];
static int g_band_primers_initialized;

static void init_band_primers(void)
{
    if (g_band_primers_initialized)
        return;
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        struct quad_desc *p = &g_band_primers[b];
        memset(p, 0, sizeof(*p));
        p->x_min = 0;
        p->x_max = 0;
        p->y_min = (__s16)(b * VOXEL_BAND_CACHE_HEIGHT);
        p->y_max = p->y_min;
        /* All 4 edges constant +1 in Q24.8 → e(x,y) ≥ 0 everywhere → the
         * single bbox pixel evaluates inside, primes the pipeline. */
        for (int i = 0; i < 4; i++) {
            p->edges[i].A = 0;
            p->edges[i].B = 0;
            p->edges[i].C = 1 << 8;
        }
        p->z0 = 0;
        p->dz_dx = 0;
        p->dz_dy = 0;
        p->tex_or_color = 0;  /* palette index 0 = clear color */
        p->flags = 0;          /* flat color, no z-test, no fog, no alpha-key */
    }
    g_band_primers_initialized = 1;
}

/* Diagnostics gated by VOXEL_DIAG_BBOX=1: scan submitted x_min/x_max and
 * y_min/y_max ranges so we can confirm nothing is leaking off-screen on
 * the left/top edges (red-stripe investigation). */
static int g_diag_bbox;
static int g_diag_cache;
static int g_hw_band_reuse;
static unsigned g_diag_frame_count;
static int g_diag_log_flip_next;

enum {
    DIAG_SKY_GRADIENT_BASE = 40,
    DIAG_SKY_GRADIENT_LAST = 63,
    DIAG_SKY_GRADIENT_COLORS = DIAG_SKY_GRADIENT_LAST - DIAG_SKY_GRADIENT_BASE + 1,
    DIAG_EXTMEM_COPY_TARGET_SHIFT = 11,
    HW_EPOCH_INITIAL = 1,
    DIAG_NONSKIP_DESC_OVERHEAD_CYCLES = 17,
    DIAG_SKIP_DESC_OVERHEAD_CYCLES = 2,
    DIAG_COPY_DRAIN_CYCLES = 128,
    DIAG_VGA_FRAME_CYCLES = 840000,
    DIAG_VGA_WRITE_SLACK_CYCLES = 686400,
};

struct diag_cost {
    uint64_t desc_count;
    uint64_t band_copies;
    uint64_t textured_band_copies;
    uint64_t skipped_sky_copies;
    uint64_t cached_sky_band_reuses;
    uint64_t cached_exact_band_reuses;
    uint64_t bbox_pixels_1px;
    uint64_t pair_cycles_2px;
    uint64_t fetch_words;
    uint64_t band_copies_band[VOXEL_BAND_COUNT];
    uint64_t textured_band_copies_band[VOXEL_BAND_COUNT];
    uint64_t bbox_pixels_band[VOXEL_BAND_COUNT];
    uint64_t init_cycles[VOXEL_BAND_COUNT];
    uint64_t fetch_words_band[VOXEL_BAND_COUNT];
    uint64_t desc_overhead_cycles_band[VOXEL_BAND_COUNT];
    uint64_t pair_cycles_band[VOXEL_BAND_COUNT];
    uint64_t flush_words[VOXEL_BAND_COUNT];
};

struct diag_band_submit {
    size_t bytes;
    double begin_ms;
    double write_ms;
    double end_ms;
    double total_ms;
};

/* Bin-during-emit staging accumulators. Populated by
 * gpu_transport_bin_descriptor (called from the renderer at quad-emit
 * time) and consumed by submit_hw_binned, which resets them for the
 * next frame. */
static struct diag_cost g_staged_diag_cost;
static int16_t g_staged_diag_x_min, g_staged_diag_x_max;
static int16_t g_staged_diag_y_min, g_staged_diag_y_max;
static int g_staged_diag_log_frame;
static unsigned g_staged_diag_frame_index;
static uint64_t g_staged_desc_count;

struct pipeline_submit_job {
    int bin_set;
    struct diag_cost diag_cost;
    int16_t diag_x_min, diag_x_max;
    int16_t diag_y_min, diag_y_max;
    int diag_log_frame;
    unsigned diag_frame_index;
    uint64_t desc_count;
};

static double diag_timespec_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 +
           (b->tv_nsec - a->tv_nsec) / 1e6;
}

static double diag_cycles_ms(uint64_t cycles)
{
    return (double)cycles / 50000.0; /* 50 MHz user clock */
}

static uint64_t diag_band_pixels(unsigned band)
{
    unsigned base = band * VOXEL_BAND_CACHE_HEIGHT;
    unsigned rows;

    if (base >= VOXEL_RENDER_HEIGHT)
        return 0;
    rows = VOXEL_RENDER_HEIGHT - base;
    if (rows > VOXEL_BAND_CACHE_HEIGHT)
        rows = VOXEL_BAND_CACHE_HEIGHT;
    return (uint64_t)rows * VOXEL_RENDER_WIDTH;
}

static unsigned band_visible_rows(unsigned band)
{
    unsigned base = band * VOXEL_BAND_CACHE_HEIGHT;
    unsigned rows;

    if (base >= VOXEL_RENDER_HEIGHT)
        return 0;
    rows = VOXEL_RENDER_HEIGHT - base;
    if (rows > VOXEL_BAND_CACHE_HEIGHT)
        rows = VOXEL_BAND_CACHE_HEIGHT;
    return rows;
}

static int diag_clamp_x(int x)
{
    if (x < 0)
        return 0;
    if (x >= (int)VOXEL_RENDER_WIDTH)
        return (int)VOXEL_RENDER_WIDTH - 1;
    return x;
}

static int diag_desc_is_redundant_sky_clear(const struct quad_desc *desc)
{
    return desc &&
           desc->flags == 0 &&
           desc->tex_or_color >= DIAG_SKY_GRADIENT_BASE &&
           desc->tex_or_color <= DIAG_SKY_GRADIENT_LAST &&
           desc->x_min == 0 &&
           desc->x_max == (int16_t)(VOXEL_RENDER_WIDTH - 1);
}

static int sky_palette_index_is_valid(uint8_t index)
{
    return index < DIAG_SKY_GRADIENT_COLORS;
}

static uint64_t hash_band_bytes(const void *data, size_t bytes)
{
    const uint8_t *ptr = data;
    uint64_t hash = HW_BAND_HASH_OFFSET;

    for (size_t i = 0; i < bytes; i++) {
        hash ^= ptr[i];
        hash *= HW_BAND_HASH_PRIME;
    }
    return hash;
}

static void diag_count_omitted_sky_clear(struct diag_cost *cost,
                                         unsigned first_band,
                                         unsigned last_band)
{
    if (!cost)
        return;
    for (unsigned band = first_band; band <= last_band; band++)
        cost->skipped_sky_copies++;
}

static void diag_add_band_descriptor(struct diag_cost *cost,
                                     unsigned band,
                                     const struct quad_desc *desc,
                                     int clipped_y_min,
                                     int clipped_y_max,
                                     size_t desc_size)
{
    int x_min = diag_clamp_x(desc->x_min);
    int x_max = diag_clamp_x(desc->x_max);
    uint64_t rows;
    uint64_t width_1px;
    uint64_t pairs_per_row;
    int x_start_even;
    int redundant_sky_clear;

    if (!cost || band >= VOXEL_BAND_COUNT ||
        clipped_y_max < clipped_y_min || x_max < x_min)
        return;

    rows = (uint64_t)(clipped_y_max - clipped_y_min + 1);
    width_1px = (uint64_t)(x_max - x_min + 1);
    x_start_even = x_min & ~1;
    pairs_per_row = (uint64_t)((x_max - x_start_even + 2) / 2);

    cost->band_copies++;
    cost->band_copies_band[band]++;
    if (desc->flags & QUAD_FLAG_TEX) {
        cost->textured_band_copies++;
        cost->textured_band_copies_band[band]++;
    }

    redundant_sky_clear = diag_desc_is_redundant_sky_clear(desc);
    if (redundant_sky_clear) {
        cost->skipped_sky_copies++;
        cost->desc_overhead_cycles_band[band] += DIAG_SKIP_DESC_OVERHEAD_CYCLES;
    } else {
        cost->bbox_pixels_1px += rows * width_1px;
        cost->bbox_pixels_band[band] += rows * width_1px;
        cost->pair_cycles_2px += rows * pairs_per_row;
        cost->pair_cycles_band[band] += rows * pairs_per_row;
        cost->desc_overhead_cycles_band[band] += DIAG_NONSKIP_DESC_OVERHEAD_CYCLES;
    }

    cost->fetch_words += desc_size / sizeof(uint32_t);
    cost->fetch_words_band[band] += desc_size / sizeof(uint32_t);
}

static void diag_add_band_fixed_cost(struct diag_cost *cost, unsigned band)
{
    uint64_t pixels;

    if (!cost || band >= VOXEL_BAND_COUNT)
        return;

    pixels = diag_band_pixels(band);
    /* ST_CACHE_INIT now clears only Z, two pixels/cycle; untouched color is
     * synthesized from sky/clear when a later blend or flush sees Z=0xFFFF. */
    cost->init_cycles[band] += (pixels + 1) / 2;
    cost->flush_words[band] += pixels;
}

static void diag_set_band_fixed_rect_cost(struct diag_cost *cost,
                                          unsigned band,
                                          unsigned x_min,
                                          unsigned x_max,
                                          unsigned y_min,
                                          unsigned y_max)
{
    uint64_t rows;
    uint64_t width;
    uint64_t pixels;

    if (!cost || band >= VOXEL_BAND_COUNT || x_min > x_max || y_min > y_max)
        return;

    width = (uint64_t)(x_max - x_min + 1);
    rows = (uint64_t)(y_max - y_min + 1);
    pixels = rows * width;
    cost->init_cycles[band] = (pixels + 1) / 2;
    cost->flush_words[band] = pixels;
}

static void diag_remove_cached_band_cost(struct diag_cost *cost, unsigned band)
{
    uint64_t pixels;
    uint64_t init_cycles;

    if (!cost || band >= VOXEL_BAND_COUNT)
        return;

    pixels = diag_band_pixels(band);
    init_cycles = (pixels + 1) / 2;

    if (cost->init_cycles[band] >= init_cycles)
        cost->init_cycles[band] -= init_cycles;
    else
        cost->init_cycles[band] = 0;

    if (cost->flush_words[band] >= pixels)
        cost->flush_words[band] -= pixels;
    else
        cost->flush_words[band] = 0;

    if (cost->band_copies >= cost->band_copies_band[band])
        cost->band_copies -= cost->band_copies_band[band];
    else
        cost->band_copies = 0;

    if (cost->textured_band_copies >= cost->textured_band_copies_band[band])
        cost->textured_band_copies -= cost->textured_band_copies_band[band];
    else
        cost->textured_band_copies = 0;

    if (cost->bbox_pixels_1px >= cost->bbox_pixels_band[band])
        cost->bbox_pixels_1px -= cost->bbox_pixels_band[band];
    else
        cost->bbox_pixels_1px = 0;

    if (cost->pair_cycles_2px >= cost->pair_cycles_band[band])
        cost->pair_cycles_2px -= cost->pair_cycles_band[band];
    else
        cost->pair_cycles_2px = 0;

    if (cost->fetch_words >= cost->fetch_words_band[band])
        cost->fetch_words -= cost->fetch_words_band[band];
    else
        cost->fetch_words = 0;

    cost->band_copies_band[band] = 0;
    cost->textured_band_copies_band[band] = 0;
    cost->bbox_pixels_band[band] = 0;
    cost->pair_cycles_band[band] = 0;
    cost->fetch_words_band[band] = 0;
    cost->desc_overhead_cycles_band[band] = 0;

}

static uint64_t diag_sum_u64(const uint64_t *values, unsigned count)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < count; i++)
        sum += values[i];
    return sum;
}

static uint64_t diag_write_slack_limited_cycles(uint64_t words)
{
    return (words * DIAG_VGA_FRAME_CYCLES +
            DIAG_VGA_WRITE_SLACK_CYCLES - 1) /
           DIAG_VGA_WRITE_SLACK_CYCLES;
}

static uint64_t diag_estimate_ready_cycles(const struct diag_cost *cost)
{
    uint64_t draw_done[VOXEL_BAND_COUNT] = {0};
    uint64_t flush_done[VOXEL_BAND_COUNT] = {0};

    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        uint64_t draw_start = 0;
        uint64_t draw_cycles;
        uint64_t flush_start;
        uint64_t flush_cycles;

        if (band > 0)
            draw_start = draw_done[band - 1];
        if (band >= 2 && draw_start < flush_done[band - 2])
            draw_start = flush_done[band - 2];

        draw_cycles = cost->init_cycles[band] +
                      cost->fetch_words_band[band] +
                      cost->desc_overhead_cycles_band[band] +
                      cost->pair_cycles_band[band];
        draw_done[band] = draw_start + draw_cycles;

        flush_start = draw_done[band];
        if (band > 0 && flush_start < flush_done[band - 1])
            flush_start = flush_done[band - 1];
        flush_cycles = diag_write_slack_limited_cycles(cost->flush_words[band]) +
                       DIAG_COPY_DRAIN_CYCLES;
        flush_done[band] = flush_start + flush_cycles;
    }

    return flush_done[VOXEL_BAND_COUNT - 1];
}

static const char *backend_mode_name(GPUBackendMode mode)
{
    switch (mode) {
    case GPU_BACKEND_HW:
        return "hw";
    case GPU_BACKEND_SOCKET:
        return "socket";
    case GPU_BACKEND_TEE:
        return "tee";
    default:
        return "unknown";
    }
}

static int parse_backend_mode(const char *value, GPUBackendMode *mode)
{
    if (!value || strcmp(value, "hw") == 0) {
        *mode = GPU_BACKEND_HW;
        return 0;
    }
    if (strcmp(value, "socket") == 0) {
        *mode = GPU_BACKEND_SOCKET;
        return 0;
    }
    if (strcmp(value, "tee") == 0) {
        *mode = GPU_BACKEND_TEE;
        return 0;
    }

    fprintf(stderr,
            "renderer: unsupported VOXEL_GPU_BACKEND=%s (expected hw, socket, tee)\n",
            value);
    return -EINVAL;
}

static int transport_needs_hw(const GPUTransport *transport)
{
    return transport->mode == GPU_BACKEND_HW || transport->mode == GPU_BACKEND_TEE;
}

static int transport_needs_socket(const GPUTransport *transport)
{
    return transport->mode == GPU_BACKEND_SOCKET || transport->mode == GPU_BACKEND_TEE;
}

static size_t transport_textured_descriptor_size(const GPUTransport *transport)
{
    (void)transport;
    return QUAD_DESC_TEXTURED_BYTES;
}

size_t gpu_transport_textured_descriptor_size(const GPUTransport *transport)
{
    return transport_textured_descriptor_size(transport);
}

static void gpu_transport_note_render_state_write(GPUTransport *transport)
{
    if (!transport)
        return;

    transport->hw_render_epoch++;
    transport->hw_render_epoch_bumps++;
    if (transport->hw_render_epoch == 0) {
        memset(transport->hw_band_cache, 0,
               sizeof(transport->hw_band_cache));
        transport->hw_render_epoch = HW_EPOCH_INITIAL;
    }
}

static void gpu_transport_note_generated_sky_palette_write(GPUTransport *transport,
                                                           uint8_t index)
{
    if (!transport || !sky_palette_index_is_valid(index))
        return;

    transport->hw_sky_epoch++;
    transport->hw_sky_epoch_bumps++;
    if (transport->hw_sky_epoch_bump_count <
        sizeof(transport->hw_sky_epoch_bump_indices)) {
        transport->hw_sky_epoch_bump_indices[
            transport->hw_sky_epoch_bump_count++] = index;
    }
    if (transport->hw_sky_epoch == 0) {
        memset(transport->hw_sky_band_epoch, 0,
               sizeof(transport->hw_sky_band_epoch));
        memset(transport->hw_band_cache, 0,
               sizeof(transport->hw_band_cache));
        transport->hw_sky_epoch = HW_EPOCH_INITIAL;
    }
}

static int gpu_transport_read_copy_target_buffer(GPUTransport *transport)
{
    struct voxel_extmem_state ext;

    if (!transport || !transport_needs_hw(transport))
        return -1;

    if (ioctl(transport->hw_fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0)
        return -1;

    transport->hw_sky_gradient_clear_enabled =
        !!(ext.ctrl & VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR);
    /* Require external memory before trusting the copy-target bit. The
     * software band-reuse skips are opt-in; diagnostics still use this state
     * to correlate software submissions with the hardware target buffer. */
    if (!(ext.ctrl & VOXEL_EXTMEM_CTRL_ENABLE))
        return -1;

    return (int)((ext.dma_status >> DIAG_EXTMEM_COPY_TARGET_SHIFT) & 1u);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *ptr = buf;

    while (len > 0) {
        ssize_t written = write(fd, ptr, len);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (written == 0)
            return -EPIPE;

        ptr += (size_t)written;
        len -= (size_t)written;
    }

    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *ptr = buf;

    while (len > 0) {
        ssize_t got = read(fd, ptr, len);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (got == 0)
            return -EPIPE;

        ptr += (size_t)got;
        len -= (size_t)got;
    }

    return 0;
}

static int quad_descriptor_size(const GPUTransport *transport,
                                const struct quad_desc *desc, size_t *size)
{
    *size = sizeof(struct quad_desc);
    if (desc->flags & QUAD_FLAG_TEX)
        *size = transport_textured_descriptor_size(transport);
    return 0;
}

static uint16_t clamp_z_i64(int64_t value)
{
    if (value < 0)
        return 0;
    if (value > UINT16_MAX)
        return UINT16_MAX;
    return (uint16_t)value;
}

static int32_t clamp_s32_i64(int64_t value)
{
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}

static void rewrite_clipped(uint8_t *dst, const uint8_t *src, size_t len,
                            int clipped_y_min, int clipped_y_max)
{
    struct quad_desc *desc;
    int delta_y;
    int64_t z0;

    memcpy(dst, src, len);
    desc = (struct quad_desc *)(void *)dst;
    delta_y = clipped_y_min - desc->y_min;

    z0 = (int64_t)desc->z0 + (int64_t)desc->dz_dy * delta_y;
    desc->z0 = clamp_z_i64(z0);
    desc->y_min = (__s16)clipped_y_min;
    desc->y_max = (__s16)clipped_y_max;

    if (desc->flags & QUAD_FLAG_TEX) {
        struct quad_desc_uv *uv =
            (struct quad_desc_uv *)(void *)(dst + sizeof(*desc));

        uv->u_over_w_0 = clamp_s32_i64((int64_t)uv->u_over_w_0 +
                                       (int64_t)uv->u_over_w_dy * delta_y);
        uv->v_over_w_0 = clamp_s32_i64((int64_t)uv->v_over_w_0 +
                                       (int64_t)uv->v_over_w_dy * delta_y);
        uv->one_over_w_0 = clamp_s32_i64((int64_t)uv->one_over_w_0 +
                                         (int64_t)uv->one_over_w_dy * delta_y);
    }
}



static int submit_hw_band_flat(GPUTransport *transport, unsigned band_index,
                               const void *buf, size_t buf_bytes,
                               unsigned flush_x_min, unsigned flush_x_max,
                               unsigned flush_y_min, unsigned flush_y_max,
                               struct diag_band_submit *diag)
{
    struct voxel_band_state band = {
        .band_index = band_index,
        .flush_x_min = flush_x_min,
        .flush_x_max = flush_x_max,
        .flush_y_min = flush_y_min,
        .flush_y_max = flush_y_max,
    };
    struct timespec t0 = {0}, t_begin = {0}, t_write = {0}, t_end = {0};
    int ret;

    if (diag) {
        memset(diag, 0, sizeof(*diag));
        diag->bytes = buf_bytes;
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }

    if (ioctl(transport->hw_fd, VOXEL_IOC_BEGIN_BAND, &band) < 0) {
        perror("ioctl(BEGIN_BAND)");
        return -errno;
    }
    if (diag)
        clock_gettime(CLOCK_MONOTONIC, &t_begin);

    if (buf_bytes > 0) {
        ret = write_all(transport->hw_fd, buf, buf_bytes);
        if (ret < 0) {
            errno = -ret;
            perror("write(descriptors)");
            return ret;
        }
    }
    if (diag)
        clock_gettime(CLOCK_MONOTONIC, &t_write);

    if (ioctl(transport->hw_fd, VOXEL_IOC_END_BAND) < 0) {
        perror("ioctl(END_BAND)");
        return -errno;
    }
    if (diag) {
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        diag->begin_ms = diag_timespec_ms(&t0, &t_begin);
        diag->write_ms = diag_timespec_ms(&t_begin, &t_write);
        diag->end_ms = diag_timespec_ms(&t_write, &t_end);
        diag->total_ms = diag_timespec_ms(&t0, &t_end);
    }

    return 0;
}

static int bin_ensure_flat_capacity(struct descriptor_bin *bin, size_t needed)
{
    if (bin->flat_capacity >= needed)
        return 0;

    size_t new_cap = bin->flat_capacity ? bin->flat_capacity : 8192;
    while (new_cap < needed)
        new_cap *= 2;

    uint8_t *p = realloc(bin->flat_buf, new_cap);
    if (!p)
        return -ENOMEM;

    bin->flat_buf = p;
    bin->flat_capacity = new_cap;
    return 0;
}

static int bin_append_descriptor(struct descriptor_bin *bin,
                                 const void *desc, size_t desc_size)
{
    int ret = bin_ensure_flat_capacity(bin, bin->flat_used + desc_size);
    if (ret < 0)
        return ret;

    memcpy(bin->flat_buf + bin->flat_used, desc, desc_size);
    bin->flat_used += desc_size;
    return 0;
}

static void bin_note_scene_rect(struct descriptor_bin *bin, unsigned band,
                                int x_min, int x_max, int y_min, int y_max)
{
    int band_y_min = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
    int local_min = y_min - band_y_min;
    int local_max = y_max - band_y_min;
    int clipped_x_min = diag_clamp_x((int16_t)x_min);
    int clipped_x_max = diag_clamp_x((int16_t)x_max);

    if (local_min < 0)
        local_min = 0;
    if (local_max >= (int)VOXEL_BAND_CACHE_HEIGHT)
        local_max = (int)VOXEL_BAND_CACHE_HEIGHT - 1;
    if (local_min > local_max || clipped_x_min > clipped_x_max)
        return;

    if (!bin->has_scene_rect) {
        bin->scene_x_min = (uint16_t)clipped_x_min;
        bin->scene_x_max = (uint16_t)clipped_x_max;
        bin->scene_y_min = (uint8_t)local_min;
        bin->scene_y_max = (uint8_t)local_max;
        bin->has_scene_rect = 1;
        return;
    }

    if (clipped_x_min < bin->scene_x_min)
        bin->scene_x_min = (uint16_t)clipped_x_min;
    if (clipped_x_max > bin->scene_x_max)
        bin->scene_x_max = (uint16_t)clipped_x_max;
    if (local_min < bin->scene_y_min)
        bin->scene_y_min = (uint8_t)local_min;
    if (local_max > bin->scene_y_max)
        bin->scene_y_max = (uint8_t)local_max;
}

/* Reset per-band bins, prime each, and zero the staging diag accumulators.
 * Shared by the legacy in-submit binning loop and the new bin-during-emit
 * path so both produce identical per-band content (primer first, then quads). */
static int reset_bins_and_prime(struct descriptor_bin bins[VOXEL_BAND_COUNT],
                                struct diag_cost *diag_cost)
{
    init_band_primers();
    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        int ret;
        bins[band].flat_used = 0;
        bins[band].has_scene_rect = 0;
        bins[band].scene_x_min = 0;
        bins[band].scene_x_max = 0;
        bins[band].scene_y_min = 0;
        bins[band].scene_y_max = 0;
        ret = bin_append_descriptor(&bins[band], &g_band_primers[band],
                                    sizeof(g_band_primers[band]));
        if (ret < 0)
            return ret;
        if (g_diag_bbox && diag_cost) {
            diag_add_band_fixed_cost(diag_cost, band);
            diag_add_band_descriptor(diag_cost, band, &g_band_primers[band],
                                     g_band_primers[band].y_min,
                                     g_band_primers[band].y_max,
                                     sizeof(g_band_primers[band]));
        }
    }
    return 0;
}

/* Route one already-built descriptor into the per-band bins. Returns 0 on
 * success (including the y-fully-off-screen and redundant-sky-clear skips),
 * negative errno on allocation failure. Caller-owned diag accumulators are
 * updated when g_diag_bbox is set. Pulled out of submit_hw_binned so both
 * the legacy second-pass loop and the new gpu_transport_bin_descriptor
 * front-loaded path share one implementation. */
static int bin_one_descriptor(GPUTransport *transport,
                              struct descriptor_bin bins[VOXEL_BAND_COUNT],
                              const void *desc_bytes, size_t desc_size,
                              struct diag_cost *diag_cost,
                              int16_t *diag_x_min, int16_t *diag_x_max,
                              int16_t *diag_y_min, int16_t *diag_y_max)
{
    const struct quad_desc *desc = desc_bytes;
    int x_min, x_max;
    int y_min, y_max;
    unsigned first_band, last_band;

    if (g_diag_bbox && diag_cost) {
        diag_cost->desc_count++;
        if (diag_x_min && desc->x_min < *diag_x_min) *diag_x_min = desc->x_min;
        if (diag_x_max && desc->x_max > *diag_x_max) *diag_x_max = desc->x_max;
        if (diag_y_min && desc->y_min < *diag_y_min) *diag_y_min = desc->y_min;
        if (diag_y_max && desc->y_max > *diag_y_max) *diag_y_max = desc->y_max;
    }

    x_min = desc->x_min;
    x_max = desc->x_max;
    y_min = desc->y_min;
    y_max = desc->y_max;
    if (y_max < 0 || y_min >= (int)VOXEL_RENDER_HEIGHT || y_min > y_max)
        return 0;
    if (y_min < 0)
        y_min = 0;
    if (y_max >= (int)VOXEL_RENDER_HEIGHT)
        y_max = (int)VOXEL_RENDER_HEIGHT - 1;

    first_band = (unsigned)y_min / VOXEL_BAND_CACHE_HEIGHT;
    last_band = (unsigned)y_max / VOXEL_BAND_CACHE_HEIGHT;
    if (last_band >= VOXEL_BAND_COUNT)
        last_band = VOXEL_BAND_COUNT - 1;

    if (transport->hw_sky_gradient_clear_enabled &&
        diag_desc_is_redundant_sky_clear(desc)) {
        if (g_diag_bbox && diag_cost)
            diag_count_omitted_sky_clear(diag_cost, first_band, last_band);
        return 0;
    }

    /* Fast path: descriptor fits inside one band and didn't need y-clipping. */
    if (first_band == last_band &&
        (int)desc->y_min == y_min && (int)desc->y_max == y_max) {
        struct descriptor_bin *bin = &bins[first_band];
        int ret = bin_append_descriptor(bin, desc_bytes, desc_size);
        if (ret < 0)
            return ret;
        bin_note_scene_rect(bin, first_band, x_min, x_max, y_min, y_max);
        if (g_diag_bbox && diag_cost)
            diag_add_band_descriptor(diag_cost, first_band, desc,
                                     y_min, y_max, desc_size);
        return 0;
    }

    /* Slow path: rewrite z0/uv into per-band flat_buf. */
    for (unsigned band = first_band; band <= last_band; band++) {
        struct descriptor_bin *bin = &bins[band];
        int band_y_min = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
        int band_y_max = band_y_min + (int)VOXEL_BAND_CACHE_HEIGHT - 1;
        int clipped_y_min = y_min > band_y_min ? y_min : band_y_min;
        int clipped_y_max = y_max < band_y_max ? y_max : band_y_max;
        uint8_t *dst;
        int ret = bin_ensure_flat_capacity(bin, bin->flat_used + desc_size);
        if (ret < 0)
            return ret;
        dst = bin->flat_buf + bin->flat_used;
        rewrite_clipped(dst, desc_bytes, desc_size,
                        clipped_y_min, clipped_y_max);
        bin->flat_used += desc_size;
        bin_note_scene_rect(bin, band, x_min, x_max,
                            clipped_y_min, clipped_y_max);
        if (g_diag_bbox && diag_cost)
            diag_add_band_descriptor(diag_cost, band, desc,
                                     clipped_y_min, clipped_y_max,
                                     desc_size);
    }
    return 0;
}

void gpu_transport_begin_descriptors(GPUTransport *transport)
{
    struct descriptor_bin *bins;

    if (!transport || !transport_needs_hw(transport))
        return;

    bins = g_bins_pool[g_main_bin_set];

    memset(&g_staged_diag_cost, 0, sizeof(g_staged_diag_cost));
    g_staged_diag_x_min = INT16_MAX;
    g_staged_diag_x_max = INT16_MIN;
    g_staged_diag_y_min = INT16_MAX;
    g_staged_diag_y_max = INT16_MIN;
    g_staged_desc_count = 0;
    if (g_diag_bbox || g_diag_cache) {
        g_staged_diag_frame_index = g_diag_frame_count;
        g_staged_diag_log_frame = ((g_diag_frame_count++ % 60) == 0);
    } else {
        g_staged_diag_frame_index = 0;
        g_staged_diag_log_frame = 0;
    }

    if (reset_bins_and_prime(bins, g_diag_bbox ? &g_staged_diag_cost : NULL) < 0) {
        /* OOM: leave staged_active=0 so submit_hw_binned re-runs the
         * legacy binning path on the contiguous stream. */
        transport->staged_active = 0;
        return;
    }
    transport->staged_active = 1;
}

int gpu_transport_bin_descriptor(GPUTransport *transport,
                                 const void *desc_bytes, size_t desc_size)
{
    int ret;

    if (!transport || !transport->staged_active)
        return 0;
    g_staged_desc_count++;
    ret = bin_one_descriptor(transport, g_bins_pool[g_main_bin_set],
                             desc_bytes, desc_size,
                             &g_staged_diag_cost,
                             &g_staged_diag_x_min, &g_staged_diag_x_max,
                             &g_staged_diag_y_min, &g_staged_diag_y_max);
    if (ret < 0)
        transport->staged_active = 0;
    return ret;
}

static int submit_hw_prebinned(GPUTransport *transport,
                               struct descriptor_bin bins[VOXEL_BAND_COUNT],
                               struct diag_cost *diag_cost,
                               int16_t diag_x_min, int16_t diag_x_max,
                               int16_t diag_y_min, int16_t diag_y_max,
                               int diag_log_frame,
                               unsigned diag_frame_index,
                               unsigned diag_quad_count,
                               const struct timespec *t_start,
                               const struct timespec *t_binned)
{
    struct diag_band_submit diag_bands[VOXEL_BAND_COUNT];
    struct timespec t_submit = {0};
    int copy_target_buffer = -1;
    int have_t_submit = 0;
    int ret = 0;

    memset(diag_bands, 0, sizeof(diag_bands));

    copy_target_buffer = gpu_transport_read_copy_target_buffer(transport);

    if (g_diag_cache) {
        fprintf(stderr,
                "DIAG_CACHE frame=%u ct=%d reuse=%d render_epoch=%u sky_epoch=%u\n",
                diag_frame_index, copy_target_buffer,
                g_hw_band_reuse,
                transport->hw_render_epoch, transport->hw_sky_epoch);
    }

    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        const int primer_only =
            bins[band].flat_used == sizeof(g_band_primers[band]);
        const uint64_t band_hash =
            hash_band_bytes(bins[band].flat_buf, bins[band].flat_used);
        struct hw_band_cache_entry *band_cache =
            (copy_target_buffer >= 0) ?
                &transport->hw_band_cache[copy_target_buffer][band] : NULL;
        unsigned flush_y_min = 0;
        unsigned flush_y_max = band_visible_rows(band);
        int full_band_flush = 1;

        if (flush_y_max > 0)
            flush_y_max--;
        else
            flush_y_max = 0;

        if (g_hw_band_reuse && band_cache &&
            band_cache->render_epoch == transport->hw_render_epoch &&
            (!transport->hw_sky_gradient_clear_enabled ||
             band_cache->sky_epoch == transport->hw_sky_epoch) &&
            band_cache->bytes == bins[band].flat_used &&
            band_cache->hash == band_hash) {
            if (g_diag_bbox) {
                diag_remove_cached_band_cost(diag_cost, band);
                diag_cost->cached_exact_band_reuses++;
            }
            if (g_diag_cache) {
                fprintf(stderr,
                        "DIAG_CACHE   b%u=HIT  bytes=%zu hash=0x%016llx\n",
                        band, bins[band].flat_used,
                        (unsigned long long)band_hash);
            }
            continue;
        }

        if (g_hw_band_reuse &&
            primer_only && copy_target_buffer >= 0 &&
            transport->hw_sky_band_epoch[copy_target_buffer][band] ==
                transport->hw_sky_epoch) {
            if (g_diag_bbox) {
                diag_remove_cached_band_cost(diag_cost, band);
                diag_cost->cached_sky_band_reuses++;
            }
            if (band_cache) {
                band_cache->hash = band_hash;
                band_cache->bytes = bins[band].flat_used;
                band_cache->render_epoch = transport->hw_render_epoch;
                band_cache->sky_epoch = transport->hw_sky_epoch;
                band_cache->has_scene_rect = 0;
                band_cache->scene_x_min = 0;
                band_cache->scene_x_max = 0;
                band_cache->scene_y_min = 0;
                band_cache->scene_y_max = 0;
            }
            if (g_diag_cache) {
                fprintf(stderr,
                        "DIAG_CACHE   b%u=SKY  bytes=%zu hash=0x%016llx\n",
                        band, bins[band].flat_used,
                        (unsigned long long)band_hash);
            }
            continue;
        }

        /* Primer-only cache-miss: BEGIN_BAND fills the cache via the RTL
         * sky-gradient-clear, so we don't need any descriptor writes. The
         * primer existed to absorb a first-pixel pipeline glitch when real
         * descriptors rasterize; with zero descriptors pushed, there is no
         * glitch to absorb. Skipping the primer write saves a write_all()
         * syscall + one descriptor's FIFO traversal per primer-only miss. */
        const void *submit_buf =
            primer_only ? NULL : bins[band].flat_buf;
        size_t submit_bytes =
            primer_only ? 0 : bins[band].flat_used;
        unsigned flush_x_min = 0;
        unsigned flush_x_max = VOXEL_RENDER_WIDTH - 1;

        if (!primer_only && bins[band].has_scene_rect && band_cache &&
            (!transport->hw_sky_gradient_clear_enabled ||
             band_cache->sky_epoch == transport->hw_sky_epoch)) {
            flush_x_min = bins[band].scene_x_min;
            flush_x_max = bins[band].scene_x_max;
            flush_y_min = bins[band].scene_y_min;
            flush_y_max = bins[band].scene_y_max;
            if (band_cache->has_scene_rect) {
                if (band_cache->scene_x_min < flush_x_min)
                    flush_x_min = band_cache->scene_x_min;
                if (band_cache->scene_x_max > flush_x_max)
                    flush_x_max = band_cache->scene_x_max;
                if (band_cache->scene_y_min < flush_y_min)
                    flush_y_min = band_cache->scene_y_min;
                if (band_cache->scene_y_max > flush_y_max)
                    flush_y_max = band_cache->scene_y_max;
            }
            full_band_flush = 0;
        }

        if ((flush_x_max - flush_x_min + 1) > HW_RECT_STRIDE_MAX_WIDTH) {
            flush_x_min = 0;
            flush_x_max = VOXEL_RENDER_WIDTH - 1;
        }

        if (g_diag_bbox && !full_band_flush)
            diag_set_band_fixed_rect_cost(diag_cost, band,
                                          flush_x_min, flush_x_max,
                                          flush_y_min, flush_y_max);

        ret = submit_hw_band_flat(transport, band,
                                  submit_buf, submit_bytes,
                                  flush_x_min, flush_x_max,
                                  flush_y_min, flush_y_max,
                                  diag_log_frame ? &diag_bands[band] : NULL);
        if (ret < 0)
            goto done;

        if (g_diag_cache) {
            struct voxel_extmem_state ext_post;
            unsigned ct_post = 0, fa_post = 0, cbi_post = 0,
                     dv_post = 0, ccp_post = 0, late_post = 0;
            uint32_t dma_post = 0;
            if (ioctl(transport->hw_fd, VOXEL_IOC_GET_EXTMEM, &ext_post) == 0) {
                dma_post = ext_post.dma_status;
                late_post = dma_post >> 16;
                ct_post  = (dma_post >> 11) & 1u;
                fa_post  = (dma_post >> 8)  & 1u;
                ccp_post = (dma_post >> 10) & 1u;
                dv_post  = (dma_post >> 6)  & 1u;
                cbi_post = (dma_post >> 13) & 7u;
            }
            fprintf(stderr,
                    "DIAG_CACHE   b%u=MISS bytes=%zu hash=0x%016llx primer=%d "
                    "post: dma=0x%08x late=%u ct=%u fa=%u ccp=%u dv=%u cbi=%u\n",
                    band, bins[band].flat_used,
                    (unsigned long long)band_hash, primer_only,
                    dma_post, late_post, ct_post, fa_post, ccp_post,
                    dv_post, cbi_post);
        }

        if (copy_target_buffer >= 0) {
            transport->hw_sky_band_epoch[copy_target_buffer][band] =
                primer_only ? transport->hw_sky_epoch : 0;
            if (band_cache) {
                band_cache->hash = band_hash;
                band_cache->bytes = bins[band].flat_used;
                band_cache->render_epoch = transport->hw_render_epoch;
                band_cache->sky_epoch = transport->hw_sky_epoch;
                band_cache->has_scene_rect =
                    !primer_only && bins[band].has_scene_rect;
                band_cache->scene_x_min = bins[band].scene_x_min;
                band_cache->scene_x_max = bins[band].scene_x_max;
                band_cache->scene_y_min = bins[band].scene_y_min;
                band_cache->scene_y_max = bins[band].scene_y_max;
            }
        }
    }

    if (g_diag_bbox) {
        clock_gettime(CLOCK_MONOTONIC, &t_submit);
        have_t_submit = 1;
    }

done:
    if (g_diag_bbox && have_t_submit && diag_log_frame) {
        uint64_t init_cycles = diag_sum_u64(diag_cost->init_cycles,
                                            VOXEL_BAND_COUNT);
        uint64_t overhead_cycles =
            diag_sum_u64(diag_cost->desc_overhead_cycles_band,
                         VOXEL_BAND_COUNT);
        uint64_t flush_words = diag_sum_u64(diag_cost->flush_words,
                                            VOXEL_BAND_COUNT);
        uint64_t flush_wall_cycles =
            diag_write_slack_limited_cycles(flush_words);
        uint64_t ready_cycles = diag_estimate_ready_cycles(diag_cost);
        uint64_t vsync_slots =
            (ready_cycles + DIAG_VGA_FRAME_CYCLES - 1) /
            DIAG_VGA_FRAME_CYCLES;
        double ms_bin = (t_binned->tv_sec - t_start->tv_sec) * 1000.0 +
                        (t_binned->tv_nsec - t_start->tv_nsec) / 1e6;
        double ms_submit = (t_submit.tv_sec - t_binned->tv_sec) * 1000.0 +
                           (t_submit.tv_nsec - t_binned->tv_nsec) / 1e6;

        if (vsync_slots == 0)
            vsync_slots = 1;

        fprintf(stderr,
                "renderer: HW mode [flat buf, %u quads, bbox: (%d,%d)-(%d,%d)] bin=%5.2fms bands=%5.2fms\n",
                diag_quad_count,
                diag_x_min, diag_y_min, diag_x_max, diag_y_max,
                ms_bin, ms_submit);
        fprintf(stderr,
                "renderer: band detail "
                "b0=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b1=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b2=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b3=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b4=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b5=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b6=%5.2f(%5.2f/%5.2f/%5.2f,%zuB) "
                "b7=%5.2f(%5.2f/%5.2f/%5.2f,%zuB)\n",
                diag_bands[0].total_ms, diag_bands[0].begin_ms,
                diag_bands[0].write_ms, diag_bands[0].end_ms,
                diag_bands[0].bytes,
                diag_bands[1].total_ms, diag_bands[1].begin_ms,
                diag_bands[1].write_ms, diag_bands[1].end_ms,
                diag_bands[1].bytes,
                diag_bands[2].total_ms, diag_bands[2].begin_ms,
                diag_bands[2].write_ms, diag_bands[2].end_ms,
                diag_bands[2].bytes,
                diag_bands[3].total_ms, diag_bands[3].begin_ms,
                diag_bands[3].write_ms, diag_bands[3].end_ms,
                diag_bands[3].bytes,
                diag_bands[4].total_ms, diag_bands[4].begin_ms,
                diag_bands[4].write_ms, diag_bands[4].end_ms,
                diag_bands[4].bytes,
                diag_bands[5].total_ms, diag_bands[5].begin_ms,
                diag_bands[5].write_ms, diag_bands[5].end_ms,
                diag_bands[5].bytes,
                diag_bands[6].total_ms, diag_bands[6].begin_ms,
                diag_bands[6].write_ms, diag_bands[6].end_ms,
                diag_bands[6].bytes,
                diag_bands[7].total_ms, diag_bands[7].begin_ms,
                diag_bands[7].write_ms, diag_bands[7].end_ms,
                diag_bands[7].bytes);
        fprintf(stderr,
                "renderer: calc desc=%llu copies=%llu tex=%llu sky_skip=%llu "
                "sky_band_reuse=%llu band_reuse=%llu "
                "bbox1=%5.2fms bbox2=%5.2fms save=%5.2fms init=%5.2fms "
                "fetch=%5.2fms overhead=%5.2fms flush_raw=%5.2fms "
                "flush_slack=%5.2fms "
                "ideal_ready=%5.2fms vsync_floor=%5.2fms slots=%llu\n",
                (unsigned long long)diag_cost->desc_count,
                (unsigned long long)diag_cost->band_copies,
                (unsigned long long)diag_cost->textured_band_copies,
                (unsigned long long)diag_cost->skipped_sky_copies,
                (unsigned long long)diag_cost->cached_sky_band_reuses,
                (unsigned long long)diag_cost->cached_exact_band_reuses,
                diag_cycles_ms(diag_cost->bbox_pixels_1px),
                diag_cycles_ms(diag_cost->pair_cycles_2px),
                diag_cycles_ms(diag_cost->bbox_pixels_1px -
                               diag_cost->pair_cycles_2px),
                diag_cycles_ms(init_cycles),
                diag_cycles_ms(diag_cost->fetch_words),
                diag_cycles_ms(overhead_cycles),
                diag_cycles_ms(flush_words),
                diag_cycles_ms(flush_wall_cycles),
                diag_cycles_ms(ready_cycles),
                diag_cycles_ms(vsync_slots * DIAG_VGA_FRAME_CYCLES),
                (unsigned long long)vsync_slots);
        if (transport->hw_fd >= 0) {
            /* HW perf counters reset on the next FLIP (writedata[1] to
             * ADDR_CONTROL), so read them BEFORE gpu_transport_flip(). At
             * 50 MHz, 50000 cycles = 1 ms. Counters can overlap (bg flush
             * runs in parallel with raster + ping-pong init), so columns
             * don't sum to wall-clock — they reveal the breakdown of what
             * was happening during the frame. See PROJECT_NOTES.md. */
            struct voxel_perf_counters_v2 perf2;
            int have_perf2 = (ioctl(transport->hw_fd,
                                    VOXEL_IOC_GET_PERF2, &perf2) == 0);
            int have_perf = have_perf2;
            if (!have_perf2) {
                struct voxel_perf_counters perf1;
                memset(&perf2, 0, sizeof(perf2));
                if (ioctl(transport->hw_fd, VOXEL_IOC_GET_PERF, &perf1) == 0) {
                    perf2.base = perf1;
                    have_perf = 1;
                } else {
                    memset(&perf2.base, 0, sizeof(perf2.base));
                }
            }
            if (have_perf) {
                const double cyc_per_ms = 50000.0;
                fprintf(stderr,
                        "renderer: hw_perf draw_act=%5.2fms draw_idle=%5.2fms "
                        "flush_act=%5.2fms flush_stall=%5.2fms "
                        "init=%5.2fms load=%5.2fms "
                        "(draw_busy=%.0f%% flush_busy=%.0f%%)\n",
                        perf2.base.draw_active  / cyc_per_ms,
                        perf2.base.draw_idle    / cyc_per_ms,
                        perf2.base.flush_active / cyc_per_ms,
                        perf2.base.flush_stall  / cyc_per_ms,
                        perf2.base.init         / cyc_per_ms,
                        perf2.base.load         / cyc_per_ms,
                        (perf2.base.draw_active + perf2.base.draw_idle) ?
                            100.0 * perf2.base.draw_active /
                                (double)(perf2.base.draw_active +
                                         perf2.base.draw_idle) : 0.0,
                        (perf2.base.flush_active + perf2.base.flush_stall) ?
                            100.0 * perf2.base.flush_active /
                                (double)(perf2.base.flush_active +
                                         perf2.base.flush_stall) : 0.0);
                if (have_perf2) {
                    fprintf(stderr,
                            "renderer: hw_flush_wait load=%5.2fms fifo=%5.2fms "
                            "data=%5.2fms drain=%5.2fms\n",
                            perf2.flush_wait_load  / cyc_per_ms,
                            perf2.flush_wait_fifo  / cyc_per_ms,
                            perf2.flush_wait_data  / cyc_per_ms,
                            perf2.flush_wait_drain / cyc_per_ms);
                }
            }
        }
        {
            char idx_buf[64];
            size_t off = 0;
            uint32_t n = transport->hw_sky_epoch_bump_count;
            if (n > sizeof(transport->hw_sky_epoch_bump_indices))
                n = sizeof(transport->hw_sky_epoch_bump_indices);
            for (uint32_t i = 0; i < n && off + 4 < sizeof(idx_buf); i++) {
                int w = snprintf(idx_buf + off, sizeof(idx_buf) - off,
                                 i ? ",%u" : "%u",
                                 (unsigned)transport->hw_sky_epoch_bump_indices[i]);
                if (w < 0)
                    break;
                off += (size_t)w;
            }
            if (off == 0) {
                idx_buf[0] = '-';
                idx_buf[1] = '\0';
            }
            fprintf(stderr,
                    "renderer: epoch_bumps render=%u sky=%u sky_idx=[%s] "
                    "copy_target=%d render_epoch=%u sky_epoch=%u\n",
                    (unsigned)transport->hw_render_epoch_bumps,
                    (unsigned)transport->hw_sky_epoch_bumps,
                    idx_buf,
                    copy_target_buffer,
                    (unsigned)transport->hw_render_epoch,
                    (unsigned)transport->hw_sky_epoch);
        }
        g_diag_log_flip_next = 1;
    }

    transport->hw_render_epoch_bumps = 0;
    transport->hw_sky_epoch_bumps = 0;
    transport->hw_sky_epoch_bump_count = 0;
    return ret;
}

static int submit_hw_binned(GPUTransport *transport, const void *descriptors,
                            size_t descriptor_bytes)
{
    const uint8_t *stream = descriptors;
    size_t offset = 0;
    int ret = 0;
    int16_t diag_x_min = INT16_MAX, diag_x_max = INT16_MIN;
    int16_t diag_y_min = INT16_MAX, diag_y_max = INT16_MIN;
    struct diag_cost diag_cost = {0};
    int diag_log_frame = 0;
    unsigned diag_frame_index = 0;
    const int staged = transport->staged_active;
    struct descriptor_bin *bins = g_bins_pool[g_main_bin_set];

    struct timespec t_start = {0}, t_binned = {0};

    if (g_diag_bbox)
        clock_gettime(CLOCK_MONOTONIC, &t_start);
    if (staged) {
        /* Bin-during-emit path: the renderer already populated the current
         * bin set via gpu_transport_bin_descriptor. Pull staged diag
         * accumulators in so the per-frame log line still has the same data. */
        diag_cost = g_staged_diag_cost;
        diag_x_min = g_staged_diag_x_min;
        diag_x_max = g_staged_diag_x_max;
        diag_y_min = g_staged_diag_y_min;
        diag_y_max = g_staged_diag_y_max;
        diag_log_frame = g_staged_diag_log_frame;
        diag_frame_index = g_staged_diag_frame_index;
        transport->staged_active = 0;
        goto skip_binning;
    }

    if (g_diag_bbox || g_diag_cache) {
        diag_frame_index = g_diag_frame_count;
        diag_log_frame = ((g_diag_frame_count++ % 60) == 0);
    }

    ret = reset_bins_and_prime(bins, g_diag_bbox ? &diag_cost : NULL);
    if (ret < 0)
        return ret;

    if (descriptor_bytes == 0) {
        if (g_diag_bbox)
            clock_gettime(CLOCK_MONOTONIC, &t_binned);
        return submit_hw_prebinned(transport, bins, &diag_cost,
                                   diag_x_min, diag_x_max,
                                   diag_y_min, diag_y_max,
                                   diag_log_frame, diag_frame_index, 0,
                                   &t_start, &t_binned);
    }

    while (offset < descriptor_bytes) {
        const struct quad_desc *desc;
        size_t desc_size;

        if (descriptor_bytes - offset < sizeof(struct quad_desc)) {
            ret = -EINVAL;
            goto done;
        }

        desc = (const struct quad_desc *)(const void *)(stream + offset);
        quad_descriptor_size(transport, desc, &desc_size);
        if (descriptor_bytes - offset < desc_size) {
            ret = -EINVAL;
            goto done;
        }

        ret = bin_one_descriptor(transport, bins, stream + offset, desc_size,
                                 &diag_cost,
                                 &diag_x_min, &diag_x_max,
                                 &diag_y_min, &diag_y_max);
        if (ret < 0)
            goto done;

        offset += desc_size;
    }

done:
    if (ret < 0) {
        transport->hw_render_epoch_bumps = 0;
        transport->hw_sky_epoch_bumps = 0;
        transport->hw_sky_epoch_bump_count = 0;
        return ret;
    }

skip_binning:
    if (g_diag_bbox)
        clock_gettime(CLOCK_MONOTONIC, &t_binned);
    return submit_hw_prebinned(transport, bins, &diag_cost,
                               diag_x_min, diag_x_max,
                               diag_y_min, diag_y_max,
                               diag_log_frame,
                               diag_frame_index,
                               staged ? (unsigned)g_staged_desc_count :
                                        (unsigned)(descriptor_bytes /
                                                   sizeof(struct quad_desc)),
                               &t_start, &t_binned);
}

static void log_extmem_state(GPUTransport *transport, int debug_enabled)
{
    struct voxel_extmem_state ext;

    if (ioctl(transport->hw_fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0) {
        perror("ioctl(GET_EXTMEM)");
        return;
    }

    transport->hw_sky_gradient_clear_enabled =
        !!(ext.ctrl & VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR);

    if (debug_enabled) {
        fprintf(stderr,
                "renderer: extmem ctrl=0x%08x front=0x%08x back=0x%08x "
                "stride=%u tile=0x%08x dma=0x%08x (sw expects stride=%u)\n",
                ext.ctrl, ext.front_base, ext.back_base, ext.stride_bytes,
                ext.tile_cfg, ext.dma_status, VOXEL_RENDER_STRIDE);
    }

    if (ext.stride_bytes != VOXEL_RENDER_STRIDE) {
        fprintf(stderr,
                "renderer: WARNING extmem stride %u != expected %u — "
                "left-edge pixels likely show wrong color\n",
                ext.stride_bytes, VOXEL_RENDER_STRIDE);
    }
    if ((ext.front_base | ext.back_base) & 1u) {
        fprintf(stderr,
                "renderer: WARNING extmem buffer base not 16-bit aligned — "
                "RGB565 byte order will be swapped\n");
    }
}

static int connect_socket_path(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (!path || !path[0])
        return -EINVAL;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    return fd;
}

static int socket_request(GPUTransport *transport, uint16_t opcode,
                          const void *payload, uint32_t payload_size,
                          void *reply_payload, uint32_t *reply_payload_size)
{
    struct vgpu_socket_header header;
    struct vgpu_socket_reply reply;
    uint32_t expected_payload = reply_payload_size ? *reply_payload_size : 0;
    int ret;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, SOCKET_MAGIC, sizeof(header.magic));
    header.version = VGPU_SOCKET_VERSION;
    header.opcode = opcode;
    header.payload_size = payload_size;

    ret = write_all(transport->socket_fd, &header, sizeof(header));
    if (ret < 0)
        return ret;

    if (payload_size > 0) {
        ret = write_all(transport->socket_fd, payload, payload_size);
        if (ret < 0)
            return ret;
    }

    ret = read_all(transport->socket_fd, &reply, sizeof(reply));
    if (ret < 0)
        return ret;

    if (memcmp(reply.magic, SOCKET_MAGIC, sizeof(reply.magic)) != 0 ||
        reply.version != VGPU_SOCKET_VERSION)
        return -EPROTO;

    if (reply.payload_size > 0) {
        if (!reply_payload || reply.payload_size > expected_payload) {
            uint8_t discard[256];
            uint32_t remaining = reply.payload_size;

            while (remaining > 0) {
                size_t chunk = remaining;
                if (chunk > sizeof(discard))
                    chunk = sizeof(discard);

                ret = read_all(transport->socket_fd, discard, chunk);
                if (ret < 0)
                    return ret;
                remaining -= (uint32_t)chunk;
            }
            return -EMSGSIZE;
        }

        ret = read_all(transport->socket_fd, reply_payload, reply.payload_size);
        if (ret < 0)
            return ret;
    }

    if (reply_payload_size)
        *reply_payload_size = reply.payload_size;

    return reply.status;
}

static int read_debug_enabled_local(void)
{
    const char *value = getenv("DEBUG");

    if (!value || value[0] == '\0')
        return 0;
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "no") == 0)
        return 0;
    return 1;
}

static int env_flag_enabled(const char *value)
{
    if (!value || value[0] == '\0')
        return 0;
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "no") == 0)
        return 0;
    return 1;
}

static int gpu_transport_flip_hw_only(GPUTransport *transport);

static int gpu_transport_wait_pending_flip(GPUTransport *transport)
{
    int ret = 0;

    if (!transport_needs_hw(transport) || !transport->hw_flip_pending)
        return 0;

    {
        struct timespec t0 = {0}, t1 = {0};

        if (g_diag_bbox && g_diag_log_flip_next)
            clock_gettime(CLOCK_MONOTONIC, &t0);

        if (ioctl(transport->hw_fd, VOXEL_IOC_WAIT_FLIP) < 0) {
            perror("ioctl(WAIT_FLIP)");
            ret = -errno;
        } else {
            transport->hw_flip_pending = 0;
        }

        if (g_diag_bbox && g_diag_log_flip_next) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms_wait = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                             (t1.tv_nsec - t0.tv_nsec) / 1e6;
            fprintf(stderr, "renderer: FLIP wait=%5.2fms\n", ms_wait);
            g_diag_log_flip_next = 0;
        }
    }

    return ret;
}

static void *gpu_transport_pipeline_worker(void *arg)
{
    GPUTransport *transport = arg;

    for (;;) {
        struct pipeline_submit_job job;
        struct timespec t_start = {0}, t_binned = {0};
        int ret;

        pthread_mutex_lock(&transport->pipeline_mutex);
        while (!transport->pipeline_shutdown &&
               !transport->pipeline_job_ready) {
            pthread_cond_wait(&transport->pipeline_cond,
                              &transport->pipeline_mutex);
        }
        if (transport->pipeline_shutdown &&
            !transport->pipeline_job_ready) {
            pthread_mutex_unlock(&transport->pipeline_mutex);
            return NULL;
        }

        job = *transport->pipeline_job;
        transport->pipeline_job_ready = 0;
        transport->pipeline_job_running = 1;
        pthread_mutex_unlock(&transport->pipeline_mutex);

        if (g_diag_bbox) {
            clock_gettime(CLOCK_MONOTONIC, &t_start);
            t_binned = t_start;
        }

        ret = submit_hw_prebinned(transport, g_bins_pool[job.bin_set],
                                  &job.diag_cost,
                                  job.diag_x_min, job.diag_x_max,
                                  job.diag_y_min, job.diag_y_max,
                                  job.diag_log_frame,
                                  job.diag_frame_index,
                                  (unsigned)job.desc_count,
                                  &t_start, &t_binned);
        if (ret == 0)
            ret = gpu_transport_flip_hw_only(transport);

        pthread_mutex_lock(&transport->pipeline_mutex);
        transport->pipeline_ret = ret;
        transport->pipeline_job_running = 0;
        pthread_cond_broadcast(&transport->pipeline_cond);
        pthread_mutex_unlock(&transport->pipeline_mutex);
    }
}

static int gpu_transport_pipeline_wait_idle(GPUTransport *transport)
{
    int ret;

    if (!transport || !transport->pipeline_enabled)
        return 0;

    pthread_mutex_lock(&transport->pipeline_mutex);
    while (transport->pipeline_job_ready ||
           transport->pipeline_job_running) {
        pthread_cond_wait(&transport->pipeline_cond,
                          &transport->pipeline_mutex);
    }
    ret = transport->pipeline_ret;
    transport->pipeline_ret = 0;
    transport->pipeline_flip_owned = 0;
    pthread_mutex_unlock(&transport->pipeline_mutex);
    return ret;
}

static int gpu_transport_pipeline_queue_submit(GPUTransport *transport)
{
    int ret;

    if (!transport || !transport->pipeline_enabled ||
        !transport->pipeline_thread_started ||
        !transport->pipeline_job)
        return -EINVAL;

    ret = gpu_transport_pipeline_wait_idle(transport);
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&transport->pipeline_mutex);
    transport->pipeline_job->bin_set = g_main_bin_set;
    transport->pipeline_job->diag_cost = g_staged_diag_cost;
    transport->pipeline_job->diag_x_min = g_staged_diag_x_min;
    transport->pipeline_job->diag_x_max = g_staged_diag_x_max;
    transport->pipeline_job->diag_y_min = g_staged_diag_y_min;
    transport->pipeline_job->diag_y_max = g_staged_diag_y_max;
    transport->pipeline_job->diag_log_frame = g_staged_diag_log_frame;
    transport->pipeline_job->diag_frame_index = g_staged_diag_frame_index;
    transport->pipeline_job->desc_count = g_staged_desc_count;
    transport->pipeline_ret = 0;
    transport->pipeline_job_ready = 1;
    transport->pipeline_flip_owned = 1;
    transport->staged_active = 0;
    g_main_bin_set ^= 1;
    pthread_cond_signal(&transport->pipeline_cond);
    pthread_mutex_unlock(&transport->pipeline_mutex);

    return 0;
}

static int gpu_transport_pipeline_start(GPUTransport *transport,
                                        int debug_enabled)
{
    int ret;

    transport->pipeline_job = calloc(1, sizeof(*transport->pipeline_job));
    if (!transport->pipeline_job)
        return -ENOMEM;

    ret = pthread_mutex_init(&transport->pipeline_mutex, NULL);
    if (ret != 0) {
        free(transport->pipeline_job);
        transport->pipeline_job = NULL;
        return -ret;
    }

    ret = pthread_cond_init(&transport->pipeline_cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&transport->pipeline_mutex);
        free(transport->pipeline_job);
        transport->pipeline_job = NULL;
        return -ret;
    }

    transport->pipeline_enabled = 1;
    ret = pthread_create(&transport->pipeline_thread, NULL,
                         gpu_transport_pipeline_worker, transport);
    if (ret != 0) {
        transport->pipeline_enabled = 0;
        pthread_cond_destroy(&transport->pipeline_cond);
        pthread_mutex_destroy(&transport->pipeline_mutex);
        free(transport->pipeline_job);
        transport->pipeline_job = NULL;
        return -ret;
    }

    transport->pipeline_thread_started = 1;
    if (debug_enabled)
        fprintf(stderr,
                "renderer: VOXEL_PIPELINE_FRAMES enabled (HW submit+flip worker)\n");
    return 0;
}

static void gpu_transport_pipeline_stop(GPUTransport *transport)
{
    if (!transport)
        return;

    if (transport->pipeline_thread_started) {
        (void)gpu_transport_pipeline_wait_idle(transport);
        pthread_mutex_lock(&transport->pipeline_mutex);
        transport->pipeline_shutdown = 1;
        pthread_cond_signal(&transport->pipeline_cond);
        pthread_mutex_unlock(&transport->pipeline_mutex);
        pthread_join(transport->pipeline_thread, NULL);
        transport->pipeline_thread_started = 0;
    }

    if (transport->pipeline_enabled || transport->pipeline_job) {
        pthread_cond_destroy(&transport->pipeline_cond);
        pthread_mutex_destroy(&transport->pipeline_mutex);
        free(transport->pipeline_job);
        transport->pipeline_job = NULL;
        transport->pipeline_enabled = 0;
    }
}

GPUTransport *gpu_transport_open(void)
{
    const char *backend_env = getenv("VOXEL_GPU_BACKEND");
    const char *socket_env = getenv("VOXEL_GPU_SOCKET_PATH");
    int debug_enabled = read_debug_enabled_local();
    GPUTransport *transport = calloc(1, sizeof(*transport));
    int ret;

    if (!transport)
        return NULL;

    transport->hw_fd = -1;
    transport->socket_fd = -1;
    transport->hw_async_flip_supported = 1;
    transport->hw_render_epoch = HW_EPOCH_INITIAL;
    transport->hw_sky_epoch = HW_EPOCH_INITIAL;

    ret = parse_backend_mode(backend_env, &transport->mode);
    if (ret < 0) {
        free(transport);
        return NULL;
    }

    if (!socket_env || !socket_env[0])
        socket_env = VGPU_SOCKET_DEFAULT_PATH;
    if (strlen(socket_env) >= sizeof(transport->socket_path)) {
        fprintf(stderr, "renderer: socket path too long: %s\n", socket_env);
        free(transport);
        return NULL;
    }
    strcpy(transport->socket_path, socket_env);

    if (transport_needs_hw(transport)) {
        transport->hw_fd = open(DEV_PATH, O_RDWR);
        if (transport->hw_fd < 0) {
            perror("open " DEV_PATH);
            gpu_transport_close(transport);
            return NULL;
        }
    }

    if (transport_needs_socket(transport)) {
        transport->socket_fd = connect_socket_path(transport->socket_path);
        if (transport->socket_fd < 0) {
            fprintf(stderr, "renderer: connect(%s) failed: %s\n",
                    transport->socket_path, strerror(-transport->socket_fd));
            gpu_transport_close(transport);
            return NULL;
        }
    }

    if (debug_enabled) {
        fprintf(stderr, "renderer: gpu backend=%s render=%ux%u band=%ux%u",
                backend_mode_name(transport->mode),
                VOXEL_RENDER_WIDTH,
                VOXEL_RENDER_HEIGHT,
                VOXEL_RENDER_WIDTH,
                VOXEL_BAND_CACHE_HEIGHT);
        if (transport_needs_socket(transport))
            fprintf(stderr, " socket=%s", transport->socket_path);
        fprintf(stderr, "\n");
    }

    if (transport_needs_hw(transport))
        log_extmem_state(transport, debug_enabled);

    {
        const char *diag = getenv("VOXEL_DIAG_BBOX");
        g_diag_bbox = (diag && diag[0] && strcmp(diag, "0") != 0) ? 1 : 0;
        if (g_diag_bbox && debug_enabled)
            fprintf(stderr, "renderer: VOXEL_DIAG_BBOX enabled (per-60-frame quad bbox log)\n");
    }

    {
        const char *diag = getenv("VOXEL_DIAG_CACHE");
        g_diag_cache = (diag && diag[0] && strcmp(diag, "0") != 0) ? 1 : 0;
        if (g_diag_cache && debug_enabled)
            fprintf(stderr, "renderer: VOXEL_DIAG_CACHE enabled (per-band hit/miss + HW dma_status log)\n");
    }

    {
        const char *reuse = getenv("VOXEL_HW_BAND_REUSE");
        g_hw_band_reuse = (reuse && reuse[0] && strcmp(reuse, "0") == 0) ? 0 : 1;
        if (g_hw_band_reuse && debug_enabled)
            fprintf(stderr,
                    "renderer: VOXEL_HW_BAND_REUSE enabled (full-band skip path; set =0 to disable)\n");
    }

    {
        const char *pipeline = getenv("VOXEL_PIPELINE_FRAMES");
        int pipeline_default =
            (!pipeline || pipeline[0] == '\0') &&
            transport->mode == GPU_BACKEND_HW;
        if (pipeline_default || env_flag_enabled(pipeline)) {
            if (transport->mode == GPU_BACKEND_HW) {
                ret = gpu_transport_pipeline_start(transport, debug_enabled);
                if (ret < 0) {
                    fprintf(stderr,
                            "renderer: VOXEL_PIPELINE_FRAMES disabled (worker start failed: %s)\n",
                            strerror(-ret));
                }
            } else if (debug_enabled) {
                fprintf(stderr,
                        "renderer: VOXEL_PIPELINE_FRAMES requested but requires VOXEL_GPU_BACKEND=hw\n");
            }
        }
    }

    return transport;
}

void gpu_transport_close(GPUTransport *transport)
{
    if (!transport)
        return;

    gpu_transport_pipeline_stop(transport);
    (void)gpu_transport_wait_pending_flip(transport);

    if (transport->socket_fd >= 0)
        close(transport->socket_fd);
    if (transport->hw_fd >= 0)
        close(transport->hw_fd);
    free(transport);

    for (unsigned set = 0; set < 2; set++) {
        for (unsigned i = 0; i < VOXEL_BAND_COUNT; i++) {
            free(g_bins_pool[set][i].flat_buf);
            g_bins_pool[set][i].flat_buf = NULL;
            g_bins_pool[set][i].flat_used = 0;
            g_bins_pool[set][i].flat_capacity = 0;
        }
    }
}

int gpu_transport_clear(GPUTransport *transport)
{
    int ret = gpu_transport_pipeline_wait_idle(transport);

    if (ret < 0)
        return ret;

    ret = gpu_transport_wait_pending_flip(transport);

    if (ret < 0)
        return ret;

    if (transport_needs_hw(transport) &&
        ioctl(transport->hw_fd, VOXEL_IOC_CLEAR_FRAME) < 0) {
        perror("ioctl(CLEAR_FRAME)");
        ret = -errno;
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_CLEAR,
                                      NULL, 0, NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket CLEAR failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    return ret;
}

static int gpu_transport_flip_hw_only(GPUTransport *transport)
{
    struct timespec t0 = {0}, t1 = {0};
    int ret = 0;

    if (transport_needs_hw(transport) && transport->hw_flip_pending) {
        ret = gpu_transport_wait_pending_flip(transport);
        if (ret < 0)
            return ret;
    }

    if (g_diag_bbox)
        clock_gettime(CLOCK_MONOTONIC, &t0);

    if (transport->hw_async_flip_supported) {
        if (ioctl(transport->hw_fd, VOXEL_IOC_FLIP_ASYNC) < 0) {
            if (errno == ENOTTY || errno == EINVAL) {
                transport->hw_async_flip_supported = 0;
            } else {
                perror("ioctl(FLIP_ASYNC)");
                ret = -errno;
            }
        } else {
            transport->hw_flip_pending = 1;
        }
    }

    if (ret == 0 && !transport->hw_async_flip_supported &&
        ioctl(transport->hw_fd, VOXEL_IOC_FLIP) < 0) {
        perror("ioctl(FLIP)");
        ret = -errno;
    }

    if (g_diag_bbox && g_diag_log_flip_next && ret == 0 &&
        !transport->hw_flip_pending) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms_flip = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "renderer: FLIP flip=%5.2fms\n", ms_flip);
        g_diag_log_flip_next = 0;
    }

    return ret;
}

int gpu_transport_flip(GPUTransport *transport)
{
    int ret = 0;

    if (transport->pipeline_enabled) {
        pthread_mutex_lock(&transport->pipeline_mutex);
        if (transport->pipeline_flip_owned) {
            transport->pipeline_flip_owned = 0;
            pthread_mutex_unlock(&transport->pipeline_mutex);
            return 0;
        }
        pthread_mutex_unlock(&transport->pipeline_mutex);

        ret = gpu_transport_pipeline_wait_idle(transport);
        if (ret < 0)
            return ret;
    }

    if (transport_needs_hw(transport))
        ret = gpu_transport_flip_hw_only(transport);

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_FLIP,
                                      NULL, 0, NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket FLIP failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    return ret;
}

int gpu_transport_set_palette(GPUTransport *transport,
                              const struct voxel_palette_entry *entry)
{
    int ret = gpu_transport_pipeline_wait_idle(transport);
    int hw_written = 0;

    if (ret < 0)
        return ret;

    if (transport_needs_hw(transport)) {
        if (ioctl(transport->hw_fd, VOXEL_IOC_SET_PALETTE, entry) < 0) {
            perror("ioctl(SET_PALETTE)");
            ret = -errno;
        } else {
            hw_written = 1;
        }
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_SET_PALETTE,
                                      entry, sizeof(*entry), NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket SET_PALETTE failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    if (hw_written)
        gpu_transport_note_render_state_write(transport);

    return ret;
}

int gpu_transport_set_sky_palette(GPUTransport *transport,
                                  const struct voxel_sky_palette_entry *entry)
{
    int ret = gpu_transport_pipeline_wait_idle(transport);
    int hw_written = 0;

    if (ret < 0)
        return ret;

    if (transport_needs_hw(transport)) {
        if (ioctl(transport->hw_fd, VOXEL_IOC_SET_SKY_PALETTE, entry) < 0) {
            perror("ioctl(SET_SKY_PALETTE)");
            ret = -errno;
        } else {
            hw_written = 1;
        }
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_SET_SKY_PALETTE,
                                      entry, sizeof(*entry), NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket SET_SKY_PALETTE failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    if (hw_written)
        gpu_transport_note_generated_sky_palette_write(transport, entry->index);

    return ret;
}

int gpu_transport_set_fog(GPUTransport *transport,
                          const struct voxel_fog_state *fog)
{
    int ret = gpu_transport_pipeline_wait_idle(transport);
    int hw_written = 0;

    if (ret < 0)
        return ret;

    if (transport_needs_hw(transport)) {
        if (ioctl(transport->hw_fd, VOXEL_IOC_SET_FOG, fog) < 0) {
            perror("ioctl(SET_FOG)");
            ret = -errno;
        } else {
            hw_written = 1;
        }
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_SET_FOG,
                                      fog, sizeof(*fog), NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket SET_FOG failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    if (hw_written)
        gpu_transport_note_render_state_write(transport);

    return ret;
}

int gpu_transport_submit_descriptors(GPUTransport *transport,
                                     const void *descriptors,
                                     size_t descriptor_bytes)
{
    int ret = 0;

    if (descriptor_bytes > UINT32_MAX)
        return -E2BIG;

    if (transport_needs_hw(transport)) {
        if (transport->pipeline_enabled && transport->staged_active &&
            descriptor_bytes > 0 && !transport_needs_socket(transport))
            return gpu_transport_pipeline_queue_submit(transport);

        ret = gpu_transport_pipeline_wait_idle(transport);
        if (ret < 0)
            return ret;

        ret = submit_hw_binned(transport, descriptors, descriptor_bytes);
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_SUBMIT_QUADS,
                                      descriptors, (uint32_t)descriptor_bytes,
                                      NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket SUBMIT_QUADS failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
    }

    return ret;
}
