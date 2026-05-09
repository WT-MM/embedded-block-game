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
int gpu_transport_set_fog(GPUTransport *transport,
                          const struct voxel_fog_state *fog);
int gpu_transport_submit_descriptors(GPUTransport *transport,
                                     const void *descriptors,
                                     size_t descriptor_bytes);
size_t gpu_transport_textured_descriptor_size(const GPUTransport *transport);

/* Bin-during-emit: the renderer calls begin at frame start, then bin once
 * per finished descriptor. submit_descriptors will then skip its second-pass
 * walk over the contiguous stream and use the pre-staged per-band bins.
 * Both calls are HW-mode no-ops; the contiguous `descriptors` stream is
 * still the source of truth for the socket backend. */
void gpu_transport_begin_descriptors(GPUTransport *transport);
int  gpu_transport_bin_descriptor(GPUTransport *transport,
                                  const void *desc_bytes, size_t desc_size);

#endif
