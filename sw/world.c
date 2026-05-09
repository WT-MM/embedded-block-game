#include "world.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

#define CHUNK_LOOKUP_EMPTY (-1)

/* Greedy face merging runs only on chunks farther than this (Chebyshev)
 * distance from the player's chunk. Near chunks emit 1x1 faces so the
 * T-junction / UV-shimmer artifacts caused by merged quads stay out of
 * the viewer's foreground, while distant chunks (which dominate face
 * counts at large render distances) still get the merge win. */
#define DEFAULT_NEAR_CHUNK_RADIUS 1
#define DEFAULT_STREAM_CHUNKS_PER_FRAME 0
#define STREAM_CHUNKS_PER_FRAME_MAX 64
#define WORLD_META_VERSION 1u
#define WORLD_CHUNK_VERSION 1u

/*
 * Save format v1:
 *   world.meta
 *     - fixed-size WorldSaveHeader
 *   chunks/<chunk_x>_<chunk_z>.chk
 *     - fixed-size ChunkSaveHeader
 *     - WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE * WORLD_CHUNK_HEIGHT bytes
 *       of uint8_t block ids in y/z/x order
 *
 * The world remains procedural by default; on-disk data stores only modified
 * chunk snapshots, which are overlaid after deterministic terrain generation.
 */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t reserved;
    uint32_t procedural_seed;
    uint32_t stone_tries_per_chunk;
    uint16_t chunk_size;
    uint16_t chunk_height;
    uint32_t reserved2;
} WorldSaveHeader;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t reserved;
    int32_t chunk_x;
    int32_t chunk_z;
    uint16_t chunk_size;
    uint16_t chunk_height;
    uint32_t block_count;
} ChunkSaveHeader;

_Static_assert(sizeof(WorldSaveHeader) == 24, "WorldSaveHeader must be 24 bytes");
_Static_assert(sizeof(ChunkSaveHeader) == 24, "ChunkSaveHeader must be 24 bytes");

typedef struct {
    int wx;
    int wy;
    int wz;
} LightNode;

static bool chunk_in_window(int chunk_x, int chunk_z,
                            int origin_chunk_x, int origin_chunk_z,
                            int diameter);
static bool world_has_dirty_meshes_locked(const VoxelWorld *world);

static uint64_t timespec_diff_u64_ns(const struct timespec *end,
                                     const struct timespec *start)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000LL;
    }
    if (sec < 0)
        return 0;
    return (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
}

/* A slot is "present" once it has been allocated by stream_generate_chunk
 * and inserted into the lookup, regardless of whether the gen worker has
 * finished filling its blocks. LOADING-only slots are findable so that a
 * second stream pass for the same coords does not double-allocate, but
 * they appear as AIR to world_get_block until they finalize. */
static inline bool chunk_slot_is_present(const Chunk *chunk)
{
    return chunk &&
           (chunk->flags & (CHUNK_FLAG_LOADED | CHUNK_FLAG_LOADING)) != 0u;
}
static const int FACE_NX[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
static const int FACE_NY[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
static const int FACE_NZ[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };

static bool greedy_meshing_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("BLOCK_GAME_GREEDY_MESH");
        cached = (env && env[0] == '1' && env[1] == '\0');
    }
    return cached != 0;
}

static bool chunk_is_near(const VoxelWorld *world, const Chunk *chunk)
{
    int dx = chunk->chunk_x - world->center_chunk_x;
    int dz = chunk->chunk_z - world->center_chunk_z;
    int near_radius = world->near_chunk_radius;

    if (dx < 0)
        dx = -dx;
    if (dz < 0)
        dz = -dz;
    if (near_radius < 0)
        near_radius = DEFAULT_NEAR_CHUNK_RADIUS;

    return (dx <= near_radius) && (dz <= near_radius);
}

static uint32_t hash_chunk_coord(int chunk_x, int chunk_z)
{
    uint32_t h = ((uint32_t)chunk_x * 0x8da6b343u) ^
                 ((uint32_t)chunk_z * 0xd8163841u);

    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static int floor_div(int value, int divisor)
{
    int q = value / divisor;
    int r = value % divisor;

    if (r < 0)
        q--;
    return q;
}

static int positive_mod(int value, int divisor)
{
    int r = value % divisor;

    if (r < 0)
        r += divisor;
    return r;
}

static int chunk_coord_from_world(float world_pos)
{
    return (int)floorf(world_pos / (float)WORLD_CHUNK_SIZE);
}

static int env_int_clamped(const char *primary,
                           const char *fallback_name,
                           int default_value,
                           int min_value,
                           int max_value)
{
    const char *value = getenv(primary);
    char *end = NULL;
    long parsed;

    if ((!value || value[0] == '\0') && fallback_name)
        value = getenv(fallback_name);
    if (!value || value[0] == '\0')
        return default_value;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0'))
        return default_value;
    if (parsed < min_value)
        return min_value;
    if (parsed > max_value)
        return max_value;

    return (int)parsed;
}

static size_t chunk_block_count(void)
{
    return (size_t)WORLD_CHUNK_SIZE *
           (size_t)WORLD_CHUNK_SIZE *
           (size_t)WORLD_CHUNK_HEIGHT;
}

static bool build_world_meta_path(const VoxelWorld *world,
                                  char *path, size_t path_size)
{
    if (!world || !world->persistence_enabled || !path || path_size == 0)
        return false;

    return snprintf(path, path_size, "%s/world.meta", world->save_root) <
           (int)path_size;
}

static bool build_chunk_directory_path(const VoxelWorld *world,
                                       char *path, size_t path_size)
{
    if (!world || !world->persistence_enabled || !path || path_size == 0)
        return false;

    return snprintf(path, path_size, "%s/chunks", world->save_root) <
           (int)path_size;
}

static bool build_chunk_save_path(const VoxelWorld *world,
                                  int chunk_x, int chunk_z,
                                  char *path, size_t path_size)
{
    if (!world || !world->persistence_enabled || !path || path_size == 0)
        return false;

    return snprintf(path, path_size, "%s/chunks/%d_%d.chk",
                    world->save_root, chunk_x, chunk_z) < (int)path_size;
}

static bool build_chunk_temp_path(const VoxelWorld *world,
                                  int chunk_x, int chunk_z,
                                  char *path, size_t path_size)
{
    if (!world || !world->persistence_enabled || !path || path_size == 0)
        return false;

    return snprintf(path, path_size, "%s/chunks/%d_%d.chk.tmp",
                    world->save_root, chunk_x, chunk_z) < (int)path_size;
}

static bool ensure_directory_recursive(const char *path)
{
    char partial[WORLD_SAVE_PATH_MAX];
    size_t len;

    if (!path || path[0] == '\0')
        return false;

    len = strlen(path);
    if (len >= sizeof(partial))
        return false;

    memcpy(partial, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (partial[i] != '/')
            continue;

        partial[i] = '\0';
        if (partial[0] != '\0' &&
            mkdir(partial, 0755) < 0 &&
            errno != EEXIST)
            return false;
        partial[i] = '/';
    }

    if (mkdir(partial, 0755) < 0 && errno != EEXIST)
        return false;

    return true;
}

static bool ensure_world_save_layout(const VoxelWorld *world)
{
    char chunks_path[WORLD_SAVE_PATH_MAX];

    if (!world || !world->persistence_enabled)
        return true;
    if (!ensure_directory_recursive(world->save_root))
        return false;
    if (!build_chunk_directory_path(world, chunks_path, sizeof(chunks_path)))
        return false;
    return ensure_directory_recursive(chunks_path);
}

static bool load_or_create_world_meta(VoxelWorld *world)
{
    char meta_path[WORLD_SAVE_PATH_MAX];
    WorldSaveHeader header = {0};
    FILE *file;

    if (!world || !world->persistence_enabled)
        return true;
    if (!build_world_meta_path(world, meta_path, sizeof(meta_path)))
        return false;

    file = fopen(meta_path, "rb");
    if (!file) {
        if (errno != ENOENT)
            return false;

        memcpy(header.magic, "VWLD", 4);
        header.version = WORLD_META_VERSION;
        header.procedural_seed = world->procedural_seed;
        header.stone_tries_per_chunk = (uint32_t)world->stone_tries_per_chunk;
        header.chunk_size = WORLD_CHUNK_SIZE;
        header.chunk_height = WORLD_CHUNK_HEIGHT;

        file = fopen(meta_path, "wb");
        if (!file)
            return false;
        if (fwrite(&header, sizeof(header), 1, file) != 1) {
            fclose(file);
            return false;
        }
        return fclose(file) == 0;
    }

    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);

    if (memcmp(header.magic, "VWLD", 4) != 0 ||
        header.version != WORLD_META_VERSION ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT ||
        header.procedural_seed != world->procedural_seed ||
        header.stone_tries_per_chunk != (uint32_t)world->stone_tries_per_chunk)
        return false;

    return true;
}

bool world_read_save_metadata(const char *save_root,
                              uint32_t *seed_out,
                              int *stone_tries_per_chunk_out)
{
    char meta_path[WORLD_SAVE_PATH_MAX];
    WorldSaveHeader header = {0};
    FILE *file;

    if (!save_root || save_root[0] == '\0')
        return false;
    if (snprintf(meta_path, sizeof(meta_path), "%s/world.meta", save_root) >=
        (int)sizeof(meta_path))
        return false;

    file = fopen(meta_path, "rb");
    if (!file)
        return false;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);

    if (memcmp(header.magic, "VWLD", 4) != 0 ||
        header.version != WORLD_META_VERSION ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT)
        return false;

    if (seed_out)
        *seed_out = header.procedural_seed;
    if (stone_tries_per_chunk_out)
        *stone_tries_per_chunk_out = (int)header.stone_tries_per_chunk;
    return true;
}

static bool initialize_world_persistence(VoxelWorld *world,
                                         const char *save_root)
{
    if (!world)
        return false;

    world->persistence_enabled = false;
    world->save_root[0] = '\0';

    if (!save_root || save_root[0] == '\0')
        return true;

    if (strlen(save_root) >= sizeof(world->save_root))
        return false;

    memcpy(world->save_root, save_root, strlen(save_root) + 1);
    world->persistence_enabled = true;

    if (!ensure_world_save_layout(world) ||
        !load_or_create_world_meta(world)) {
        world->persistence_enabled = false;
        world->save_root[0] = '\0';
        return false;
    }

    return true;
}

static bool save_chunk_snapshot(const VoxelWorld *world, Chunk *chunk)
{
    char path[WORLD_SAVE_PATH_MAX];
    char temp_path[WORLD_SAVE_PATH_MAX];
    ChunkSaveHeader header = {0};
    uint8_t blocks[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    FILE *file;

    if (!world || !chunk || !world->persistence_enabled)
        return true;
    if (!(chunk->flags & CHUNK_FLAG_MODIFIED))
        return true;
    if (!build_chunk_save_path(world, chunk->chunk_x, chunk->chunk_z,
                               path, sizeof(path)) ||
        !build_chunk_temp_path(world, chunk->chunk_x, chunk->chunk_z,
                               temp_path, sizeof(temp_path)))
        return false;

    memcpy(header.magic, "VCHK", 4);
    header.version = WORLD_CHUNK_VERSION;
    header.chunk_x = chunk->chunk_x;
    header.chunk_z = chunk->chunk_z;
    header.chunk_size = WORLD_CHUNK_SIZE;
    header.chunk_height = WORLD_CHUNK_HEIGHT;
    header.block_count = (uint32_t)chunk_block_count();

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++)
                blocks[y][z][x] = (uint8_t)chunk->blocks[y][z][x];
        }
    }

    file = fopen(temp_path, "wb");
    if (!file)
        return false;
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(blocks, sizeof(blocks), 1, file) != 1) {
        fclose(file);
        unlink(temp_path);
        return false;
    }
    if (fclose(file) != 0) {
        unlink(temp_path);
        return false;
    }
    if (rename(temp_path, path) < 0) {
        unlink(temp_path);
        return false;
    }

    chunk->flags &= ~CHUNK_FLAG_MODIFIED;
    return true;
}

