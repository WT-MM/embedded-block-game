#ifndef WORLD_H
#define WORLD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "block_types.h"

#define WORLD_CHUNK_SIZE 16
#define WORLD_CHUNK_HEIGHT 32
#define WORLD_SAVE_PATH_MAX 512
#define CHUNK_FACE_CROSS_A ((uint8_t)NUM_FACES)
#define CHUNK_FACE_CROSS_B ((uint8_t)(NUM_FACES + 1))
#define CHUNK_FACE_FLAT ((uint8_t)(NUM_FACES + 2))
#define WORLD_MAX_FALLING_BLOCKS 256

typedef enum {
    WORLD_BIOME_PLAINS = 0,
    WORLD_BIOME_OCEAN,
    WORLD_BIOME_DESERT,
    WORLD_BIOME_HILLS,
    WORLD_BIOME_MOUNTAINS,
} WorldBiome;

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
    /* Vertical extent of the block in 1/8 units (1..8). 8 = full block.
     * Flowing fluids use values < 8: the top Y of TOP and side faces is
     * lowered to y + height/8. All other blocks always set this to 8 so the
     * renderer's full-cube path is unchanged. */
    uint8_t height;
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
    /* Slot is allocated and findable by chunk_lookup, but its block /
     * lighting buffers are still being filled by the gen worker. Block
     * queries treat LOADING-only chunks as AIR. Cleared and replaced by
     * CHUNK_FLAG_LOADED once the worker integrates the result. */
    CHUNK_FLAG_LOADING      = 1u << 6,
    /* Set on a LOADING chunk after gen_worker_drain_pending pushes it
     * to the queue, so the next drain pass does not re-enqueue it. */
    CHUNK_FLAG_GEN_QUEUED   = 1u << 7,
    /* Set by the main thread after a player edit so the next
     * mesh_worker_drain_dirty pass routes this chunk through the
     * worker's priority lane (head-of-line) instead of the back of
     * the main FIFO. Cleared by drain when the chunk is pushed onto
     * the priority queue. */
    CHUNK_FLAG_MESH_EDIT_PRIORITY = 1u << 8,
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
    /* 0 = source / full height. 1..7 = decreasing flow level (Minecraft
     * encoding). Meaningful only for fluid cells (water/lava source or flow).
     * For any other block id this byte is ignored and should be left 0. */
    uint8_t water_level[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
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

typedef struct {
    bool active;
    BlockID type;
    int wx;
    int origin_y;
    int wz;
    float y;
} FallingBlock;

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
    int stream_chunks_per_frame;
    int near_chunk_radius;
    uint32_t procedural_seed;
    int stone_tries_per_chunk;
    bool desert_lava_pools_enabled;
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
    bool has_light_emitters;
    bool async_mesh_rebuilds_enabled;
    bool async_chunk_gen_enabled;
    _Atomic int foreground_lock_requests;
    uint64_t last_stream_lock_wait_ns;
    uint64_t last_stream_body_ns;
    FallingBlock falling_blocks[WORLD_MAX_FALLING_BLOCKS];
    int falling_block_count;
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
                                    bool desert_lava_pools_enabled,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z,
                                    const char *save_root);
bool world_read_save_metadata(const char *save_root,
                              uint32_t *seed_out,
                              int *stone_tries_per_chunk_out,
                              bool *desert_lava_pools_enabled_out);
bool world_stream_around(VoxelWorld *world, float world_x, float world_z);
bool world_flush(VoxelWorld *world);
bool world_rebuild_dirty_meshes(VoxelWorld *world);
bool world_rebuild_lighting(VoxelWorld *world);
void world_set_stream_chunks_per_frame(VoxelWorld *world, int chunks_per_frame);
int world_stream_chunks_per_frame(const VoxelWorld *world);
void world_set_near_chunk_radius(VoxelWorld *world, int radius);
int world_near_chunk_radius(const VoxelWorld *world);

/* Change the render distance at runtime. Resizes chunk storage and forces
 * a full re-stream on the next world_stream_around call. The player will
 * see a brief loading hitch while the new chunk window is populated. */
void world_set_render_distance(VoxelWorld *world, int distance);
int world_render_distance(const VoxelWorld *world);

const Chunk *world_get_chunk(const VoxelWorld *world, int chunk_x, int chunk_z);
Chunk *world_get_chunk_mut_locked(VoxelWorld *world, int chunk_x, int chunk_z);
BlockID world_get_block(const VoxelWorld *world, int wx, int wy, int wz);
bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type);
WorldBiome world_biome_at(const VoxelWorld *world, int wx, int wz);
const char *world_biome_name(WorldBiome biome);

