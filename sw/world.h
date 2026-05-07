#ifndef WORLD_H
#define WORLD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "block_types.h"

#define WORLD_CHUNK_SIZE 16
#define WORLD_CHUNK_HEIGHT 16
#define WORLD_SAVE_PATH_MAX 512

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t face;
    uint8_t type;
    uint8_t u_size;
    uint8_t v_size;
    uint8_t sky_light;
    uint8_t block_light;
} ChunkFace;

/* Immutable snapshot of a chunk's face mesh - published by the rebuild path
 * via atomic exchange on chunk->live_mesh, read lock-free by the renderer.
 * Generation matches chunk->generation at build time so the worker thread
 * can detect stale rebuilds (chunk edited again before its job ran). */
typedef struct ChunkMesh {
    ChunkFace *faces;
    int face_count;
    uint32_t generation;
    bool meshed_near;
} ChunkMesh;

typedef enum {
    CHUNK_FLAG_LOADED       = 1u << 0,
    CHUNK_FLAG_MESH_DIRTY   = 1u << 1,
    CHUNK_FLAG_MESH_READY   = 1u << 2,
    CHUNK_FLAG_MODIFIED     = 1u << 3,
    CHUNK_FLAG_MESHED_NEAR  = 1u << 4,
    CHUNK_FLAG_MESH_QUEUED  = 1u << 5,
} ChunkFlags;

typedef struct {
    int chunk_x;
    int chunk_z;
    uint32_t flags;
    uint32_t generation;
    uint32_t last_used_epoch;
    BlockID blocks[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    uint8_t sky_light[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    uint8_t block_light[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    /* Scratch buffer used by rebuild_chunk_faces to assemble a ChunkMesh
     * snapshot, which is then published via live_mesh. Renderer must NOT
     * read these directly - they are mutated mid-rebuild. */
    ChunkFace *faces;
    int face_count;
    int face_capacity;
    /* live_mesh is the latest published immutable snapshot. Renderer reads
     * lock-free via atomic_load. retired_mesh is the previous snapshot
     * pending free - kept one publish cycle so any in-flight reader pointer
     * is no longer in use by the time we free it (the renderer never
     * holds a chunk-mesh pointer past one frame). */
    _Atomic(ChunkMesh *) live_mesh;
    _Atomic(ChunkMesh *) retired_mesh;
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
    bool persistence_enabled;
    char save_root[WORLD_SAVE_PATH_MAX];
    bool lighting_dirty;
    bool meshes_dirty;
    bool async_mesh_rebuilds_enabled;
    /* Held by the mesh worker thread while it reads chunk blocks/lighting
     * during rebuild_chunk_faces, and by the main thread while it mutates
     * the chunk array (world_set_block, world_stream_around, lighting
     * propagation). The renderer never takes this - it reads chunk
     * geometry only via the lock-free live_mesh atomic snapshot. */
    pthread_mutex_t world_mu;
    bool world_mu_initialized;
} VoxelWorld;

void world_init(VoxelWorld *world);
void world_free(VoxelWorld *world);

bool world_init_infinite_procedural(VoxelWorld *world,
                                    uint32_t seed,
                                    int stone_tries_per_chunk,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z,
                                    const char *save_root);
bool world_stream_around(VoxelWorld *world, float world_x, float world_z);
bool world_flush(VoxelWorld *world);
bool world_rebuild_dirty_meshes(VoxelWorld *world);
bool world_rebuild_lighting(VoxelWorld *world);

const Chunk *world_get_chunk(const VoxelWorld *world, int chunk_x, int chunk_z);
Chunk *world_get_chunk_mut_locked(VoxelWorld *world, int chunk_x, int chunk_z);
BlockID world_get_block(const VoxelWorld *world, int wx, int wy, int wz);
bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type);
int world_total_faces(const VoxelWorld *world);
int world_total_blocks(const VoxelWorld *world);
int world_loaded_chunk_count(const VoxelWorld *world);
int world_chunk_capacity(const VoxelWorld *world);

/* Rebuild the chunk's mesh and atomically publish it to live_mesh.
 * Caller must hold world->world_mu. The previous live_mesh is moved
 * into retired_mesh; the caller (or next publish) is responsible for
 * eventually freeing it via chunk_mesh_free_retired. */
bool world_rebuild_and_publish_mesh(VoxelWorld *world, Chunk *chunk);

/* Free the retired_mesh slot if any. Safe to call any time the renderer
 * is not actively iterating that chunk's faces - main thread should call
 * once per frame after the renderer has finished its draw pass. */
void chunk_mesh_free_retired(Chunk *chunk);

/* Expose mesh worker hooks: claim/release the world mutex from outside
 * world.c. mesh_worker.c uses these so it does not need to friend-include
 * the VoxelWorld internals. */
void world_lock(VoxelWorld *world);
void world_unlock(VoxelWorld *world);

#endif