static bool load_chunk_snapshot(const VoxelWorld *world, Chunk *chunk)
{
    char path[WORLD_SAVE_PATH_MAX];
    ChunkSaveHeader header = {0};
    uint8_t blocks[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    FILE *file;

    if (!world || !chunk || !world->persistence_enabled)
        return true;
    if (!build_chunk_save_path(world, chunk->chunk_x, chunk->chunk_z,
                               path, sizeof(path)))
        return false;

    file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT)
            return true;
        return false;
    }

    if (fread(&header, sizeof(header), 1, file) != 1 ||
        fread(blocks, sizeof(blocks), 1, file) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);

    if (memcmp(header.magic, "VCHK", 4) != 0 ||
        header.version != WORLD_CHUNK_VERSION ||
        header.chunk_x != chunk->chunk_x ||
        header.chunk_z != chunk->chunk_z ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT ||
        header.block_count != chunk_block_count())
        return false;

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                uint8_t id = blocks[y][z][x];

                if (id >= NUM_BLOCK_TYPES)
                    return false;
                chunk->blocks[y][z][x] = (BlockID)id;
            }
        }
    }

    chunk->flags &= ~CHUNK_FLAG_MODIFIED;
    return true;
}

static bool persist_chunks_outside_window(VoxelWorld *world,
                                          int origin_chunk_x,
                                          int origin_chunk_z,
                                          int diameter)
{
    if (!world || !world->persistence_enabled)
        return true;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];
        bool keep = (chunk->flags & CHUNK_FLAG_LOADED) &&
                    chunk_in_window(chunk->chunk_x, chunk->chunk_z,
                                    origin_chunk_x, origin_chunk_z,
                                    diameter);

        if (!keep && !save_chunk_snapshot(world, chunk))
            return false;
    }

    return true;
}

static void clear_chunk_lookup(VoxelWorld *world)
{
    if (!world || !world->chunk_lookup)
        return;

    for (int i = 0; i < world->chunk_lookup_capacity; i++)
        world->chunk_lookup[i] = CHUNK_LOOKUP_EMPTY;
}

static bool ensure_chunk_lookup_capacity(VoxelWorld *world, int chunk_capacity)
{
    int needed = 16;

    while (needed < chunk_capacity * 4)
        needed <<= 1;

    if (world->chunk_lookup && world->chunk_lookup_capacity >= needed)
        return true;

    int *lookup = realloc(world->chunk_lookup, (size_t)needed * sizeof(*lookup));
    if (!lookup)
        return false;

    world->chunk_lookup = lookup;
    world->chunk_lookup_capacity = needed;
    clear_chunk_lookup(world);
    return true;
}

static int chunk_lookup_find_index(const VoxelWorld *world, int chunk_x, int chunk_z)
{
    if (!world || !world->chunks)
        return -1;

    if (!world->chunk_lookup || world->chunk_lookup_capacity <= 0) {
        for (int i = 0; i < world->chunk_count; i++) {
            const Chunk *chunk = &world->chunks[i];
            if (chunk_slot_is_present(chunk) &&
                chunk->chunk_x == chunk_x &&
                chunk->chunk_z == chunk_z)
                return i;
        }
        return -1;
    }

    uint32_t slot = hash_chunk_coord(chunk_x, chunk_z) &
                    (uint32_t)(world->chunk_lookup_capacity - 1);
    for (int probe = 0; probe < world->chunk_lookup_capacity; probe++) {
        int index = world->chunk_lookup[slot];

        if (index == CHUNK_LOOKUP_EMPTY)
            return -1;
        if (index >= 0 && index < world->chunk_count) {
            const Chunk *chunk = &world->chunks[index];
            if (chunk_slot_is_present(chunk) &&
                chunk->chunk_x == chunk_x &&
                chunk->chunk_z == chunk_z)
                return index;
        }

        slot = (slot + 1u) & (uint32_t)(world->chunk_lookup_capacity - 1);
    }

    return -1;
}

static bool chunk_lookup_insert(VoxelWorld *world, int index)
{
    if (!world || !world->chunk_lookup ||
        index < 0 || index >= world->chunk_count)
        return false;

    const Chunk *chunk = &world->chunks[index];
    uint32_t slot = hash_chunk_coord(chunk->chunk_x, chunk->chunk_z) &
                    (uint32_t)(world->chunk_lookup_capacity - 1);

    for (int probe = 0; probe < world->chunk_lookup_capacity; probe++) {
        int existing = world->chunk_lookup[slot];

        if (existing == CHUNK_LOOKUP_EMPTY) {
            world->chunk_lookup[slot] = index;
            return true;
        }
        if (existing >= 0 && existing < world->chunk_count) {
            const Chunk *other = &world->chunks[existing];
            if (other->chunk_x == chunk->chunk_x &&
                other->chunk_z == chunk->chunk_z) {
                world->chunk_lookup[slot] = index;
                return true;
            }
        }

        slot = (slot + 1u) & (uint32_t)(world->chunk_lookup_capacity - 1);
    }

    return false;
}

static bool rebuild_chunk_lookup(VoxelWorld *world)
{
    if (!ensure_chunk_lookup_capacity(world, world->chunk_capacity))
        return false;

    clear_chunk_lookup(world);
    for (int i = 0; i < world->chunk_count; i++) {
        if (chunk_slot_is_present(&world->chunks[i]) &&
            !chunk_lookup_insert(world, i))
            return false;
    }

    return true;
}

static void release_chunk(Chunk *chunk)
{
    ChunkMesh *live;
    ChunkMesh *retired;

    if (!chunk)
        return;

    free(chunk->faces);

    /* Free both published mesh slots before zeroing the struct. The
     * renderer must NOT be holding a chunk pointer through this path -
     * release_chunk is only called from world_free / retain_chunks_in_window
     * which run on the main thread between frames. */
    live = atomic_exchange_explicit(&chunk->live_mesh, NULL,
                                    memory_order_acq_rel);
    if (live) {
        free(live->faces);
        free(live);
    }
    retired = atomic_exchange_explicit(&chunk->retired_mesh, NULL,
                                       memory_order_acq_rel);
    if (retired) {
        free(retired->faces);
        free(retired);
    }

    memset(chunk, 0, sizeof(*chunk));
}

static void free_chunk_storage(VoxelWorld *world)
{
    if (!world)
        return;

    if (world->persistence_enabled && !world_flush(world))
        fprintf(stderr, "world: failed to flush modified chunks before free\n");

    if (world->chunks) {
        for (int i = 0; i < world->chunk_capacity; i++)
            release_chunk(&world->chunks[i]);
    }

    free(world->chunks);
    free(world->chunk_lookup);
    world->chunks = NULL;
    world->chunk_lookup = NULL;
    world->chunk_count = 0;
    world->chunk_capacity = 0;
    world->chunks_x = 0;
    world->chunks_z = 0;
    world->origin_chunk_x = 0;
    world->origin_chunk_z = 0;
    world->center_chunk_x = 0;
    world->center_chunk_z = 0;
    world->chunk_lookup_capacity = 0;
}

static bool chunk_in_window(int chunk_x, int chunk_z,
                            int origin_chunk_x, int origin_chunk_z,
                            int diameter)
{
    return chunk_x >= origin_chunk_x &&
           chunk_x < origin_chunk_x + diameter &&
           chunk_z >= origin_chunk_z &&
           chunk_z < origin_chunk_z + diameter;
}

static void clear_chunk_blocks(Chunk *chunk)
{
    memset(chunk->blocks, 0, sizeof(chunk->blocks));
}

static void clear_chunk_lighting(Chunk *chunk)
{
    memset(chunk->sky_light, 0, sizeof(chunk->sky_light));
    memset(chunk->block_light, 0, sizeof(chunk->block_light));
}

static void rebuild_chunk_sky_lighting(Chunk *chunk)
{
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return;

    memset(chunk->sky_light, 0, sizeof(chunk->sky_light));
    for (int lz = 0; lz < WORLD_CHUNK_SIZE; lz++) {
        for (int lx = 0; lx < WORLD_CHUNK_SIZE; lx++) {
            uint8_t sky = 15;

            for (int y = WORLD_CHUNK_HEIGHT - 1; y >= 0; y--) {
                BlockID id = chunk->blocks[y][lz][lx];

                if (block_blocks_light(id)) {
                    sky = 0;
                    continue;
                }

                chunk->sky_light[y][lz][lx] = sky;
            }
        }
    }
}

static bool chunk_has_light_emitters(const Chunk *chunk)
{
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                if (block_emission_level(chunk->blocks[y][z][x]) > 0)
                    return true;
            }
        }
    }

    return false;
}

static bool world_recompute_light_emitters_locked(VoxelWorld *world)
{
    if (!world)
        return false;

    for (int i = 0; i < world->chunk_count; i++) {
        if (chunk_has_light_emitters(&world->chunks[i])) {
            world->has_light_emitters = true;
            return true;
        }
    }

    world->has_light_emitters = false;
    return false;
}

static void initialize_chunk_slot(Chunk *chunk, int chunk_x, int chunk_z,
                                  uint32_t stream_epoch)
{
    uint32_t generation = chunk->generation + 1u;

    clear_chunk_blocks(chunk);
    clear_chunk_lighting(chunk);
    chunk->chunk_x = chunk_x;
    chunk->chunk_z = chunk_z;
    chunk->flags = CHUNK_FLAG_LOADED | CHUNK_FLAG_MESH_DIRTY;
    chunk->generation = generation ? generation : 1u;
    chunk->last_used_epoch = stream_epoch;
    chunk->face_count = 0;
}

/* Async-gen variant of initialize_chunk_slot: marks the slot LOADING (no
 * LOADED, no MESH_DIRTY) so the gen worker can fill it later. blocks /
 * lighting are zeroed; world_get_block + the mesh worker treat the slot
 * as AIR until finalize sets LOADED. */
static void initialize_chunk_slot_pending(Chunk *chunk,
                                          int chunk_x, int chunk_z,
                                          uint32_t stream_epoch)
{
    uint32_t generation = chunk->generation + 1u;

    chunk->chunk_x = chunk_x;
    chunk->chunk_z = chunk_z;
    chunk->flags = CHUNK_FLAG_LOADING;
    chunk->generation = generation ? generation : 1u;
    chunk->last_used_epoch = stream_epoch;
    chunk->face_count = 0;
}

static void mark_chunk_mesh_dirty(Chunk *chunk)
{
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return;

    chunk->flags |= CHUNK_FLAG_MESH_DIRTY;
}

static void mark_chunk_coord_dirty(VoxelWorld *world, int chunk_x, int chunk_z)
{
    int index = chunk_lookup_find_index(world, chunk_x, chunk_z);

    if (index >= 0)
        mark_chunk_mesh_dirty(&world->chunks[index]);
}

static void mark_chunk_and_neighbors_dirty(VoxelWorld *world, int chunk_x, int chunk_z)
{
    mark_chunk_coord_dirty(world, chunk_x, chunk_z);
    mark_chunk_coord_dirty(world, chunk_x - 1, chunk_z);
    mark_chunk_coord_dirty(world, chunk_x + 1, chunk_z);
    mark_chunk_coord_dirty(world, chunk_x, chunk_z - 1);
    mark_chunk_coord_dirty(world, chunk_x, chunk_z + 1);
}

static bool neighbor_is_loading_locked(const VoxelWorld *world,
                                       int chunk_x, int chunk_z)
{
    int idx = chunk_lookup_find_index(world, chunk_x, chunk_z);
    if (idx < 0)
        return false;
    const Chunk *n = &world->chunks[idx];
    return (n->flags & CHUNK_FLAG_LOADING) && !(n->flags & CHUNK_FLAG_LOADED);
}

bool world_chunk_has_loading_neighbor_locked(const VoxelWorld *world,
                                             int chunk_x, int chunk_z)
{
    if (!world)
        return false;
    return neighbor_is_loading_locked(world, chunk_x - 1, chunk_z) ||
           neighbor_is_loading_locked(world, chunk_x + 1, chunk_z) ||
           neighbor_is_loading_locked(world, chunk_x, chunk_z - 1) ||
           neighbor_is_loading_locked(world, chunk_x, chunk_z + 1);
}

void world_mark_chunk_mesh_edit_priority(VoxelWorld *world, int wx, int wz)
{
    if (!world)
        return;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);

    world_lock(world);
    Chunk *chunk = world_get_chunk_mut_locked(world, chunk_x, chunk_z);
    if (chunk && (chunk->flags & CHUNK_FLAG_LOADED))
        chunk->flags |= CHUNK_FLAG_MESH_EDIT_PRIORITY;
    world_unlock(world);
}

/*
 * A chunk needs re-meshing when one of its neighbors has been added or removed
 * since its last mesh, because an unloaded neighbor shows up as AIR in
 * `world_get_block`, which changes face-exposure. `mark_chunk_and_neighbors_dirty`
 * already covers every chunk adjacent to a newly *added* chunk (the leading-edge
 * perimeter). This function covers the mirror case: chunks on the *trailing*
 * edges whose outer neighbor was just dropped when the window shifted. The old
 * implementation dirtied all four edges unconditionally, which redundantly
 * re-meshed the leading and perpendicular sides too.
 */
