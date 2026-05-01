#include "gpu_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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
    uint8_t *bytes;
    size_t used;
};

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

static int bin_append(struct descriptor_bin *bin, const uint8_t *src, size_t len)
{
    if (SIZE_MAX - bin->used < len)
        return -EOVERFLOW;

    memcpy(bin->bytes + bin->used, src, len);
    bin->used += len;
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

static int bin_append_y_clipped(struct descriptor_bin *bin,
                                const uint8_t *src,
                                size_t len,
                                int clipped_y_min,
                                int clipped_y_max)
{
    uint8_t clipped[sizeof(struct quad_desc) + sizeof(struct quad_desc_uv)];
    struct quad_desc *desc;
    int delta_y;
    int64_t z0;

    if (len > sizeof(clipped))
        return -EINVAL;

    memcpy(clipped, src, len);
    desc = (struct quad_desc *)(void *)clipped;
    delta_y = clipped_y_min - desc->y_min;

    z0 = (int64_t)desc->z0 + (int64_t)desc->dz_dy * delta_y;
    desc->z0 = clamp_z_i64(z0);
    desc->y_min = (__s16)clipped_y_min;
    desc->y_max = (__s16)clipped_y_max;

    if (desc->flags & QUAD_FLAG_TEX) {
        struct quad_desc_uv *uv =
            (struct quad_desc_uv *)(void *)(clipped + sizeof(*desc));

        uv->u_over_w_0 = clamp_s32_i64((int64_t)uv->u_over_w_0 +
                                       (int64_t)uv->u_over_w_dy * delta_y);
        uv->v_over_w_0 = clamp_s32_i64((int64_t)uv->v_over_w_0 +
                                       (int64_t)uv->v_over_w_dy * delta_y);
        uv->one_over_w_0 = clamp_s32_i64((int64_t)uv->one_over_w_0 +
                                         (int64_t)uv->one_over_w_dy * delta_y);
    }

    return bin_append(bin, clipped, len);
}

static int submit_hw_band(GPUTransport *transport, unsigned band_index,
                          const void *descriptors, size_t descriptor_bytes)
{
    struct voxel_band_state band = { .band_index = band_index };
    int ret;

    if (ioctl(transport->hw_fd, VOXEL_IOC_BEGIN_BAND, &band) < 0) {
        perror("ioctl(BEGIN_BAND)");
        return -errno;
    }

    if (descriptor_bytes > 0) {
        ret = write_all(transport->hw_fd, descriptors, descriptor_bytes);
        if (ret < 0) {
            errno = -ret;
            perror("write(descriptors)");
            return ret;
        }
    }

    if (ioctl(transport->hw_fd, VOXEL_IOC_END_BAND) < 0) {
        perror("ioctl(END_BAND)");
        return -errno;
    }

    return 0;
}

static int submit_hw_binned(GPUTransport *transport, const void *descriptors,
                            size_t descriptor_bytes)
{
    const uint8_t *stream = descriptors;
    struct descriptor_bin bins[VOXEL_BAND_COUNT];
    size_t offset = 0;
    int ret = 0;

    if (descriptor_bytes == 0) {
        for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
            ret = submit_hw_band(transport, band, NULL, 0);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    memset(bins, 0, sizeof(bins));
    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        bins[band].bytes = malloc(descriptor_bytes);
        if (!bins[band].bytes) {
            ret = -ENOMEM;
            goto out;
        }
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
            goto out;
        }

        desc = (const struct quad_desc *)(const void *)(stream + offset);
        quad_descriptor_size(desc, &desc_size);
        if (descriptor_bytes - offset < desc_size) {
            ret = -EINVAL;
            goto out;
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

        for (unsigned band = first_band; band <= last_band; band++) {
            int band_y_min = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
            int band_y_max = band_y_min + (int)VOXEL_BAND_CACHE_HEIGHT - 1;
            int clipped_y_min = y_min > band_y_min ? y_min : band_y_min;
            int clipped_y_max = y_max < band_y_max ? y_max : band_y_max;

            ret = bin_append_y_clipped(&bins[band], stream + offset, desc_size,
                                       clipped_y_min, clipped_y_max);
            if (ret < 0)
                goto out;
        }

        offset += desc_size;
    }

    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++) {
        ret = submit_hw_band(transport, band, bins[band].bytes, bins[band].used);
        if (ret < 0)
            goto out;
    }

out:
    for (unsigned band = 0; band < VOXEL_BAND_COUNT; band++)
        free(bins[band].bytes);
    return ret;
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

GPUTransport *gpu_transport_open(void)
{
    const char *backend_env = getenv("VOXEL_GPU_BACKEND");
    const char *socket_env = getenv("VOXEL_GPU_SOCKET_PATH");
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

    fprintf(stderr, "renderer: gpu backend=%s render=%ux%u band=%ux%u",
            backend_mode_name(transport->mode),
            VOXEL_RENDER_WIDTH,
            VOXEL_RENDER_HEIGHT,
            VOXEL_RENDER_WIDTH,
            VOXEL_BAND_CACHE_HEIGHT);
    if (transport_needs_socket(transport))
        fprintf(stderr, " socket=%s", transport->socket_path);
    fprintf(stderr, "\n");

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