/* Minecraft-style environment simulation tick. Call every ~250 ms (5 game
 * ticks). Water/lava sources spread to adjacent air downward then laterally
 * up to 7 blocks. Flow blocks evaporate when they lose upstream support from
 * fluid above, a source, or a lower-level flow. Unsupported gravity blocks
 * (sand/gravel) leave the integer grid and become smooth falling entities.
 * Returns true if any block changed. */
bool world_water_tick(VoxelWorld *world);
bool world_update_falling_blocks(VoxelWorld *world, float dt);
int world_falling_block_count(const VoxelWorld *world);

/* Diagnostic counters from the most recent world_water_tick. Useful for
 * verifying on hardware whether the simulation finds sources / spreads /
 * falling gravity blocks. */
typedef struct {
    int sources_seen;
    int flows_seen;
    int spread_placed;
    int evaporated;
    int falling_moved;
} WaterTickStats;
WaterTickStats world_water_tick_stats(void);
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

/* True if any in-array neighbor of (chunk_x, chunk_z) has CHUNK_FLAG_LOADING
 * set (and not yet CHUNK_FLAG_LOADED). Used by the mesh-rebuild path to
 * defer a chunk's mesh until all its neighbors are stable, avoiding
 * O(neighbors) re-meshes during async chunk load waves. Neighbors that
 * are not in chunks[] (outside the load radius) do not count.
 * Caller must hold world->world_mu. */
bool world_chunk_has_loading_neighbor_locked(const VoxelWorld *world,
                                             int chunk_x, int chunk_z);

/* Mark the chunk containing (wx, wz) with CHUNK_FLAG_MESH_EDIT_PRIORITY
 * so mesh_worker_drain_dirty routes it through the worker's priority
 * lane on its next pass. Use after a player edit to get sub-frame mesh
 * response even when the main mesh queue is backlogged. Caller must
 * NOT hold world->world_mu. */
void world_mark_chunk_mesh_edit_priority(VoxelWorld *world, int wx, int wz);

/* Worker-owned snapshot scratch used by world_run_mesh_job. Holds a self
 * + 4 cardinal neighbor Chunks plus a face scratch buffer. Allocate once
 * per worker thread and reuse for every job. */
typedef struct ChunkMeshWorkerScratch ChunkMeshWorkerScratch;

ChunkMeshWorkerScratch *chunk_mesh_worker_scratch_create(void);
void chunk_mesh_worker_scratch_destroy(ChunkMeshWorkerScratch *scratch);

/* Run one mesh-rebuild job. Locks world_mu briefly to snapshot data and
 * to publish the result; the heavy greedy meshing in between runs without
 * the lock held. The chunk's MESH_QUEUED bit is cleared on every exit
 * path (success, stale, or alloc failure). Returns true if a mesh was
 * actually published (i.e. the chunk still existed and the generation
 * still matched at publish time). */
bool world_run_mesh_job(VoxelWorld *world,
                        ChunkMeshWorkerScratch *scratch,
                        int chunk_x, int chunk_z,
                        uint32_t expected_generation);

/* Result buffer for off-thread chunk generation. Sized to a single chunk
 * - the worker fills this without holding world->world_mu, then hands it
 * to world_finalize_async_chunk_load for integration. */
typedef struct ChunkGenResult {
    BlockID blocks[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    uint8_t sky_light[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    /* Mirror of Chunk::water_level so an async-generated chunk can carry
     * its initial water levels into the integration step. Procedural gen
     * only ever emits sources, so this stays zero in practice - but a
     * future loaded snapshot could populate it. */
    uint8_t water_level[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    bool has_light_emitters;
} ChunkGenResult;

/* Run terrain generation, snapshot load, and per-chunk sky lighting into
 * `out`. Reads world->procedural_seed, ->stone_tries_per_chunk,
 * ->persistence_enabled, ->save_root - all immutable post-init. Does NOT
 * take world->world_mu. Safe to call from the gen worker thread. */
bool world_async_chunk_gen_offline(const VoxelWorld *world,
                                   int chunk_x, int chunk_z,
                                   ChunkGenResult *out);

/* Integrate a generated chunk under world->world_mu. Looks up the slot
 * by (chunk_x, chunk_z); discards if the slot is missing or its
 * generation no longer matches the one the job was enqueued with.
 * Returns true if the slot was finalized. Marks self+neighbors mesh
 * dirty. Caller must NOT already hold world->world_mu. */
bool world_finalize_async_chunk_load(VoxelWorld *world,
                                     int chunk_x, int chunk_z,
                                     uint32_t generation,
                                     const ChunkGenResult *result);

#endif