static void mark_trailing_perimeter_dirty(VoxelWorld *world,
                                          int old_origin_x,
                                          int old_origin_z,
                                          int new_origin_x,
                                          int new_origin_z,
                                          int diameter)
{
    bool west_lost_neighbor  = old_origin_x < new_origin_x;
    bool east_lost_neighbor  = old_origin_x > new_origin_x;
    bool north_lost_neighbor = old_origin_z < new_origin_z;
    bool south_lost_neighbor = old_origin_z > new_origin_z;

    if (!west_lost_neighbor && !east_lost_neighbor &&
        !north_lost_neighbor && !south_lost_neighbor)
        return;

    int max_chunk_x = new_origin_x + diameter - 1;
    int max_chunk_z = new_origin_z + diameter - 1;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        if ((west_lost_neighbor  && chunk->chunk_x == new_origin_x) ||
            (east_lost_neighbor  && chunk->chunk_x == max_chunk_x) ||
            (north_lost_neighbor && chunk->chunk_z == new_origin_z) ||
            (south_lost_neighbor && chunk->chunk_z == max_chunk_z))
            mark_chunk_mesh_dirty(chunk);
    }
}

/* Sea level: any column whose surface lands below this gets flooded with
 * water up to (but not above) y=WORLDGEN_SEA_LEVEL. */
#define WORLDGEN_SEA_LEVEL 14
/* Trees only spawn on grass that is at or above this y. Slight buffer above
 * sea level so we don't end up with logs sprouting out of shallow ponds. */
#define WORLDGEN_TREE_MIN_Y (WORLDGEN_SEA_LEVEL + 1)
/* Tree canopy half-width in xz; the loop below scans neighbor cells out to
 * this radius beyond the chunk edge so canopies span chunk seams. */
#define WORLDGEN_TREE_RADIUS 2

static uint32_t hash_world_coord(int wx, int wz, uint32_t base_seed,
                                 uint32_t salt)
{
    uint32_t h = base_seed ^ salt;

    h ^= (uint32_t)wx * 0x9e3779b9u;
    h ^= (uint32_t)wz * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static float smoothstep01(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float value_noise_octave(int wx, int wz, int period, uint32_t base_seed,
                                uint32_t salt)
{
    int gx = (wx >= 0) ? (wx / period) : ((wx - period + 1) / period);
    int gz = (wz >= 0) ? (wz / period) : ((wz - period + 1) / period);
    int rx = wx - gx * period;
    int rz = wz - gz * period;
    float fx = smoothstep01((float)rx / (float)period);
    float fz = smoothstep01((float)rz / (float)period);

    float c00 = (float)(hash_world_coord(gx,     gz,     base_seed, salt) & 0xFFFFu);
    float c10 = (float)(hash_world_coord(gx + 1, gz,     base_seed, salt) & 0xFFFFu);
    float c01 = (float)(hash_world_coord(gx,     gz + 1, base_seed, salt) & 0xFFFFu);
    float c11 = (float)(hash_world_coord(gx + 1, gz + 1, base_seed, salt) & 0xFFFFu);

    float a = c00 + (c10 - c00) * fx;
    float b = c01 + (c11 - c01) * fx;
    return (a + (b - a) * fz) / 65535.0f; /* 0..1 */
}

/* Height range stays well inside [0, WORLD_CHUNK_HEIGHT) so trees and the
 * water column have headroom. Surfaces concentrate around sea level so most
 * gameplay is shoreline / lowlands, with hills poking above. */
static int worldgen_surface_height(int wx, int wz, uint32_t base_seed)
{
    float low  = value_noise_octave(wx, wz, 32, base_seed, 0xa1u);
    float mid  = value_noise_octave(wx, wz, 12, base_seed, 0xb2u);
    float fine = value_noise_octave(wx, wz, 4,  base_seed, 0xc3u);
    float n = low * 0.60f + mid * 0.30f + fine * 0.10f;

    int min_h = WORLDGEN_SEA_LEVEL - 4;
    int max_h = WORLDGEN_SEA_LEVEL + 8;
    int h = min_h + (int)(n * (float)(max_h - min_h));

    if (h < 1) h = 1;
    if (h >= WORLD_CHUNK_HEIGHT - 6) h = WORLD_CHUNK_HEIGHT - 7;
    return h;
}

/* Deterministic per-(wx,wz) tree roll. Returns true if a tree wants to spawn
 * here, and only if this column is the local maximum of the roll within its
 * footprint - that way two trees never overlap into each other. */
static bool worldgen_wants_tree(int wx, int wz, uint32_t base_seed)
{
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xd00du);
    /* ~1.6% base spawn density. */
    if ((roll & 0x3Fu) != 0u)
        return false;

    for (int dz = -WORLDGEN_TREE_RADIUS; dz <= WORLDGEN_TREE_RADIUS; dz++) {
        for (int dx = -WORLDGEN_TREE_RADIUS; dx <= WORLDGEN_TREE_RADIUS; dx++) {
            if (dx == 0 && dz == 0)
                continue;
            uint32_t neighbor = hash_world_coord(wx + dx, wz + dz,
                                                 base_seed, 0xd00du);
            if ((neighbor & 0x3Fu) != 0u)
                continue;
            /* Tie-break by raw hash; lower wins. */
            if (neighbor < roll)
                return false;
        }
    }
    return true;
}

static void worldgen_set_block_local(Chunk *chunk, int lx, int ly, int lz,
                                     BlockID block, bool overwrite)
{
    if (lx < 0 || lx >= WORLD_CHUNK_SIZE) return;
    if (lz < 0 || lz >= WORLD_CHUNK_SIZE) return;
    if (ly < 0 || ly >= WORLD_CHUNK_HEIGHT) return;
    if (!overwrite && chunk->blocks[ly][lz][lx] != BLOCK_AIR)
        return;
    chunk->blocks[ly][lz][lx] = block;
}

static void worldgen_place_tree(Chunk *chunk, int local_x, int trunk_top_y,
                                int local_z, uint32_t roll)
{
    int trunk_height = 4 + (int)(roll & 1u); /* 4 or 5 logs */
    int trunk_base = trunk_top_y - trunk_height + 1;
    /* Extend one extra block down to surface level so the tree is visually
     * rooted to the ground rather than floating one block above the grass. */
    int trunk_ground = (trunk_base > 0) ? trunk_base - 1 : trunk_base;

    for (int y = trunk_ground; y <= trunk_top_y; y++)
        worldgen_set_block_local(chunk, local_x, y, local_z, BLOCK_WOOD, true);

    /* Two-tier canopy: a 5x5 mid layer just below the top log, and a 3x3
     * crown on top. Corners of the 5x5 layer are skipped for a softer
     * silhouette. */
    int canopy_y = trunk_top_y;
    for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
            if ((dx == -2 || dx == 2) && (dz == -2 || dz == 2))
                continue; /* nip corners */
            worldgen_set_block_local(chunk, local_x + dx, canopy_y, local_z + dz,
                                     BLOCK_LEAVES, false);
        }
    }
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0)
                continue;
            worldgen_set_block_local(chunk, local_x + dx, canopy_y + 1,
                                     local_z + dz, BLOCK_LEAVES, false);
        }
    }
    worldgen_set_block_local(chunk, local_x, canopy_y + 2, local_z,
                             BLOCK_LEAVES, false);
}

static void generate_chunk_terrain(Chunk *chunk, uint32_t base_seed,
                                   int stone_tries_per_chunk)
{
    /* stone_tries_per_chunk is retained as a save-metadata field for
     * forward/backward compatibility with the old random-stone gen but is
     * unused by the heightmap-driven path. */
    (void)stone_tries_per_chunk;

    clear_chunk_blocks(chunk);

    int origin_x = chunk->chunk_x * WORLD_CHUNK_SIZE;
    int origin_z = chunk->chunk_z * WORLD_CHUNK_SIZE;

    /* Pass 1: stratified terrain column for every (lx, lz) in this chunk. */
    for (int lz = 0; lz < WORLD_CHUNK_SIZE; lz++) {
        for (int lx = 0; lx < WORLD_CHUNK_SIZE; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;
            int surface_y = worldgen_surface_height(wx, wz, base_seed);
            bool underwater = surface_y < WORLDGEN_SEA_LEVEL;

            for (int y = 0; y <= surface_y; y++) {
                BlockID b;
                if (y == surface_y) {
                    b = underwater ? BLOCK_DIRT : BLOCK_GRASS;
                } else if (y >= surface_y - 3) {
                    b = BLOCK_DIRT;
                } else {
                    b = BLOCK_STONE;
                }
                chunk->blocks[y][lz][lx] = b;
            }

            if (underwater) {
                for (int y = surface_y + 1; y <= WORLDGEN_SEA_LEVEL; y++)
                    chunk->blocks[y][lz][lx] = BLOCK_WATER;
            }
        }
    }

    /* Pass 2: trees. Scan a band that extends WORLDGEN_TREE_RADIUS past the
     * chunk edge so canopies whose trunks are in a neighbor still drop their
     * leaves into this chunk, giving seamless forests across chunk borders.
     * Trunk-only blocks outside the chunk are clipped by
     * worldgen_set_block_local. */
    for (int lz = -WORLDGEN_TREE_RADIUS;
         lz < WORLD_CHUNK_SIZE + WORLDGEN_TREE_RADIUS; lz++) {
        for (int lx = -WORLDGEN_TREE_RADIUS;
             lx < WORLD_CHUNK_SIZE + WORLDGEN_TREE_RADIUS; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;

            if (!worldgen_wants_tree(wx, wz, base_seed))
                continue;

            int surface_y = worldgen_surface_height(wx, wz, base_seed);
            if (surface_y < WORLDGEN_TREE_MIN_Y)
                continue;
            int trunk_top_y = surface_y + 4 +
                              (int)(hash_world_coord(wx, wz, base_seed,
                                                     0xfeedu) & 1u);
            if (trunk_top_y + 2 >= WORLD_CHUNK_HEIGHT)
                continue;

            uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xd00du);
            worldgen_place_tree(chunk, lx, trunk_top_y, lz, roll);
        }
    }
}

/*
 * A face between `current` and `neighbor` should be emitted when the
 * neighbor doesn't fully occlude it:
 *   - Any face next to air is visible.
 *   - An opaque block's face next to glass / water / leaves is visible:
 *     you can see the opaque surface through the transparent neighbor.
 *   - A translucent block (glass, water) next to anything but air is
 *     hidden, which collapses interior walls of solid translucent volumes
 *     into a single hull and avoids alpha-over-alpha overdraw artefacts
 *     when looking at translucent stacks.
 */
static bool face_should_render(BlockID current, BlockID neighbor)
{
    if (neighbor == BLOCK_AIR)
        return true;
    if (block_is_translucent(current))
        return false;
    /* Hide alpha-keyed faces against the same block: leaf canopies stack
     * dense enough that emitting every leaf-vs-leaf interior face was
     * doubling chunk quad counts and tanking FPS. Adjacent leaves share
     * the same texture and lighting, so the interior face was never the
     * one the camera actually sees. */
    if (current == neighbor && block_is_alpha_keyed(current))
        return false;
    return block_is_transparent(neighbor);
}

void world_init(VoxelWorld *world)
{
    memset(world, 0, sizeof(*world));
    world->near_chunk_radius = DEFAULT_NEAR_CHUNK_RADIUS;
    world->stream_chunks_per_frame = DEFAULT_STREAM_CHUNKS_PER_FRAME;
    if (pthread_mutex_init(&world->world_mu, NULL) == 0)
        world->world_mu_initialized = true;
}

void world_free(VoxelWorld *world)
{
    if (!world)
        return;

    free_chunk_storage(world);
    if (world->world_mu_initialized) {
        pthread_mutex_destroy(&world->world_mu);
        world->world_mu_initialized = false;
    }
    memset(world, 0, sizeof(*world));
}

void world_lock(VoxelWorld *world)
{
    if (world && world->world_mu_initialized)
        pthread_mutex_lock(&world->world_mu);
}

void world_unlock(VoxelWorld *world)
{
    if (world && world->world_mu_initialized)
        pthread_mutex_unlock(&world->world_mu);
}

static void world_lock_for_worker(VoxelWorld *world)
{
#ifdef __linux__
    while (world &&
           atomic_load_explicit(&world->foreground_lock_requests,
                                memory_order_acquire) > 0)
        sched_yield();
#endif
    world_lock(world);
}

Chunk *world_get_chunk_mut_locked(VoxelWorld *world, int chunk_x, int chunk_z)
{
    int index;

    if (!world)
        return NULL;
    index = chunk_lookup_find_index(world, chunk_x, chunk_z);
    if (index < 0 || index >= world->chunk_count)
        return NULL;
    if (!(world->chunks[index].flags & CHUNK_FLAG_LOADED))
        return NULL;
    return &world->chunks[index];
}

