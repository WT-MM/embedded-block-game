#ifndef VIRTUAL_GPU_PROTOCOL_H
#define VIRTUAL_GPU_PROTOCOL_H

#include <stdint.h>

#define VGPU_SOCKET_DEFAULT_PATH "/tmp/voxel_gpu.sock"
#define VGPU_SOCKET_VERSION 1u

enum {
    VGPU_SOCKET_CMD_CLEAR = 1,
    VGPU_SOCKET_CMD_FLIP = 2,
    VGPU_SOCKET_CMD_SET_PALETTE = 3,
    VGPU_SOCKET_CMD_SUBMIT_QUADS = 4,
    VGPU_SOCKET_CMD_GET_STATUS = 5,
    VGPU_SOCKET_CMD_GET_FRAME_COUNT = 6,
};

struct vgpu_socket_header {
    char magic[4];
    uint16_t version;
    uint16_t opcode;
    uint32_t payload_size;
} __attribute__((packed));

struct vgpu_socket_reply {
    char magic[4];
    uint16_t version;
    int16_t status;
    uint32_t payload_size;
} __attribute__((packed));

#endif
