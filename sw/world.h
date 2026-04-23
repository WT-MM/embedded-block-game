#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "block_types.h"

#define WORLD_CHUNK_SIZE 16
#define WORLD_CHUNK_HEIGHT 16

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t face;
    uint8_t type;
    uint8_t u_size;
    uint8_t v_size;
} ChunkFace;

typedef enum {
    CHUNK_FLAG_LOADED       = 1u << 0,
    CHUNK_FLAG_MESH_DIRTY   = 1u << 1,
    CHUNK_FLAG_MESH_READY   = 1u << 2,
    CHUNK_FLAG_MODIFIED     = 1u << 3,
    CHUNK_FLAG_MESHED_NEAR  = 1u << 4,
} ChunkFlags;

typedef struct {
    int chunk_x;
    int chunk_z;
    uint32_t flags;
    uint32_t generation;
    uint32_t last_used_epoch;
    BlockID blocks[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    ChunkFace *faces;
    int face_count;
    int face_capacity;
} Chunk;

typedef struct VoxelWorld {
    Chunk *chunks;
    int chunk_count;
    int chunk_capacity;
    int chunks_x;
    int chunks_z;
    int origin_chunk_x;
    int origin_chunk_z;
    int center_chunk_x;
    int center_chunk_z;
    int render_distance_chunks;
    int load_radius_chunks;
    uint32_t procedural_seed;
    int stone_tries_per_chunk;
    int *chunk_lookup;
    int chunk_lookup_capacity;
    uint32_t stream_epoch;
    int chunks_generated_last_stream;
    int chunks_reused_last_stream;
    int meshes_rebuilt_last_stream;
} VoxelWorld;

void world_init(VoxelWorld *world);
void world_free(VoxelWorld *world);

bool world_init_infinite_procedural(VoxelWorld *world,
                                    uint32_t seed,
                                    int stone_tries_per_chunk,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z);
bool world_stream_around(VoxelWorld *world, float world_x, float world_z);

const Chunk *world_get_chunk(const VoxelWorld *world, int chunk_x, int chunk_z);
BlockID world_get_block(const VoxelWorld *world, int wx, int wy, int wz);
bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type);
int world_total_faces(const VoxelWorld *world);
int world_total_blocks(const VoxelWorld *world);
int world_loaded_chunk_count(const VoxelWorld *world);
int world_chunk_capacity(const VoxelWorld *world);

#endif