void chunk_mesh_free_retired(Chunk *chunk)
{
    ChunkMesh *retired;

    if (!chunk)
        return;

    retired = atomic_exchange_explicit(&chunk->retired_mesh, NULL,
                                       memory_order_acq_rel);
    if (!retired)
        return;

    free(retired->faces);
    free(retired);
}

static ChunkMesh *chunk_mesh_alloc(int face_count)
{
    ChunkMesh *mesh = calloc(1, sizeof(*mesh));
    if (!mesh)
        return NULL;

    if (face_count > 0) {
        mesh->faces = malloc((size_t)face_count * sizeof(*mesh->faces));
        if (!mesh->faces) {
            free(mesh);
            return NULL;
        }
    }
    mesh->face_count = face_count;
    return mesh;
}

/* Atomically publish a freshly-built ChunkMesh as the chunk's live_mesh.
 * Old live_mesh moves into retired_mesh; any prior retired_mesh is freed
 * here under the assumption that no reader still holds it (renderer reads
 * are bounded to a single frame's draw pass). */
static void chunk_publish_mesh(Chunk *chunk, ChunkMesh *new_mesh)
{
    ChunkMesh *prev_retired;
    ChunkMesh *prev_live;

    prev_retired = atomic_exchange_explicit(&chunk->retired_mesh, NULL,
                                            memory_order_acq_rel);
    if (prev_retired) {
        free(prev_retired->faces);
        free(prev_retired);
    }

    prev_live = atomic_exchange_explicit(&chunk->live_mesh, new_mesh,
                                         memory_order_acq_rel);
    atomic_store_explicit(&chunk->retired_mesh, prev_live,
                          memory_order_release);
}

const Chunk *world_get_chunk(const VoxelWorld *world, int chunk_x, int chunk_z)
{
    int index = chunk_lookup_find_index(world, chunk_x, chunk_z);

    if (index < 0)
        return NULL;

    return &world->chunks[index];
}

static Chunk *world_get_chunk_mut(VoxelWorld *world, int chunk_x, int chunk_z)
{
    int index = chunk_lookup_find_index(world, chunk_x, chunk_z);

    if (index < 0)
        return NULL;

    return &world->chunks[index];
}

BlockID world_get_block(const VoxelWorld *world, int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return BLOCK_AIR;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return BLOCK_AIR;

    return chunk->blocks[wy][lz][lx];
}

__attribute__((unused))
static uint8_t world_get_sky_light(const VoxelWorld *world, int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 0;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return 0;

    return chunk->sky_light[wy][lz][lx];
}

static uint8_t world_get_block_light(const VoxelWorld *world, int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 0;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return 0;

    return chunk->block_light[wy][lz][lx];
}

__attribute__((unused))
static bool world_set_sky_light(VoxelWorld *world, int wx, int wy, int wz,
                                uint8_t value)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    chunk->sky_light[wy][lz][lx] = value;
    return true;
}

static bool world_set_block_light(VoxelWorld *world, int wx, int wy, int wz,
                                  uint8_t value)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    chunk->block_light[wy][lz][lx] = value;
    return true;
}

/* 3×3 around (ccx,ccz): nb[ix][iz] = chunk at (ccx+ix-1, ccz+iz-1), or NULL.
 * Avoids repeated open-addressing probes in rebuild_chunk_faces / face counts. */
static void fill_neighbor_chunk_cache(const VoxelWorld *world, int ccx, int ccz,
                                      const Chunk *nb[3][3])
{
    for (int ix = 0; ix < 3; ix++) {
        for (int iz = 0; iz < 3; iz++)
            nb[ix][iz] = world_get_chunk(world, ccx + ix - 1, ccz + iz - 1);
    }
}

static BlockID read_block_cached(const Chunk *nb[3][3], int ccx, int ccz,
                                 int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return BLOCK_AIR;

    int cx = floor_div(wx, WORLD_CHUNK_SIZE);
    int cz = floor_div(wz, WORLD_CHUNK_SIZE);
    int ix = cx - ccx + 1;
    int iz = cz - ccz + 1;

    if (ix < 0 || ix > 2 || iz < 0 || iz > 2)
        return BLOCK_AIR;

    const Chunk *c = nb[ix][iz];

    if (!c || !(c->flags & CHUNK_FLAG_LOADED))
        return BLOCK_AIR;

    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);

    return c->blocks[wy][lz][lx];
}

static uint8_t read_sky_light_cached(const Chunk *nb[3][3], int ccx, int ccz,
                                     int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 0;

    int cx = floor_div(wx, WORLD_CHUNK_SIZE);
    int cz = floor_div(wz, WORLD_CHUNK_SIZE);
    int ix = cx - ccx + 1;
    int iz = cz - ccz + 1;

    if (ix < 0 || ix > 2 || iz < 0 || iz > 2)
        return 0;

    const Chunk *c = nb[ix][iz];

    if (!c || !(c->flags & CHUNK_FLAG_LOADED))
        return 0;

    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);

    return c->sky_light[wy][lz][lx];
}

static uint8_t read_block_light_cached(const Chunk *nb[3][3], int ccx, int ccz,
                                       int wx, int wy, int wz)
{
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 0;

    int cx = floor_div(wx, WORLD_CHUNK_SIZE);
    int cz = floor_div(wz, WORLD_CHUNK_SIZE);
    int ix = cx - ccx + 1;
    int iz = cz - ccz + 1;

    if (ix < 0 || ix > 2 || iz < 0 || iz > 2)
        return 0;

    const Chunk *c = nb[ix][iz];

    if (!c || !(c->flags & CHUNK_FLAG_LOADED))
        return 0;

    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);

    return c->block_light[wy][lz][lx];
}

static void mark_all_loaded_chunks_mesh_dirty(VoxelWorld *world)
{
    if (!world)
        return;

    for (int i = 0; i < world->chunk_count; i++) {
        if (world->chunks[i].flags & CHUNK_FLAG_LOADED)
            mark_chunk_mesh_dirty(&world->chunks[i]);
    }
}

static void clear_world_lighting(VoxelWorld *world)
{
    if (!world)
        return;

    for (int i = 0; i < world->chunk_count; i++) {
        if (world->chunks[i].flags & CHUNK_FLAG_LOADED)
            clear_chunk_lighting(&world->chunks[i]);
    }
}

static bool light_queue_push(LightNode **queue,
                             size_t *queue_capacity,
                             size_t *queue_tail,
                             LightNode node)
{
    if (*queue_tail >= *queue_capacity) {
        size_t new_capacity = (*queue_capacity == 0) ? 256 : (*queue_capacity * 2);
        LightNode *grown = realloc(*queue, new_capacity * sizeof(*grown));

        if (!grown)
            return false;

        *queue = grown;
        *queue_capacity = new_capacity;
    }

    (*queue)[(*queue_tail)++] = node;
    return true;
}


typedef struct {
    int wx;
    int wy;
    int wz;
    uint8_t val;
} LightRemovalNode;

static bool light_removal_queue_push(LightRemovalNode **queue,
                                     size_t *queue_capacity,
                                     size_t *queue_tail,
                                     LightRemovalNode node)
{
    if (*queue_tail >= *queue_capacity) {
        size_t new_capacity = (*queue_capacity == 0) ? 256 : (*queue_capacity * 2);
        LightRemovalNode *grown = realloc(*queue, new_capacity * sizeof(*grown));

        if (!grown)
            return false;

        *queue = grown;
        *queue_capacity = new_capacity;
    }

    (*queue)[(*queue_tail)++] = node;
    return true;
}


static void mark_chunk_and_adjacent_dirty_for_block(VoxelWorld *world, int wx, int wz)
{
    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    Chunk *c = world_get_chunk_mut(world, chunk_x, chunk_z);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);

    if (c)
        c->flags |= CHUNK_FLAG_MESH_DIRTY;

    if (lx == 0) {
        Chunk *n = world_get_chunk_mut(world, chunk_x - 1, chunk_z);
        if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY;
    } else if (lx == WORLD_CHUNK_SIZE - 1) {
        Chunk *n = world_get_chunk_mut(world, chunk_x + 1, chunk_z);
        if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY;
    }

    if (lz == 0) {
        Chunk *n = world_get_chunk_mut(world, chunk_x, chunk_z - 1);
        if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY;
    } else if (lz == WORLD_CHUNK_SIZE - 1) {
        Chunk *n = world_get_chunk_mut(world, chunk_x, chunk_z + 1);
        if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY;
    }
}

static bool propagate_block_light(VoxelWorld *world, LightNode **queue,
                                  size_t *capacity, size_t head, size_t *tail)
{
    while (head < *tail) {
        LightNode node = (*queue)[head++];
        uint8_t current = world_get_block_light(world, node.wx, node.wy, node.wz);

        if (current <= 1)
            continue;

        for (int face = 0; face < NUM_FACES; face++) {
            int nwx = node.wx + FACE_NX[face];
            int nwy = node.wy + FACE_NY[face];
            int nwz = node.wz + FACE_NZ[face];
            uint8_t next = (uint8_t)(current - 1);

            if (nwy < 0 || nwy >= WORLD_CHUNK_HEIGHT)
                continue;
            if (block_blocks_light(world_get_block(world, nwx, nwy, nwz)))
                continue;
            if (world_get_block_light(world, nwx, nwy, nwz) >= next)
                continue;
            if (!world_set_block_light(world, nwx, nwy, nwz, next))
                continue;

            mark_chunk_and_adjacent_dirty_for_block(world, nwx, nwz);

            if (!light_queue_push(queue, capacity, tail,
                                  (LightNode){ .wx = nwx, .wy = nwy, .wz = nwz })) {
                return false;
            }
        }
    }
    return true;
}

