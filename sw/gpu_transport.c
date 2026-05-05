#include "gpu_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#define DEV_PATH "/dev/voxel_gpu"
#define SOCKET_MAGIC "VGPU"

typedef enum {
    GPU_BACKEND_HW = 0,
    GPU_BACKEND_SOCKET,
    GPU_BACKEND_TEE,
} GPUBackendMode;

struct GPUTransport {
    GPUBackendMode mode;
    int hw_fd;
    int socket_fd;
    int hw_flip_pending;
    int hw_async_flip_supported;
    int hw_sky_gradient_clear_enabled;
    uint32_t hw_sky_epoch;
    uint32_t hw_sky_band_epoch[2][VOXEL_BAND_COUNT];
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct descriptor_bin {
    /* Flat buffer for binned descriptors */
    uint8_t *flat_buf;
    size_t flat_used;
    size_t flat_capacity;
};

static struct descriptor_bin g_bins[VOXEL_BAND_COUNT];

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

#ifndef IOV_MAX
#  define IOV_MAX 1024
#endif

/* Diagnostics gated by VOXEL_DIAG_BBOX=1: scan submitted x_min/x_max and
 * y_min/y_max ranges so we can confirm nothing is leaking off-screen on
 * the left/top edges (red-stripe investigation). */
static int g_diag_bbox;
static unsigned g_diag_frame_count;
static int g_diag_log_flip_next;

enum {
    DIAG_SKY_GRADIENT_BASE = 40,
    DIAG_SKY_GRADIENT_LAST = 63,
    DIAG_EXTMEM_COPY_TARGET_SHIFT = 11,
    HW_SKY_EPOCH_INITIAL = 1,
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
    uint64_t bbox_pixels_1px;
    uint64_t pair_cycles_2px;
    uint64_t fetch_words;
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

static int palette_index_is_generated_sky(uint8_t index)
{
    return index >= DIAG_SKY_GRADIENT_BASE &&
           index <= DIAG_SKY_GRADIENT_LAST;
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
    if (desc->flags & QUAD_FLAG_TEX)
        cost->textured_band_copies++;

    redundant_sky_clear = diag_desc_is_redundant_sky_clear(desc);
    if (redundant_sky_clear) {
        cost->skipped_sky_copies++;
        cost->desc_overhead_cycles_band[band] += DIAG_SKIP_DESC_OVERHEAD_CYCLES;
    } else {
        cost->bbox_pixels_1px += rows * width_1px;
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
    cost->init_cycles[band] += (pixels + 1) / 2;
    cost->flush_words[band] += pixels;
}

static void diag_remove_cached_sky_band_cost(struct diag_cost *cost,
                                             unsigned band)
{
    uint64_t pixels;
    uint64_t init_cycles;
    uint64_t primer_words;

    if (!cost || band >= VOXEL_BAND_COUNT)
        return;

    pixels = diag_band_pixels(band);
    init_cycles = (pixels + 1) / 2;
    primer_words = sizeof(g_band_primers[band]) / sizeof(uint32_t);

    if (cost->init_cycles[band] >= init_cycles)
        cost->init_cycles[band] -= init_cycles;
    else
        cost->init_cycles[band] = 0;

    if (cost->flush_words[band] >= pixels)
        cost->flush_words[band] -= pixels;
    else
        cost->flush_words[band] = 0;

    if (cost->band_copies > 0)
        cost->band_copies--;
    if (cost->bbox_pixels_1px > 0)
        cost->bbox_pixels_1px--;
    if (cost->pair_cycles_2px > 0)
        cost->pair_cycles_2px--;

    if (cost->fetch_words >= primer_words)
        cost->fetch_words -= primer_words;
    else
        cost->fetch_words = 0;
    if (cost->fetch_words_band[band] >= primer_words)
        cost->fetch_words_band[band] -= primer_words;
    else
        cost->fetch_words_band[band] = 0;

    if (cost->desc_overhead_cycles_band[band] >=
        DIAG_NONSKIP_DESC_OVERHEAD_CYCLES) {
        cost->desc_overhead_cycles_band[band] -=
            DIAG_NONSKIP_DESC_OVERHEAD_CYCLES;
    } else {
        cost->desc_overhead_cycles_band[band] = 0;
    }

    cost->cached_sky_band_reuses++;
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

static void gpu_transport_note_generated_sky_palette_write(GPUTransport *transport,
                                                           uint8_t index)
{
    if (!transport || !palette_index_is_generated_sky(index))
        return;

    transport->hw_sky_epoch++;
    if (transport->hw_sky_epoch == 0) {
        memset(transport->hw_sky_band_epoch, 0,
               sizeof(transport->hw_sky_band_epoch));
        transport->hw_sky_epoch = HW_SKY_EPOCH_INITIAL;
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
    if (!transport->hw_sky_gradient_clear_enabled)
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

static int quad_descriptor_size(const struct quad_desc *desc, size_t *size)
{
    *size = sizeof(struct quad_desc);
    if (desc->flags & QUAD_FLAG_TEX)
        *size += sizeof(struct quad_desc_uv);
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
                               struct diag_band_submit *diag)
{
    struct voxel_band_state band = { .band_index = band_index };
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

static int submit_hw_binned(GPUTransport *transport, const void *descriptors,
                            size_t descriptor_bytes)
{
    const uint8_t *stream = descriptors;
    size_t offset = 0;
    int ret = 0;
    int16_t diag_x_min = INT16_MAX, diag_x_max = INT16_MIN;
    int16_t diag_y_min = INT16_MAX, diag_y_max = INT16_MIN;
    struct diag_cost diag_cost = {0};
    struct diag_band_submit diag_bands[VOXEL_BAND_COUNT];
    int diag_log_frame = 0;
    int copy_target_buffer = -1;

    struct timespec t_start = {0}, t_binned = {0}, t_submit = {0};
    int have_t_binned = 0;
    int have_t_submit = 0;

    if (g_diag_bbox)
        clock_gettime(CLOCK_MONOTONIC, &t_start);
    if (g_diag_bbox)
        diag_log_frame = ((g_diag_frame_count++ % 60) == 0);
    memset(diag_bands, 0, sizeof(diag_bands));

    init_band_primers();

    /* Reset bins up front so the empty/error paths below see clean state. */
    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        g_bins[band].flat_used = 0;
        ret = bin_append_descriptor(&g_bins[band], &g_band_primers[band],
                                    sizeof(g_band_primers[band]));
        if (ret < 0)
            return ret;
        if (g_diag_bbox) {
            diag_add_band_fixed_cost(&diag_cost, band);
            diag_add_band_descriptor(&diag_cost, band, &g_band_primers[band],
                                     g_band_primers[band].y_min,
                                     g_band_primers[band].y_max,
                                     sizeof(g_band_primers[band]));
        }
    }

    if (descriptor_bytes == 0) {
        for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
            ret = submit_hw_band_flat(transport, band,
                                      g_bins[band].flat_buf,
                                      g_bins[band].flat_used,
                                      diag_log_frame ? &diag_bands[band] : NULL);
            if (ret < 0)
                return ret;
        }
        if (diag_log_frame)
            g_diag_log_flip_next = 1;
        return 0;
    }

    while (offset < descriptor_bytes) {
        const struct quad_desc *desc;
        size_t desc_size;
        int y_min;
        int y_max;
        unsigned first_band;
        unsigned last_band;

        if (descriptor_bytes - offset < sizeof(struct quad_desc)) {
            ret = -EINVAL;
            goto done;
        }

        desc = (const struct quad_desc *)(const void *)(stream + offset);
        quad_descriptor_size(desc, &desc_size);
        if (descriptor_bytes - offset < desc_size) {
            ret = -EINVAL;
            goto done;
        }

        if (g_diag_bbox) {
            diag_cost.desc_count++;
            if (desc->x_min < diag_x_min) diag_x_min = desc->x_min;
            if (desc->x_max > diag_x_max) diag_x_max = desc->x_max;
            if (desc->y_min < diag_y_min) diag_y_min = desc->y_min;
            if (desc->y_max > diag_y_max) diag_y_max = desc->y_max;
        }

        y_min = desc->y_min;
        y_max = desc->y_max;
        if (y_max < 0 || y_min >= (int)VOXEL_RENDER_HEIGHT || y_min > y_max) {
            offset += desc_size;
            continue;
        }
        if (y_min < 0)
            y_min = 0;
        if (y_max >= (int)VOXEL_RENDER_HEIGHT)
            y_max = (int)VOXEL_RENDER_HEIGHT - 1;

        first_band = (unsigned)y_min / VOXEL_BAND_CACHE_HEIGHT;
        last_band = (unsigned)y_max / VOXEL_BAND_CACHE_HEIGHT;
        if (last_band >= VOXEL_BAND_COUNT)
            last_band = VOXEL_BAND_COUNT - 1;

        /*
         * Cheap path: the FPGA band init path already paints the sky gradient
         * into the resident cache and marks it dirty for generated-sky flushes.
         * Full-width sky-gradient quads are therefore redundant command traffic;
         * omit them before they consume FIFO, fetch, and descriptor overhead.
         */
        if (transport->hw_sky_gradient_clear_enabled &&
            diag_desc_is_redundant_sky_clear(desc)) {
            if (g_diag_bbox)
                diag_count_omitted_sky_clear(&diag_cost, first_band, last_band);
            offset += desc_size;
            continue;
        }

        /* Fast path: descriptor fits inside one band and didn't need y-clipping. */
        if (first_band == last_band &&
            (int)desc->y_min == y_min && (int)desc->y_max == y_max) {
            struct descriptor_bin *bin = &g_bins[first_band];

            ret = bin_append_descriptor(bin, stream + offset, desc_size);
            if (ret < 0)
                goto done;
            if (g_diag_bbox)
                diag_add_band_descriptor(&diag_cost, first_band, desc,
                                         y_min, y_max, desc_size);

            offset += desc_size;
            continue;
        }

        /* Slow path: rewrite z0/uv into per-band flat_buf. */
        for (unsigned band = first_band; band <= last_band; band++) {
            struct descriptor_bin *bin = &g_bins[band];
            int band_y_min = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
            int band_y_max = band_y_min + (int)VOXEL_BAND_CACHE_HEIGHT - 1;
            int clipped_y_min = y_min > band_y_min ? y_min : band_y_min;
            int clipped_y_max = y_max < band_y_max ? y_max : band_y_max;
            uint8_t *dst;

            ret = bin_ensure_flat_capacity(bin, bin->flat_used + desc_size);
            if (ret < 0)
                goto done;

            dst = bin->flat_buf + bin->flat_used;
            rewrite_clipped(dst, stream + offset, desc_size,
                            clipped_y_min, clipped_y_max);
            bin->flat_used += desc_size;
            if (g_diag_bbox)
                diag_add_band_descriptor(&diag_cost, band, desc,
                                         clipped_y_min, clipped_y_max,
                                         desc_size);
        }

        offset += desc_size;
    }

    if (g_diag_bbox) {
        clock_gettime(CLOCK_MONOTONIC, &t_binned);
        have_t_binned = 1;
    }

    if (transport->hw_sky_gradient_clear_enabled)
        copy_target_buffer = gpu_transport_read_copy_target_buffer(transport);

    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        const int primer_only =
            g_bins[band].flat_used == sizeof(g_band_primers[band]);

        if (primer_only && copy_target_buffer >= 0 &&
            transport->hw_sky_band_epoch[copy_target_buffer][band] ==
                transport->hw_sky_epoch) {
            if (g_diag_bbox)
                diag_remove_cached_sky_band_cost(&diag_cost, band);
            continue;
        }

        ret = submit_hw_band_flat(transport, band,
                                  g_bins[band].flat_buf, g_bins[band].flat_used,
                                  diag_log_frame ? &diag_bands[band] : NULL);
        if (ret < 0)
            goto done;

        if (copy_target_buffer >= 0) {
            transport->hw_sky_band_epoch[copy_target_buffer][band] =
                primer_only ? transport->hw_sky_epoch : 0;
        }
    }

    if (g_diag_bbox) {
        clock_gettime(CLOCK_MONOTONIC, &t_submit);
        have_t_submit = 1;
    }

done:
    if (g_diag_bbox && have_t_binned && have_t_submit && diag_log_frame) {
        uint64_t init_cycles = diag_sum_u64(diag_cost.init_cycles,
                                            VOXEL_BAND_COUNT);
        uint64_t overhead_cycles =
            diag_sum_u64(diag_cost.desc_overhead_cycles_band,
                         VOXEL_BAND_COUNT);
        uint64_t flush_words = diag_sum_u64(diag_cost.flush_words,
                                            VOXEL_BAND_COUNT);
        uint64_t flush_wall_cycles =
            diag_write_slack_limited_cycles(flush_words);
        uint64_t ready_cycles = diag_estimate_ready_cycles(&diag_cost);
        uint64_t vsync_slots =
            (ready_cycles + DIAG_VGA_FRAME_CYCLES - 1) /
            DIAG_VGA_FRAME_CYCLES;
        double ms_bin = (t_binned.tv_sec - t_start.tv_sec) * 1000.0 +
                        (t_binned.tv_nsec - t_start.tv_nsec) / 1e6;
        double ms_submit = (t_submit.tv_sec - t_binned.tv_sec) * 1000.0 +
                           (t_submit.tv_nsec - t_binned.tv_nsec) / 1e6;

        if (vsync_slots == 0)
            vsync_slots = 1;

        fprintf(stderr,
                "renderer: HW mode [flat buf, %u quads, bbox: (%d,%d)-(%d,%d)] bin=%5.2fms bands=%5.2fms\n",
                (unsigned)(descriptor_bytes / sizeof(struct quad_desc)), /* approx */
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
                "sky_band_reuse=%llu "
                "bbox1=%5.2fms bbox2=%5.2fms save=%5.2fms init=%5.2fms "
                "fetch=%5.2fms overhead=%5.2fms flush_raw=%5.2fms "
                "flush_slack=%5.2fms "
                "ideal_ready=%5.2fms vsync_floor=%5.2fms slots=%llu\n",
                (unsigned long long)diag_cost.desc_count,
                (unsigned long long)diag_cost.band_copies,
                (unsigned long long)diag_cost.textured_band_copies,
                (unsigned long long)diag_cost.skipped_sky_copies,
                (unsigned long long)diag_cost.cached_sky_band_reuses,
                diag_cycles_ms(diag_cost.bbox_pixels_1px),
                diag_cycles_ms(diag_cost.pair_cycles_2px),
                diag_cycles_ms(diag_cost.bbox_pixels_1px -
                               diag_cost.pair_cycles_2px),
                diag_cycles_ms(init_cycles),
                diag_cycles_ms(diag_cost.fetch_words),
                diag_cycles_ms(overhead_cycles),
                diag_cycles_ms(flush_words),
                diag_cycles_ms(flush_wall_cycles),
                diag_cycles_ms(ready_cycles),
                diag_cycles_ms(vsync_slots * DIAG_VGA_FRAME_CYCLES),
                (unsigned long long)vsync_slots);
        g_diag_log_flip_next = 1;
    }
    return ret;
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
    transport->hw_sky_epoch = HW_SKY_EPOCH_INITIAL;

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

    return transport;
}

void gpu_transport_close(GPUTransport *transport)
{
    if (!transport)
        return;

    (void)gpu_transport_wait_pending_flip(transport);

    if (transport->socket_fd >= 0)
        close(transport->socket_fd);
    if (transport->hw_fd >= 0)
        close(transport->hw_fd);
    free(transport);

    for (unsigned i = 0; i < VOXEL_BAND_COUNT; i++) {
        free(g_bins[i].flat_buf);
        g_bins[i].flat_buf = NULL;
        g_bins[i].flat_used = 0;
        g_bins[i].flat_capacity = 0;
    }
}

int gpu_transport_clear(GPUTransport *transport)
{
    int ret = gpu_transport_wait_pending_flip(transport);

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

int gpu_transport_flip(GPUTransport *transport)
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

    if (transport_needs_hw(transport)) {
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
    }

    if (transport_needs_socket(transport)) {
        int sock_ret = socket_request(transport, VGPU_SOCKET_CMD_FLIP,
                                      NULL, 0, NULL, NULL);
        if (sock_ret < 0 && ret == 0) {
            fprintf(stderr, "renderer: socket FLIP failed: %s\n",
                    strerror(-sock_ret));
            ret = sock_ret;
        }
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

int gpu_transport_set_palette(GPUTransport *transport,
                              const struct voxel_palette_entry *entry)
{
    int ret = 0;
    int hw_written = 0;

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
        gpu_transport_note_generated_sky_palette_write(transport, entry->index);

    return ret;
}

int gpu_transport_set_fog(GPUTransport *transport,
                          const struct voxel_fog_state *fog)
{
    int ret = 0;

    if (transport_needs_hw(transport) &&
        ioctl(transport->hw_fd, VOXEL_IOC_SET_FOG, fog) < 0) {
        perror("ioctl(SET_FOG)");
        ret = -errno;
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
