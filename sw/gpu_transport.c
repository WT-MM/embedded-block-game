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
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct descriptor_bin {
    /* iovecs reference either the original submit_buffer (fast-path quads,
     * zero-copy) or this band's slow_buf (band-crossing quads that need a
     * rewritten z0/uv). Adjacent same-band iovecs are coalesced so a chunk's
     * worth of contiguous descriptors collapses into a single entry. */
    struct iovec *iov;
    size_t iov_count;
    size_t iov_capacity;

    /* Side buffer for rewritten descriptors. Pre-sized to descriptor_bytes
     * so iovecs can safely point into it without being invalidated by a
     * mid-frame realloc. */
    uint8_t *slow_buf;
    size_t slow_used;
};

static struct descriptor_bin g_bins[VOXEL_BAND_COUNT];
static size_t g_slow_capacity;

#ifndef IOV_MAX
#  define IOV_MAX 1024
#endif

/* Diagnostics gated by VOXEL_DIAG_BBOX=1: scan submitted x_min/x_max and
 * y_min/y_max ranges so we can confirm nothing is leaking off-screen on
 * the left/top edges (red-stripe investigation). */
static int g_diag_bbox;
static unsigned g_diag_frame_count;

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

static int bin_add_iov(struct descriptor_bin *bin, void *ptr, size_t len)
{
    /* Coalesce with the previous entry when the new range is contiguous in
     * memory — the typical case for a chunk's worth of fast-path quads
     * (consecutive in submit_buffer, all landing in the same band) and for
     * runs of slow-path rewrites (consecutive in slow_buf). */
    if (bin->iov_count > 0) {
        struct iovec *last = &bin->iov[bin->iov_count - 1];
        if ((uint8_t *)last->iov_base + last->iov_len == (uint8_t *)ptr) {
            last->iov_len += len;
            return 0;
        }
    }

    if (bin->iov_count >= bin->iov_capacity) {
        size_t new_cap = bin->iov_capacity ? bin->iov_capacity * 2 : 256;
        struct iovec *p = realloc(bin->iov, new_cap * sizeof(*p));
        if (!p)
            return -ENOMEM;
        bin->iov = p;
        bin->iov_capacity = new_cap;
    }

    bin->iov[bin->iov_count].iov_base = ptr;
    bin->iov[bin->iov_count].iov_len = len;
    bin->iov_count++;
    return 0;
}

static int writev_all(int fd, struct iovec *iov, size_t count)
{
    /* Walk the iovec array in IOV_MAX chunks. The voxel_gpu kernel write()
     * is all-or-nothing per call (it only returns short on -EINTR after
     * partial copy_from_user, which we treat as fatal here), so we don't
     * need to handle partial-iovec consumption. */
    size_t i = 0;
    while (i < count) {
        size_t batch = count - i;
        if (batch > IOV_MAX)
            batch = IOV_MAX;

        ssize_t got = writev(fd, iov + i, (int)batch);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (got == 0)
            return -EPIPE;

        size_t consumed = (size_t)got;
        while (i < count && consumed >= iov[i].iov_len) {
            consumed -= iov[i].iov_len;
            i++;
        }
        if (consumed > 0) {
            /* Partial write through an iovec entry — adjust and retry. */
            iov[i].iov_base = (uint8_t *)iov[i].iov_base + consumed;
            iov[i].iov_len -= consumed;
        }
    }
    return 0;
}

static int submit_hw_band_iov(GPUTransport *transport, unsigned band_index,
                              struct iovec *iov, size_t iov_count)
{
    struct voxel_band_state band = { .band_index = band_index };
    int ret;

    if (ioctl(transport->hw_fd, VOXEL_IOC_BEGIN_BAND, &band) < 0) {
        perror("ioctl(BEGIN_BAND)");
        return -errno;
    }

    if (iov_count > 0) {
        ret = writev_all(transport->hw_fd, iov, iov_count);
        if (ret < 0) {
            errno = -ret;
            perror("writev(descriptors)");
            return ret;
        }
    }

    if (ioctl(transport->hw_fd, VOXEL_IOC_END_BAND) < 0) {
        perror("ioctl(END_BAND)");
        return -errno;
    }

    return 0;
}

