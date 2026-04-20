#ifndef GPU_TRANSPORT_H
#define GPU_TRANSPORT_H

#include <stddef.h>

#include "virtual_gpu_protocol.h"
#include "voxel_gpu.h"

typedef struct GPUTransport GPUTransport;

GPUTransport *gpu_transport_open(void);
void gpu_transport_close(GPUTransport *transport);

int gpu_transport_clear(GPUTransport *transport);
int gpu_transport_flip(GPUTransport *transport);
int gpu_transport_set_palette(GPUTransport *transport,
                              const struct voxel_palette_entry *entry);
int gpu_transport_submit_descriptors(GPUTransport *transport,
                                     const void *descriptors,
                                     size_t descriptor_bytes);

#endif