static bool remove_block_light(VoxelWorld *world,
                               LightRemovalNode **rem_queue, size_t *rem_cap, size_t rem_head, size_t *rem_tail,
                               LightNode **add_queue, size_t *add_cap, size_t *add_tail)
{
    while (rem_head < *rem_tail) {
        LightRemovalNode node = (*rem_queue)[rem_head++];

        for (int face = 0; face < NUM_FACES; face++) {
            int nwx = node.wx + FACE_NX[face];
            int nwy = node.wy + FACE_NY[face];
            int nwz = node.wz + FACE_NZ[face];

            if (nwy < 0 || nwy >= WORLD_CHUNK_HEIGHT)
                continue;

            uint8_t neighbor_level = world_get_block_light(world, nwx, nwy, nwz);
            if (neighbor_level != 0 && neighbor_level < node.val) {
                world_set_block_light(world, nwx, nwy, nwz, 0);
                mark_chunk_and_adjacent_dirty_for_block(world, nwx, nwz);

                if (!light_removal_queue_push(rem_queue, rem_cap, rem_tail,
                                              (LightRemovalNode){ .wx = nwx, .wy = nwy, .wz = nwz, .val = neighbor_level })) {
                    return false;
                }
            } else if (neighbor_level >= node.val) {
                if (!light_queue_push(add_queue, add_cap, add_tail,
                                      (LightNode){ .wx = nwx, .wy = nwy, .wz = nwz })) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void update_column_sky_light(VoxelWorld *world, int wx, int wz)
{
    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);
    int lx;
    int lz;
    uint8_t sky = 15;
    bool changed = false;

    if (!chunk)
        return;

    lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    lz = positive_mod(wz, WORLD_CHUNK_SIZE);

    for (int y = WORLD_CHUNK_HEIGHT - 1; y >= 0; y--) {
        if (block_blocks_light(chunk->blocks[y][lz][lx])) {
            sky = 0;
        }
        if (chunk->sky_light[y][lz][lx] != sky) {
            chunk->sky_light[y][lz][lx] = sky;
            changed = true;
        }
    }

    if (changed) {
        mark_chunk_and_adjacent_dirty_for_block(world, wx, wz);
    }
}

/*
 * Per-chunk snapshot used by world_rebuild_lighting_locked to detect which
 * chunks actually had a lighting delta after a full rebuild. Without this,
 * every rebuild marks the entire 9x9 window mesh-dirty, causing ~17 frames
 * of mesh-worker churn even when nothing actually changed (stable world,
 * stationary lights). With it, an idle rebuild marks zero chunks.
 */
typedef struct {
    uint8_t sky_light[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    uint8_t block_light[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE][WORLD_CHUNK_SIZE];
    bool prior_dirty;
    bool changed;
} LightRebuildSnap;

static bool world_rebuild_lighting_locked(VoxelWorld *world)
{
    LightNode *queue = NULL;
    size_t queue_head = 0;
    size_t queue_tail = 0;
    size_t queue_capacity = 0;
    LightRebuildSnap *snaps = NULL;
    bool diff_dirty_marking = false;

    if (!world)
        return false;
    if (world->chunk_count <= 0)
        return true;

    /*
     * Snapshot light arrays + prior MESH_DIRTY before clearing/rebuilding.
     * We then clear MESH_DIRTY on loaded chunks so any in-BFS calls to
     * mark_chunk_and_adjacent_dirty_for_block don't leak through - those
     * fire on every cell write from clear -> rebuilt and would mark most
     * of the window dirty regardless of whether the *final* light values
     * actually differ. Post-rebuild we re-mark dirty only where memcmp
     * shows an actual delta (or a cardinal neighbor's lighting moved,
     * since face shading samples across chunk boundaries).
     *
     * On allocation failure we fall back to the old "mark everything
     * dirty" behavior so a low-memory state never silently drops mesh
     * updates.
     */
    snaps = malloc((size_t)world->chunk_count * sizeof(*snaps));
    if (snaps) {
        diff_dirty_marking = true;
        for (int i = 0; i < world->chunk_count; i++) {
            Chunk *chunk = &world->chunks[i];

            snaps[i].prior_dirty = !!(chunk->flags & CHUNK_FLAG_MESH_DIRTY);
            snaps[i].changed = false;
            if (chunk->flags & CHUNK_FLAG_LOADED) {
                memcpy(snaps[i].sky_light, chunk->sky_light,
                       sizeof(snaps[i].sky_light));
                memcpy(snaps[i].block_light, chunk->block_light,
                       sizeof(snaps[i].block_light));
                chunk->flags &= ~CHUNK_FLAG_MESH_DIRTY;
            }
        }
    }

    clear_world_lighting(world);

    /*
     * Sky columns are fully contained in each chunk's (lx,lz) column — no
     * cross-chunk vertical dependency. Walking chunk arrays avoids two
     * hash-table lookups per cell (world_get_block + world_set_sky_light).
     */
    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        rebuild_chunk_sky_lighting(chunk);
    }

    world->has_light_emitters = false;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    BlockID id = chunk->blocks[y][z][x];
                    uint8_t emission = block_emission_level(id);

                    if (emission == 0)
                        continue;

                    world->has_light_emitters = true;
                    chunk->block_light[y][z][x] = emission;
                    if (!light_queue_push(&queue, &queue_capacity, &queue_tail,
                                          (LightNode){
                                              .wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x,
                                              .wy = y,
                                              .wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z,
                                          })) {
                        free(queue);
                        free(snaps);
                        return false;
                    }
                }
            }
        }
    }

    if (!propagate_block_light(world, &queue, &queue_capacity, queue_head, &queue_tail)) {
        free(queue);
        free(snaps);
        return false;
    }

    free(queue);

    if (diff_dirty_marking) {
        for (int i = 0; i < world->chunk_count; i++) {
            Chunk *chunk = &world->chunks[i];

            if (!(chunk->flags & CHUNK_FLAG_LOADED))
                continue;
            if (memcmp(chunk->sky_light, snaps[i].sky_light,
                       sizeof(snaps[i].sky_light)) != 0 ||
                memcmp(chunk->block_light, snaps[i].block_light,
                       sizeof(snaps[i].block_light)) != 0) {
                snaps[i].changed = true;
            }
        }

        bool any_dirty = false;
        static const int dx[4] = { -1, 1, 0, 0 };
        static const int dz[4] = { 0, 0, -1, 1 };
        for (int i = 0; i < world->chunk_count; i++) {
            Chunk *chunk = &world->chunks[i];

            if (!(chunk->flags & CHUNK_FLAG_LOADED))
                continue;

            bool dirty = snaps[i].prior_dirty || snaps[i].changed;
            if (!dirty) {
                for (int d = 0; d < 4; d++) {
                    int nidx = chunk_lookup_find_index(world,
                                                       chunk->chunk_x + dx[d],
                                                       chunk->chunk_z + dz[d]);
                    if (nidx >= 0 && snaps[nidx].changed) {
                        dirty = true;
                        break;
                    }
                }
            }
            if (dirty) {
                chunk->flags |= CHUNK_FLAG_MESH_DIRTY;
                any_dirty = true;
            }
        }
        free(snaps);
        world->meshes_dirty = any_dirty;
    } else {
        mark_all_loaded_chunks_mesh_dirty(world);
        world->meshes_dirty = true;
    }
    return true;
}

static bool world_handle_stream_lighting_locked(VoxelWorld *world,
                                                bool initial_stream,
                                                bool window_changed_or_filled)
{
    if (!world || !world->has_light_emitters)
        return true;

    if (initial_stream)
        return world_rebuild_lighting_locked(world);

    if (window_changed_or_filled)
        world->lighting_dirty = true;

    return true;
}

bool world_rebuild_lighting(VoxelWorld *world)
{
    bool ok;

    if (!world)
        return false;

    world_lock(world);
    ok = world_rebuild_lighting_locked(world);
    world_unlock(world);
    return ok;
}

static int count_exposed_faces_for_chunk_nb(const VoxelWorld *world, const Chunk *chunk,
                                              const Chunk *nb[3][3])
{
    (void)world;
    int ccx = chunk->chunk_x;
    int ccz = chunk->chunk_z;
    int count = 0;

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                BlockID id = chunk->blocks[y][z][x];

                if (id == BLOCK_AIR)
                    continue;

                int wx = ccx * WORLD_CHUNK_SIZE + x;
                int wz = ccz * WORLD_CHUNK_SIZE + z;

                for (int f = 0; f < NUM_FACES; f++) {
                    BlockID neighbor = read_block_cached(nb, ccx, ccz,
                                                         wx + FACE_NX[f],
                                                         y + FACE_NY[f],
                                                         wz + FACE_NZ[f]);
                    if (face_should_render(id, neighbor))
                        count++;
                }
            }
        }
    }

    return count;
}

static void face_cell_to_block(BlockFace face, int layer, int u, int v,
                               int *x, int *y, int *z)
{
    switch (face) {
    case FACE_TOP:
    case FACE_BOTTOM:
        *x = u;
        *y = layer;
        *z = v;
        break;
    case FACE_LEFT:
    case FACE_RIGHT:
        *x = layer;
        *y = v;
        *z = u;
        break;
    case FACE_FRONT:
    case FACE_BACK:
        *x = u;
        *y = v;
        *z = layer;
        break;
    default:
        *x = 0;
        *y = 0;
        *z = 0;
        break;
    }
}

static void face_grid_dims(BlockFace face, int *layers, int *width, int *height)
{
    switch (face) {
    case FACE_TOP:
    case FACE_BOTTOM:
        *layers = WORLD_CHUNK_HEIGHT;
        *width = WORLD_CHUNK_SIZE;   /* u axis: x */
        *height = WORLD_CHUNK_SIZE;  /* v axis: z */
        break;
    case FACE_LEFT:
    case FACE_RIGHT:
        *layers = WORLD_CHUNK_SIZE;
        *width = WORLD_CHUNK_SIZE;   /* u axis: z */
        *height = WORLD_CHUNK_HEIGHT;/* v axis: y */
        break;
    case FACE_FRONT:
    case FACE_BACK:
        *layers = WORLD_CHUNK_SIZE;
        *width = WORLD_CHUNK_SIZE;   /* u axis: x */
        *height = WORLD_CHUNK_HEIGHT;/* v axis: y */
        break;
    default:
        *layers = 0;
        *width = 0;
        *height = 0;
        break;
    }
}

static void append_chunk_face(ChunkFace *faces, int *out,
                              int x, int y, int z,
                              BlockFace face, BlockID type,
                              int u_size, int v_size,
                              uint8_t sky_light, uint8_t block_light)
{
    faces[(*out)++] = (ChunkFace){
        .x = (uint8_t)x,
        .y = (uint8_t)y,
        .z = (uint8_t)z,
        .face = (uint8_t)face,
        .type = (uint8_t)type,
        .u_size = (uint8_t)u_size,
        .v_size = (uint8_t)v_size,
        .sky_light = sky_light,
        .block_light = block_light,
    };
}

static bool ensure_chunk_face_capacity(Chunk *chunk, int needed)
{
    if (needed <= chunk->face_capacity)
        return true;

    ChunkFace *faces = realloc(chunk->faces, (size_t)needed * sizeof(*faces));
    if (!faces)
        return false;

    chunk->faces = faces;
    chunk->face_capacity = needed;
    return true;
}

/* Build a freshly-allocated ChunkMesh from `chunk`'s blocks/lighting and
 * its neighbor cache `nb`. Does NOT publish to chunk->live_mesh and does
 * NOT update chunk->flags - those are the caller's responsibility under
 * world_mu. The function may grow chunk->faces (the rebuild scratch) and
 * write chunk->face_count.
 *
 * Split out from rebuild_chunk_faces so the mesh worker can run the heavy
 * greedy meshing on a snapshot Chunk without holding world_mu, and only
 * re-acquire the lock briefly to publish the result. */
static ChunkMesh *chunk_build_mesh_unpublished(Chunk *chunk,
                                                const Chunk *nb[3][3],
                                                int ccx, int ccz,
                                                bool is_near)
{
    int max_face_count;
    int out = 0;

    if (!chunk)
        return NULL;

    max_face_count = count_exposed_faces_for_chunk_nb(NULL, chunk, nb);
    if (max_face_count == 0) {
        ChunkMesh *empty = chunk_mesh_alloc(0);
        if (!empty)
            return NULL;
        empty->generation = chunk->generation;
        empty->meshed_near = is_near;
        chunk->face_count = 0;
        return empty;
    }

    if (!ensure_chunk_face_capacity(chunk, max_face_count))
        return NULL;

    for (int f = 0; f < NUM_FACES; f++) {
        int layers;
        int width;
        int height;

        face_grid_dims((BlockFace)f, &layers, &width, &height);
        for (int layer = 0; layer < layers; layer++) {
            BlockID mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE] = {{0}};
            uint8_t sky_mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE] = {{0}};
            uint8_t block_mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE] = {{0}};
            bool used[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE] = {{0}};

            for (int v = 0; v < height; v++) {
                for (int u = 0; u < width; u++) {
                    int x, y, z;
                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);

                    BlockID id = chunk->blocks[y][z][x];
                    if (id == BLOCK_AIR)
                        continue;

                    int wx = ccx * WORLD_CHUNK_SIZE + x;
                    int wz = ccz * WORLD_CHUNK_SIZE + z;
                    BlockID neighbor = read_block_cached(nb, ccx, ccz,
                                                         wx + FACE_NX[f],
                                                         y + FACE_NY[f],
                                                         wz + FACE_NZ[f]);
                    if (face_should_render(id, neighbor)) {
                        int light_wx = wx + FACE_NX[f];
                        int light_wy = y + FACE_NY[f];
                        int light_wz = wz + FACE_NZ[f];

                        mask[v][u] = id;
                        sky_mask[v][u] = read_sky_light_cached(nb, ccx, ccz,
                                                               light_wx, light_wy, light_wz);
                        block_mask[v][u] = read_block_light_cached(nb, ccx, ccz,
                                                                   light_wx, light_wy, light_wz);
                        if (block_emission_level(id) > block_mask[v][u])
                            block_mask[v][u] = block_emission_level(id);
                    }
                }
            }

            for (int v = 0; v < height; v++) {
                for (int u = 0; u < width; u++) {
                    BlockID id = mask[v][u];
                    uint8_t sky_light = sky_mask[v][u];
                    uint8_t block_light = block_mask[v][u];
                    int merge_w = 1;
                    int merge_h = 1;
                    int x, y, z;

                    if (id == BLOCK_AIR || used[v][u])
                        continue;

                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);
                    if (block_is_translucent(id)) {
                        used[v][u] = true;
                        append_chunk_face(chunk->faces, &out, x, y, z,
                                          (BlockFace)f, id, 1, 1,
                                          sky_light, block_light);
                        continue;
                    }

                    /*
                     * Greedy merging introduces T-junctions and wide-quad UV
                     * shimmer. In some near-horizontal viewing angles the
                     * merged-quad path also dropped whole rows of top-faces
                     * (greedy run + frustum cull on the merged anchor combine
                     * to hide blocks that should be visible). Default to
                     * unit-quad emission everywhere; set BLOCK_GAME_GREEDY_MESH=1
                     * to opt back into the far-chunk merge for raw face-count
                     * wins on huge render distances.
                     */
                    if (!is_near && greedy_meshing_enabled()) {
                        while (u + merge_w < width &&
                               !used[v][u + merge_w] &&
                               mask[v][u + merge_w] == id &&
                               sky_mask[v][u + merge_w] == sky_light &&
                               block_mask[v][u + merge_w] == block_light) {
                            merge_w++;
                        }

                        bool can_extend = true;
                        while (v + merge_h < height && can_extend) {
                            for (int du = 0; du < merge_w; du++) {
                                if (used[v + merge_h][u + du] ||
                                    mask[v + merge_h][u + du] != id ||
                                    sky_mask[v + merge_h][u + du] != sky_light ||
                                    block_mask[v + merge_h][u + du] != block_light) {
                                    can_extend = false;
                                    break;
                                }
                            }
                            if (can_extend)
                                merge_h++;
                        }

                        for (int dv = 0; dv < merge_h; dv++) {
                            for (int du = 0; du < merge_w; du++)
                                used[v + dv][u + du] = true;
                        }
                    } else {
                        used[v][u] = true;
                    }

                    append_chunk_face(chunk->faces, &out, x, y, z,
                                      (BlockFace)f, id, merge_w, merge_h,
                                      sky_light, block_light);
                }
            }
        }
    }

    chunk->face_count = out;

    ChunkMesh *snapshot = chunk_mesh_alloc(out);
    if (!snapshot)
        return NULL;
    if (out > 0)
        memcpy(snapshot->faces, chunk->faces,
               (size_t)out * sizeof(ChunkFace));
    snapshot->generation = chunk->generation;
    snapshot->meshed_near = is_near;
    return snapshot;
}