static int bins_ensure_slow_capacity(size_t needed)
{
    if (g_slow_capacity >= needed)
        return 0;

    size_t new_cap = g_slow_capacity ? g_slow_capacity : 8192;
    while (new_cap < needed)
        new_cap *= 2;

    /* Realloc all bands' slow_bufs at once, BEFORE any iovec entries point
     * into them this frame. Iovec entries from the previous frame have
     * already been reset (iov_count = 0) by the time we get here. */
    for (unsigned i = 0; i < VOXEL_BAND_COUNT; i++) {
        uint8_t *p = realloc(g_bins[i].slow_buf, new_cap);
        if (!p)
            return -ENOMEM;
        g_bins[i].slow_buf = p;
    }
    g_slow_capacity = new_cap;
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

    /* Reset bins up front so the empty/error paths below see clean state. */
    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        g_bins[band].iov_count = 0;
        g_bins[band].slow_used = 0;
    }

    if (descriptor_bytes == 0) {
        for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
            ret = submit_hw_band_iov(transport, band, NULL, 0);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    /* Worst case for slow_buf: every quad is a band-crosser landing in this
     * band → descriptor_bytes total. Pre-size up front so iovec entries
     * stay valid across the binning pass. */
    ret = bins_ensure_slow_capacity(descriptor_bytes);
    if (ret < 0)
        return ret;

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

        /* Fast path: descriptor fits inside one band and didn't need
         * y-clipping. Point an iovec straight at submit_buffer — zero-copy.
         * Adjacent fast-path quads in the same band coalesce into one
         * iovec entry. */
        if (first_band == last_band &&
            (int)desc->y_min == y_min && (int)desc->y_max == y_max) {
            ret = bin_add_iov(&g_bins[first_band],
                              (void *)(uintptr_t)(stream + offset),
                              desc_size);
            if (ret < 0)
                goto done;
            offset += desc_size;
            continue;
        }

        /* Slow path: rewrite z0/uv into per-band slow_buf, point iovec there. */
        for (unsigned band = first_band; band <= last_band; band++) {
            struct descriptor_bin *bin = &g_bins[band];
            int band_y_min = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
            int band_y_max = band_y_min + (int)VOXEL_BAND_CACHE_HEIGHT - 1;
            int clipped_y_min = y_min > band_y_min ? y_min : band_y_min;
            int clipped_y_max = y_max < band_y_max ? y_max : band_y_max;
            uint8_t *dst = bin->slow_buf + bin->slow_used;

            rewrite_clipped(dst, stream + offset, desc_size,
                            clipped_y_min, clipped_y_max);
            bin->slow_used += desc_size;

            ret = bin_add_iov(bin, dst, desc_size);
            if (ret < 0)
                goto done;
        }

        offset += desc_size;
    }

    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        ret = submit_hw_band_iov(transport, band,
                                 g_bins[band].iov, g_bins[band].iov_count);
        if (ret < 0)
            goto done;
    }

done:
    if (g_diag_bbox && (g_diag_frame_count++ % 60) == 0) {
        size_t total_iov = 0;
        for (unsigned i = 0; i < VOXEL_BAND_COUNT; i++)
            total_iov += g_bins[i].iov_count;
        fprintf(stderr,
                "renderer: frame %u quad bbox x=[%d,%d] y=[%d,%d] "
                "viewport %ux%u iovecs=%zu\n",
                g_diag_frame_count,
                (int)diag_x_min, (int)diag_x_max,
                (int)diag_y_min, (int)diag_y_max,
                VOXEL_RENDER_WIDTH, VOXEL_RENDER_HEIGHT,
                total_iov);
    }
    return ret;
}

static void log_extmem_state(int hw_fd, int debug_enabled)
{
    struct voxel_extmem_state ext;

    if (ioctl(hw_fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0) {
        perror("ioctl(GET_EXTMEM)");
        return;
    }

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
        log_extmem_state(transport->hw_fd, debug_enabled);

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

    if (transport->socket_fd >= 0)
        close(transport->socket_fd);
    if (transport->hw_fd >= 0)
        close(transport->hw_fd);
    free(transport);

    for (unsigned i = 0; i < VOXEL_BAND_COUNT; i++) {
        free(g_bins[i].iov);
        free(g_bins[i].slow_buf);
        g_bins[i].iov = NULL;
        g_bins[i].slow_buf = NULL;
        g_bins[i].iov_count = 0;
        g_bins[i].iov_capacity = 0;
        g_bins[i].slow_used = 0;
    }
    g_slow_capacity = 0;
}

int gpu_transport_clear(GPUTransport *transport)
{
    int ret = 0;

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
    int ret = 0;

    if (transport_needs_hw(transport) &&
        ioctl(transport->hw_fd, VOXEL_IOC_FLIP) < 0) {
        perror("ioctl(FLIP)");
        ret = -errno;
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

    return ret;
}

int gpu_transport_set_palette(GPUTransport *transport,
                              const struct voxel_palette_entry *entry)
{
    int ret = 0;

    if (transport_needs_hw(transport) &&
        ioctl(transport->hw_fd, VOXEL_IOC_SET_PALETTE, entry) < 0) {
        perror("ioctl(SET_PALETTE)");
        ret = -errno;
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