/* Publish a freshly-built mesh and update flag bits. Caller must hold
 * world_mu so the flag writes don't race with main-thread edits. */
static void apply_built_mesh_locked(Chunk *chunk, ChunkMesh *mesh, bool is_near)
{
    chunk_publish_mesh(chunk, mesh);
    chunk->flags &= ~CHUNK_FLAG_MESH_DIRTY;
    chunk->flags |= CHUNK_FLAG_MESH_READY;
    if (is_near)
        chunk->flags |= CHUNK_FLAG_MESHED_NEAR;
    else
        chunk->flags &= ~CHUNK_FLAG_MESHED_NEAR;
}

static bool rebuild_chunk_faces(VoxelWorld *world, Chunk *chunk)
{
    const Chunk *nb[3][3];

    if (!world || !chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    fill_neighbor_chunk_cache(world, chunk->chunk_x, chunk->chunk_z, nb);
    bool is_near = chunk_is_near(world, chunk);

    ChunkMesh *mesh = chunk_build_mesh_unpublished(chunk, nb,
                                                    chunk->chunk_x,
                                                    chunk->chunk_z,
                                                    is_near);
    if (!mesh)
        return false;

    apply_built_mesh_locked(chunk, mesh, is_near);
    return true;
}

bool world_rebuild_and_publish_mesh(VoxelWorld *world, Chunk *chunk)
{
    return rebuild_chunk_faces(world, chunk);
}

/* === Snapshot mesh worker support ===
 *
 * Lets the mesh worker do the heavy greedy-meshing work without holding
 * world_mu. Lock discipline:
 *   1. Lock world, copy self + 4 cardinal neighbor block/light arrays into
 *      a worker-owned scratch. Capture is_near + generation. Unlock.
 *      We keep full neighbor snapshots for correctness; a border-only copy
 *      attempt produced seam artifacts and no measurable FPS win.
 *   2. Build the mesh from the scratch with no lock held. This is the
 *      expensive part (5-15 ms on the SoC).
 *   3. Re-lock briefly to publish via atomic_exchange and flag-update,
 *      after re-validating that the chunk wasn't evicted/regenerated
 *      while we were unlocked. Discard the mesh if so.
 *
 * The diagonal nb[3][3] corners are unused by rebuild (FACE_NX/NY/NZ are
 * cardinal only), so the scratch only stores 4 cardinal neighbors. */

struct ChunkMeshWorkerScratch {
    Chunk self;
    Chunk neighbors[4];
    bool nb_present[4];
};

static const int NB_DX[4] = {-1, +1,  0,  0};
static const int NB_DZ[4] = { 0,  0, -1, +1};
static const int NB_NB_IX[4] = {0, 2, 1, 1};
static const int NB_NB_IZ[4] = {1, 1, 0, 2};

ChunkMeshWorkerScratch *chunk_mesh_worker_scratch_create(void)
{
    return calloc(1, sizeof(ChunkMeshWorkerScratch));
}

void chunk_mesh_worker_scratch_destroy(ChunkMeshWorkerScratch *scratch)
{
    if (!scratch)
        return;
    free(scratch->self.faces);
    free(scratch);
}

static void copy_chunk_data_to_scratch(Chunk *dest, const Chunk *src)
{
    memcpy(dest->blocks, src->blocks, sizeof(dest->blocks));
    memcpy(dest->sky_light, src->sky_light, sizeof(dest->sky_light));
    memcpy(dest->block_light, src->block_light, sizeof(dest->block_light));
    dest->chunk_x = src->chunk_x;
    dest->chunk_z = src->chunk_z;
    dest->generation = src->generation;
    dest->flags = CHUNK_FLAG_LOADED;
}

bool world_run_mesh_job(VoxelWorld *world,
                        ChunkMeshWorkerScratch *scratch,
                        int chunk_x, int chunk_z,
                        uint32_t expected_generation)
{
    if (!world || !scratch)
        return false;

    bool is_near;
    uint32_t generation;

    /* Phase 1: snapshot under lock. */
    world_lock_for_worker(world);
    Chunk *target = world_get_chunk_mut_locked(world, chunk_x, chunk_z);
    if (!target ||
        target->generation != expected_generation ||
        !(target->flags & CHUNK_FLAG_LOADED)) {
        if (target)
            target->flags &= ~CHUNK_FLAG_MESH_QUEUED;
        world_unlock(world);
        return false;
    }

    copy_chunk_data_to_scratch(&scratch->self, target);
    for (int i = 0; i < 4; i++) {
        Chunk *nb_chunk = world_get_chunk_mut_locked(world,
                                                     chunk_x + NB_DX[i],
                                                     chunk_z + NB_DZ[i]);
        if (nb_chunk && (nb_chunk->flags & CHUNK_FLAG_LOADED)) {
            copy_chunk_data_to_scratch(&scratch->neighbors[i], nb_chunk);
            scratch->nb_present[i] = true;
        } else {
            scratch->neighbors[i].flags = 0;
            scratch->nb_present[i] = false;
        }
    }
    is_near = chunk_is_near(world, target);
    generation = target->generation;
    world_unlock(world);

    /* Phase 2: build mesh outside the lock. */
    const Chunk *nb_arr[3][3];
    for (int ix = 0; ix < 3; ix++)
        for (int iz = 0; iz < 3; iz++)
            nb_arr[ix][iz] = NULL;
    nb_arr[1][1] = &scratch->self;
    for (int i = 0; i < 4; i++) {
        if (scratch->nb_present[i])
            nb_arr[NB_NB_IX[i]][NB_NB_IZ[i]] = &scratch->neighbors[i];
    }

    ChunkMesh *mesh = chunk_build_mesh_unpublished(&scratch->self, nb_arr,
                                                    chunk_x, chunk_z,
                                                    is_near);
    if (!mesh) {
        world_lock_for_worker(world);
        target = world_get_chunk_mut_locked(world, chunk_x, chunk_z);
        if (target)
            target->flags &= ~CHUNK_FLAG_MESH_QUEUED;
        world_unlock(world);
        return false;
    }

    /* Phase 3: re-validate and publish under lock. Discard if the chunk
     * was evicted or its generation advanced (an edit landed mid-build). */
    bool published = false;
    world_lock_for_worker(world);
    target = world_get_chunk_mut_locked(world, chunk_x, chunk_z);
    if (target &&
        target->generation == generation &&
        (target->flags & CHUNK_FLAG_LOADED)) {
        apply_built_mesh_locked(target, mesh, is_near);
        target->flags &= ~CHUNK_FLAG_MESH_QUEUED;
        world->meshes_rebuilt_last_stream++;
        published = true;
    } else {
        free(mesh->faces);
        free(mesh);
        if (target)
            target->flags &= ~CHUNK_FLAG_MESH_QUEUED;
    }
    world_unlock(world);
    return published;
}

static void mark_near_far_transitions_dirty(VoxelWorld *world)
{
    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        bool is_near = chunk_is_near(world, chunk);
        bool was_near = (chunk->flags & CHUNK_FLAG_MESHED_NEAR) != 0;

        if (is_near != was_near)
            chunk->flags |= CHUNK_FLAG_MESH_DIRTY;
    }
}

void world_set_stream_chunks_per_frame(VoxelWorld *world, int chunks_per_frame)
{
    if (!world)
        return;

    if (chunks_per_frame < 0)
        chunks_per_frame = 0;
    if (chunks_per_frame > STREAM_CHUNKS_PER_FRAME_MAX)
        chunks_per_frame = STREAM_CHUNKS_PER_FRAME_MAX;

    world_lock(world);
    world->stream_chunks_per_frame = chunks_per_frame;
    world_unlock(world);
}

int world_stream_chunks_per_frame(const VoxelWorld *world)
{
    if (!world)
        return DEFAULT_STREAM_CHUNKS_PER_FRAME;
    return world->stream_chunks_per_frame;
}

void world_set_near_chunk_radius(VoxelWorld *world, int radius)
{
    if (!world)
        return;

    if (radius < 0)
        radius = 0;
    if (radius > world->render_distance_chunks)
        radius = world->render_distance_chunks;

    world_lock(world);
    if (world->near_chunk_radius == radius) {
        world_unlock(world);
        return;
    }

    world->near_chunk_radius = radius;
    mark_near_far_transitions_dirty(world);
    world->meshes_dirty = world_has_dirty_meshes_locked(world);
    world_unlock(world);
}

int world_near_chunk_radius(const VoxelWorld *world)
{
    if (!world)
        return DEFAULT_NEAR_CHUNK_RADIUS;
    return world->near_chunk_radius;
}

void world_set_render_distance(VoxelWorld *world, int distance)
{
    if (!world || distance < 1)
        return;

    world_lock(world);
    if (world->render_distance_chunks == distance) {
        world_unlock(world);
        return;
    }

    world->render_distance_chunks = distance;
    world->load_radius_chunks = distance + 1;

    /* Clamp near_chunk_radius to the new render distance. */
    if (world->near_chunk_radius > distance)
        world->near_chunk_radius = distance;

    /* Invalidate the cached window so the next world_stream_around call
     * takes the full re-stream branch. Setting chunks_x/z to 0 ensures
     * the same_window check in stream_world_to_chunk_center fails. */
    world->chunks_x = 0;
    world->chunks_z = 0;

    world_unlock(world);
}

int world_render_distance(const VoxelWorld *world)
{
    if (!world)
        return 0;
    return world->render_distance_chunks;
}

static bool world_has_dirty_meshes_locked(const VoxelWorld *world)
{
    if (!world)
        return false;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if ((chunk->flags & CHUNK_FLAG_LOADED) &&
            (chunk->flags & CHUNK_FLAG_MESH_DIRTY))
            return true;
    }

    return false;
}

/* 0 or unset: rebuild every dirty chunk in one pass (desktop default).
 * Positive: cap per world_stream / world_set_block / idle-tick pass so
 * chunk lighting + mesh work does not blow one frame on slow CPUs (FPGA). */
static int mesh_rebuild_chunks_per_pass(void)
{
    const char *value = getenv("VOXEL_MESH_REBUILDS_PER_FRAME");
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0')
        return 0;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed < 1)
        return 0;
    if (parsed > 4096)
        return 4096;

    return (int)parsed;
}

static bool world_rebuild_dirty_meshes_locked_with_limit(VoxelWorld *world,
                                                         int limit)
{
    bool ok = true;
    int rebuilt = 0;

    if (!world)
        return false;
    if (limit <= 0)
        limit = INT_MAX;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED) ||
            !(chunk->flags & CHUNK_FLAG_MESH_DIRTY) ||
            (chunk->flags & CHUNK_FLAG_MESH_QUEUED))
            continue;
        /* Defer if any neighbor is still LOADING - meshing now would
         * see those neighbors as AIR (per world_get_block) and require
         * a re-mesh as soon as the neighbor finalizes, which is the
         * O(neighbors) amplification that crushed FPS during async
         * chunk-load waves. The neighbor's finalize re-marks us dirty. */
        if (world_chunk_has_loading_neighbor_locked(world,
                                                    chunk->chunk_x,
                                                    chunk->chunk_z))
            continue;

        if (rebuild_chunk_faces(world, chunk)) {
            world->meshes_rebuilt_last_stream++;
        } else {
            ok = false;
        }

        rebuilt++;
        if (rebuilt >= limit)
            break;
    }

    world->meshes_dirty = world_has_dirty_meshes_locked(world);
    return ok;
}

static bool world_rebuild_dirty_meshes_locked(VoxelWorld *world)
{
    int per_pass = mesh_rebuild_chunks_per_pass();
    int limit = (per_pass <= 0) ? INT_MAX : per_pass;

    return world_rebuild_dirty_meshes_locked_with_limit(world, limit);
}

bool world_rebuild_dirty_meshes(VoxelWorld *world)
{
    bool ok;

    if (!world)
        return false;

    world_lock(world);
    ok = world_rebuild_dirty_meshes_locked(world);
    world_unlock(world);
    return ok;
}

static bool ensure_chunk_storage(VoxelWorld *world, int diameter)
{
    int chunk_capacity = diameter * diameter;

    if (diameter <= 0)
        return false;

    if (world->chunks && world->chunk_capacity == chunk_capacity) {
        world->chunks_x = diameter;
        world->chunks_z = diameter;
        return ensure_chunk_lookup_capacity(world, chunk_capacity);
    }

    free_chunk_storage(world);

    world->chunks = calloc((size_t)chunk_capacity, sizeof(*world->chunks));
    if (!world->chunks)
        return false;

    world->chunk_capacity = chunk_capacity;
    world->chunks_x = diameter;
    world->chunks_z = diameter;
    return ensure_chunk_lookup_capacity(world, chunk_capacity);
}

static void retain_chunks_in_window(VoxelWorld *world,
                                    int origin_chunk_x,
                                    int origin_chunk_z,
                                    int diameter)
{
    int left = 0;
    int right;
    int old_count;

    if (!world || world->chunk_count <= 0)
        return;

    old_count = world->chunk_count;
    right = old_count - 1;

    while (left <= right) {
        Chunk *chunk = &world->chunks[left];
        bool keep = chunk_slot_is_present(chunk) &&
                    chunk_in_window(chunk->chunk_x, chunk->chunk_z,
                                    origin_chunk_x, origin_chunk_z,
                                    diameter);

        if (keep) {
            chunk->last_used_epoch = world->stream_epoch;
            world->chunks_reused_last_stream++;
            left++;
            continue;
        }

        release_chunk(chunk);

        while (left < right) {
            Chunk *tail = &world->chunks[right];
            bool tail_keep = chunk_slot_is_present(tail) &&
                             chunk_in_window(tail->chunk_x, tail->chunk_z,
                                             origin_chunk_x, origin_chunk_z,
                                             diameter);

            if (tail_keep)
                break;
            release_chunk(tail);
            right--;
        }

        if (left < right) {
            world->chunks[left] = world->chunks[right];
            memset(&world->chunks[right], 0, sizeof(world->chunks[right]));
            world->chunks[left].last_used_epoch = world->stream_epoch;
            world->chunks_reused_last_stream++;
            left++;
            right--;
        } else {
            right--;
        }
    }

    for (int i = left; i < old_count; i++)
        memset(&world->chunks[i], 0, sizeof(world->chunks[i]));

    world->chunk_count = left;
}

static bool stream_generate_chunk(VoxelWorld *world, int chunk_x, int chunk_z)
{
    if (chunk_lookup_find_index(world, chunk_x, chunk_z) >= 0)
        return true;
    if (world->chunk_count >= world->chunk_capacity)
        return false;

    Chunk *chunk = &world->chunks[world->chunk_count];

    if (world->async_chunk_gen_enabled) {
        /* Allocate a placeholder slot only. The gen worker will run
         * terrain gen + snapshot load + sky lighting off the main
         * thread, then call world_finalize_async_chunk_load to install
         * the result and flip LOADING -> LOADED. */
        initialize_chunk_slot_pending(chunk, chunk_x, chunk_z,
                                      world->stream_epoch);
        world->chunk_count++;
        if (!chunk_lookup_insert(world, world->chunk_count - 1)) {
            world->chunk_count--;
            memset(chunk, 0, sizeof(*chunk));
            return false;
        }
        world->chunks_generated_last_stream++;
        return true;
    }

    initialize_chunk_slot(chunk, chunk_x, chunk_z, world->stream_epoch);
    generate_chunk_terrain(chunk,
                           world->procedural_seed,
                           world->stone_tries_per_chunk);
    if (!load_chunk_snapshot(world, chunk))
        return false;
    rebuild_chunk_sky_lighting(chunk);
    if (chunk_has_light_emitters(chunk))
        world->has_light_emitters = true;

    world->chunk_count++;
    if (!chunk_lookup_insert(world, world->chunk_count - 1))
        return false;

    world->chunks_generated_last_stream++;
    mark_chunk_and_neighbors_dirty(world, chunk_x, chunk_z);
    return true;
}

static bool stream_fill_missing_chunks(VoxelWorld *world,
                                       int center_chunk_x,
                                       int center_chunk_z,
                                       int origin_chunk_x,
                                       int origin_chunk_z,
                                       int diameter,
                                       bool initial_stream)
{
    int limit;
    int generated = 0;

    if (!world)
        return false;

    limit = (initial_stream || world->stream_chunks_per_frame <= 0) ?
            INT_MAX : world->stream_chunks_per_frame;

    for (int radius = 0;
         radius <= world->load_radius_chunks && generated < limit;
         radius++) {
        for (int dz = -radius; dz <= radius && generated < limit; dz++) {
            for (int dx = -radius; dx <= radius && generated < limit; dx++) {
                int abs_dx = dx < 0 ? -dx : dx;
                int abs_dz = dz < 0 ? -dz : dz;
                int cx;
                int cz;

                if (abs_dx != radius && abs_dz != radius)
                    continue;

                cx = center_chunk_x + dx;
                cz = center_chunk_z + dz;
                if (!chunk_in_window(cx, cz, origin_chunk_x,
                                     origin_chunk_z, diameter))
                    continue;
                if (chunk_lookup_find_index(world, cx, cz) >= 0)
                    continue;
                if (!stream_generate_chunk(world, cx, cz))
                    return false;
                generated++;
            }
        }
    }

    return true;
}

bool world_async_chunk_gen_offline(const VoxelWorld *world,
                                   int chunk_x, int chunk_z,
                                   ChunkGenResult *out)
{
    if (!world || !out)
        return false;

    /* Heap-allocate scratch: the Chunk struct is large (>24 KB with
     * blocks + lighting + atomics) and this function may run on a
     * worker thread with a smaller default stack. */
    Chunk *scratch = calloc(1, sizeof(*scratch));
    if (!scratch)
        return false;

    scratch->chunk_x = chunk_x;
    scratch->chunk_z = chunk_z;
    /* Set LOADED so rebuild_chunk_sky_lighting / chunk_has_light_emitters
     * accept the scratch buffer. The flag is local to this Chunk and
     * never published anywhere. */
    scratch->flags = CHUNK_FLAG_LOADED;

    generate_chunk_terrain(scratch,
                           world->procedural_seed,
                           world->stone_tries_per_chunk);
    /* load_chunk_snapshot only reads world->persistence_enabled and
     * world->save_root, both immutable post-init, so no lock needed. */
    if (!load_chunk_snapshot(world, scratch)) {
        free(scratch);
        return false;
    }
    rebuild_chunk_sky_lighting(scratch);

    memcpy(out->blocks, scratch->blocks, sizeof(out->blocks));
    memcpy(out->sky_light, scratch->sky_light, sizeof(out->sky_light));
    out->has_light_emitters = chunk_has_light_emitters(scratch);

    free(scratch);
    return true;
}

bool world_finalize_async_chunk_load(VoxelWorld *world,
                                     int chunk_x, int chunk_z,
                                     uint32_t generation,
                                     const ChunkGenResult *result)
{
    bool finalized = false;

    if (!world || !result)
        return false;

    world_lock_for_worker(world);

    int idx = chunk_lookup_find_index(world, chunk_x, chunk_z);
    if (idx >= 0) {
        Chunk *chunk = &world->chunks[idx];

        if (chunk->generation == generation &&
            (chunk->flags & CHUNK_FLAG_LOADING) &&
            !(chunk->flags & CHUNK_FLAG_LOADED)) {
            bool needs_light_rebuild =
                world->has_light_emitters || result->has_light_emitters;

            memcpy(chunk->blocks, result->blocks, sizeof(chunk->blocks));
            memcpy(chunk->sky_light, result->sky_light,
                   sizeof(chunk->sky_light));
            memset(chunk->block_light, 0, sizeof(chunk->block_light));
            chunk->flags &= ~(CHUNK_FLAG_LOADING | CHUNK_FLAG_GEN_QUEUED);
            chunk->flags |= CHUNK_FLAG_LOADED | CHUNK_FLAG_MESH_DIRTY;
            if (result->has_light_emitters)
                world->has_light_emitters = true;
            if (needs_light_rebuild)
                world->lighting_dirty = true;
            mark_chunk_and_neighbors_dirty(world, chunk_x, chunk_z);
            world->meshes_dirty = true;
            finalized = true;
        } else if (chunk->generation == generation &&
                   (chunk->flags & CHUNK_FLAG_GEN_QUEUED)) {
            /* Stale: slot was finalized through some other path. Just
             * clear the in-flight bit so a future re-stream can re-queue. */
            chunk->flags &= ~CHUNK_FLAG_GEN_QUEUED;
        }
    }

    world_unlock(world);
    return finalized;
}

static bool stream_world_to_chunk_center(VoxelWorld *world,
                                         int center_chunk_x,
                                         int center_chunk_z)
{
    int diameter;
    int origin_chunk_x;
    int origin_chunk_z;

    if (!world)
        return false;

    diameter = world->load_radius_chunks * 2 + 1;
    origin_chunk_x = center_chunk_x - world->load_radius_chunks;
    origin_chunk_z = center_chunk_z - world->load_radius_chunks;
    world->chunks_generated_last_stream = 0;
    world->chunks_reused_last_stream = 0;
    world->meshes_rebuilt_last_stream = 0;

    if (!ensure_chunk_storage(world, diameter))
        return false;

    bool same_window =
        world->center_chunk_x == center_chunk_x &&
        world->center_chunk_z == center_chunk_z &&
        world->origin_chunk_x == origin_chunk_x &&
        world->origin_chunk_z == origin_chunk_z &&
        world->chunks_x == diameter &&
        world->chunks_z == diameter;

    if (same_window) {
        if (world->chunk_count > 0 &&
            chunk_lookup_find_index(world,
                                    world->chunks[0].chunk_x,
                                    world->chunks[0].chunk_z) < 0 &&
            !rebuild_chunk_lookup(world))
            return false;
        world->chunks_reused_last_stream = world->chunk_count;

        if (world->chunk_count < world->chunk_capacity) {
            if (!stream_fill_missing_chunks(world,
                                            center_chunk_x, center_chunk_z,
                                            origin_chunk_x, origin_chunk_z,
                                            diameter, false))
                return false;
            if (!world_handle_stream_lighting_locked(
                    world, false, world->chunks_generated_last_stream > 0))
                return false;
        }

        if (world->async_mesh_rebuilds_enabled) {
            world->meshes_dirty = world_has_dirty_meshes_locked(world);
            return true;
        }
        return world_rebuild_dirty_meshes_locked(world);
    }

    world->stream_epoch++;
    if (world->stream_epoch == 0)
        world->stream_epoch = 1;

    int old_origin_x = world->origin_chunk_x;
    int old_origin_z = world->origin_chunk_z;
    bool initial_stream = world->chunk_count == 0;

    if (!persist_chunks_outside_window(world,
                                       origin_chunk_x, origin_chunk_z,
                                       diameter))
        return false;
    retain_chunks_in_window(world, origin_chunk_x, origin_chunk_z, diameter);
    if (!rebuild_chunk_lookup(world))
        return false;

    world->origin_chunk_x = origin_chunk_x;
    world->origin_chunk_z = origin_chunk_z;
    world->center_chunk_x = center_chunk_x;
    world->center_chunk_z = center_chunk_z;

    if (!stream_fill_missing_chunks(world,
                                    center_chunk_x, center_chunk_z,
                                    origin_chunk_x, origin_chunk_z,
                                    diameter, initial_stream))
        return false;

    if (!world_handle_stream_lighting_locked(world, initial_stream, true))
        return false;

    mark_trailing_perimeter_dirty(world,
                                  old_origin_x, old_origin_z,
                                  origin_chunk_x, origin_chunk_z,
                                  diameter);
    mark_near_far_transitions_dirty(world);
    if (world->async_mesh_rebuilds_enabled) {
        world->meshes_dirty = world_has_dirty_meshes_locked(world);
        return true;
    }
    if (initial_stream)
        return world_rebuild_dirty_meshes_locked_with_limit(world, INT_MAX);
    return world_rebuild_dirty_meshes_locked(world);
}

bool world_init_infinite_procedural(VoxelWorld *world,
                                    uint32_t seed,
                                    int stone_tries_per_chunk,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z,
                                    const char *save_root)
{
    if (!world || render_distance_chunks < 0)
        return false;

    free_chunk_storage(world);

    world->procedural_seed = seed;
    world->stone_tries_per_chunk = stone_tries_per_chunk;
    world->render_distance_chunks = render_distance_chunks;
    world->near_chunk_radius =
        env_int_clamped("VOXEL_NEAR_CHUNK_RADIUS", NULL,
                        DEFAULT_NEAR_CHUNK_RADIUS, 0,
                        render_distance_chunks);
    world->stream_chunks_per_frame =
        env_int_clamped("VOXEL_CHUNKS_PER_FRAME", "VOXEL_CHUNK_PER_FRAME",
                        DEFAULT_STREAM_CHUNKS_PER_FRAME, 0,
                        STREAM_CHUNKS_PER_FRAME_MAX);
    /* Keep a one-chunk procedural border around the visible radius so
     * exposed-face caches stay correct at the render edge. */
    world->load_radius_chunks = render_distance_chunks + 1;
    if (!initialize_world_persistence(world, save_root))
        return false;

    return stream_world_to_chunk_center(world,
                                        chunk_coord_from_world(center_x),
                                        chunk_coord_from_world(center_z));
}

bool world_stream_around(VoxelWorld *world, float world_x, float world_z)
{
    bool ok;
    struct timespec lock_start;
    struct timespec lock_end;
    struct timespec body_end;

    if (!world)
        return false;

    clock_gettime(CLOCK_MONOTONIC, &lock_start);
    if (world->world_mu_initialized) {
        atomic_fetch_add_explicit(&world->foreground_lock_requests, 1,
                                  memory_order_acq_rel);
        pthread_mutex_lock(&world->world_mu);
    }
    clock_gettime(CLOCK_MONOTONIC, &lock_end);

    ok = stream_world_to_chunk_center(world,
                                      chunk_coord_from_world(world_x),
                                      chunk_coord_from_world(world_z));
    world_unlock(world);
    if (world->world_mu_initialized) {
        atomic_fetch_sub_explicit(&world->foreground_lock_requests, 1,
                                  memory_order_acq_rel);
    }
    clock_gettime(CLOCK_MONOTONIC, &body_end);
    world->last_stream_lock_wait_ns =
        timespec_diff_u64_ns(&lock_end, &lock_start);
    world->last_stream_body_ns =
        timespec_diff_u64_ns(&body_end, &lock_end);
    return ok;
}

bool world_flush(VoxelWorld *world)
{
    if (!world || !world->persistence_enabled)
        return true;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if ((chunk->flags & CHUNK_FLAG_LOADED) &&
            !save_chunk_snapshot(world, chunk))
            return false;
    }

    return true;
}

static bool world_set_block_locked(VoxelWorld *world, int wx, int wy, int wz, BlockID type)
{
    int chunk_x;
    int chunk_z;
    int lx;
    int lz;
    Chunk *chunk;
    BlockID old_type;
    uint8_t old_emission;
    uint8_t new_emission;
    uint8_t current_light;
    LightNode *add_queue = NULL;
    size_t add_cap = 0, add_head = 0, add_tail = 0;
    LightRemovalNode *rem_queue = NULL;
    size_t rem_cap = 0, rem_head = 0, rem_tail = 0;
    bool success = true;

    if (!world || wy < 0 || wy >= WORLD_CHUNK_HEIGHT ||
        type < BLOCK_AIR || type >= NUM_BLOCK_TYPES)
        return false;

    chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    chunk = world_get_chunk_mut(world, chunk_x, chunk_z);
    /* world_get_chunk_mut now also returns LOADING slots (so streaming
     * does not double-allocate). Block edits must only land on a fully
     * LOADED chunk - editing a LOADING slot would race with the gen
     * worker's pending finalize copy. */
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    old_type = chunk->blocks[wy][lz][lx];
    if (old_type == type)
        return true;

    chunk->blocks[wy][lz][lx] = type;
    chunk->flags |= CHUNK_FLAG_MODIFIED;
    chunk->generation++;

    mark_chunk_and_adjacent_dirty_for_block(world, wx, wz);
    update_column_sky_light(world, wx, wz);

    old_emission = block_emission_level(old_type);
    new_emission = block_emission_level(type);
    current_light = world_get_block_light(world, wx, wy, wz);

    bool light_blocked_changed = block_blocks_light(old_type) != block_blocks_light(type);
    bool emission_changed = old_emission != new_emission;

    if (light_blocked_changed || emission_changed) {
        if (current_light > 0) {
            world_set_block_light(world, wx, wy, wz, 0);
            success &= light_removal_queue_push(&rem_queue, &rem_cap, &rem_tail,
                                                (LightRemovalNode){.wx = wx, .wy = wy, .wz = wz, .val = current_light});
        }

        if (new_emission > 0) {
            world_set_block_light(world, wx, wy, wz, new_emission);
            success &= light_queue_push(&add_queue, &add_cap, &add_tail,
                                        (LightNode){.wx = wx, .wy = wy, .wz = wz});
        } else if (!block_blocks_light(type)) {
            for (int f = 0; f < NUM_FACES; f++) {
                int nx = wx + FACE_NX[f];
                int ny = wy + FACE_NY[f];
                int nz = wz + FACE_NZ[f];
                if (ny >= 0 && ny < WORLD_CHUNK_HEIGHT) {
                    uint8_t nl = world_get_block_light(world, nx, ny, nz);
                    if (nl > 1) {
                        success &= light_queue_push(&add_queue, &add_cap, &add_tail,
                                                    (LightNode){.wx = nx, .wy = ny, .wz = nz});
                    }
                }
            }
        }
    }

    if (success) {
        success &= remove_block_light(world, &rem_queue, &rem_cap, rem_head, &rem_tail,
                                      &add_queue, &add_cap, &add_tail);
        success &= propagate_block_light(world, &add_queue, &add_cap, add_head, &add_tail);
    }

    if (add_queue) free(add_queue);
    if (rem_queue) free(rem_queue);

    if (new_emission > 0)
        world->has_light_emitters = true;
    else if (old_emission > 0)
        world_recompute_light_emitters_locked(world);

    world->meshes_rebuilt_last_stream = 0;
    world->meshes_dirty = true;
    return success;
}

bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type)
{
    bool ok;

    if (!world)
        return false;

    world_lock(world);
    ok = world_set_block_locked(world, wx, wy, wz, type);
    world_unlock(world);
    return ok;
}

int world_total_faces(const VoxelWorld *world)
{
    int total = 0;

    if (!world)
        return 0;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];
        const ChunkMesh *mesh;

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        mesh = atomic_load_explicit(&chunk->live_mesh, memory_order_acquire);
        if (mesh)
            total += mesh->face_count;
    }

    return total;
}

int world_total_blocks(const VoxelWorld *world)
{
    int total = 0;

    if (!world)
        return 0;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    if (chunk->blocks[y][z][x] != BLOCK_AIR)
                        total++;
                }
            }
        }
    }

    return total;
}

int world_loaded_chunk_count(const VoxelWorld *world)
{
    int total = 0;

    if (!world)
        return 0;

    for (int i = 0; i < world->chunk_count; i++) {
        if (world->chunks[i].flags & CHUNK_FLAG_LOADED)
            total++;
    }

    return total;
}

int world_chunk_capacity(const VoxelWorld *world)
{
    return world ? world->chunk_capacity : 0;
}

/* -----------------------------------------------------------------------
 * Minecraft-style water simulation
 * -----------------------------------------------------------------------
 * Each BLOCK_WATER (source) block attempts to:
 *   1. Flow straight down if the block below is air.
 *   2. Spread laterally (±X, ±Z) up to WATER_MAX_LATERAL_DISTANCE steps,
 *      into air blocks that are at the same Y or lower than the source.
 *
 * Each BLOCK_WATER_FLOW block evaporates (becomes air) if none of its
 * 6 face-adjacent neighbours is a water block (source or flow).
 *
 * A BFS queue is used so each tick processes at most WATER_TICK_MAX_BLOCKS
 * newly-placed flow blocks, bounding the per-tick cost on slow hardware.
 * ----------------------------------------------------------------------- */
#define WATER_MAX_LATERAL_DISTANCE  7   /* Minecraft source level */
#define WATER_TICK_MAX_BLOCKS     512   /* max new flow blocks per tick */

static bool block_is_any_water(BlockID id)
{
    return id == BLOCK_WATER || id == BLOCK_WATER_FLOW;
}

static WaterTickStats g_water_tick_stats = {0};

WaterTickStats world_water_tick_stats(void)
{
    return g_water_tick_stats;
}

bool world_water_tick(VoxelWorld *world)
{
    if (!world)
        return false;

    /* Temporary BFS queue: (wx, wy, wz, lateral_dist_from_source). */
    typedef struct { int wx, wy, wz; int dist; } WaterNode;
    static WaterNode queue[WATER_TICK_MAX_BLOCKS];
    int head = 0, tail = 0;
    bool changed = false;
    WaterTickStats stats = {0};

    world_lock(world);

    /* ---- Pass 1: evaporate orphaned flow blocks ---- */
    for (int ci = 0; ci < world->chunk_count; ci++) {
        Chunk *chunk = &world->chunks[ci];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        int ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        int oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    if (chunk->blocks[y][z][x] != BLOCK_WATER_FLOW)
                        continue;

                    stats.flows_seen++;
                    int wx = ox + x, wy = y, wz = oz + z;
                    bool has_neighbor = false;

                    /* Check all 6 face neighbors for any water. */
                    const int dx[6] = {-1,1,0,0,0,0};
                    const int dy[6] = {0,0,-1,1,0,0};
                    const int dz[6] = {0,0,0,0,-1,1};
                    for (int f = 0; f < 6 && !has_neighbor; f++) {
                        BlockID nb = world_get_block(world,
                                         wx + dx[f], wy + dy[f], wz + dz[f]);
                        if (block_is_any_water(nb))
                            has_neighbor = true;
                    }

                    if (!has_neighbor) {
                        world_set_block_locked(world, wx, wy, wz, BLOCK_AIR);
                        changed = true;
                        stats.evaporated++;
                    }
                }
            }
        }
    }

    /* ---- Pass 2: spread from source blocks ---- */
    for (int ci = 0; ci < world->chunk_count; ci++) {
        Chunk *chunk = &world->chunks[ci];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        int ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        int oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    if (chunk->blocks[y][z][x] != BLOCK_WATER)
                        continue;

                    stats.sources_seen++;
                    if (tail >= WATER_TICK_MAX_BLOCKS)
                        goto spread_done;

                    /* Seed BFS from this source block (dist=0). */
                    queue[tail++] = (WaterNode){ox + x, y, oz + z, 0};
                }
            }
        }
    }

spread_done:
    head = 0;
    while (head < tail) {
        WaterNode node = queue[head++];
        int wx = node.wx, wy = node.wy, wz = node.wz;
        int dist = node.dist;

        /* --- Try to flow straight down first (gravity priority) --- */
        if (wy > 0) {
            BlockID below = world_get_block(world, wx, wy - 1, wz);
            if (below == BLOCK_AIR) {
                world_set_block_locked(world, wx, wy - 1, wz, BLOCK_WATER_FLOW);
                changed = true;
                stats.spread_placed++;
                /* Falling water resets lateral distance. */
                if (tail < WATER_TICK_MAX_BLOCKS)
                    queue[tail++] = (WaterNode){wx, wy - 1, wz, 0};
                /* When falling, skip lateral spread from this node. */
                continue;
            }
        }

        /* --- Lateral spread (only if within level 7 distance) --- */
        if (dist >= WATER_MAX_LATERAL_DISTANCE)
            continue;

        const int lx[4] = {-1, 1,  0, 0};
        const int lz[4] = { 0, 0, -1, 1};
        for (int d = 0; d < 4; d++) {
            int nx = wx + lx[d];
            int nz = wz + lz[d];
            if (world_get_block(world, nx, wy, nz) != BLOCK_AIR)
                continue;
            world_set_block_locked(world, nx, wy, nz, BLOCK_WATER_FLOW);
            changed = true;
            stats.spread_placed++;
            if (tail < WATER_TICK_MAX_BLOCKS)
                queue[tail++] = (WaterNode){nx, wy, nz, dist + 1};
        }
    }

    world_unlock(world);
    g_water_tick_stats = stats;
    return changed;
}
