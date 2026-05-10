#include "world.h"
#include "world_gen.h"
#include "env_util.h"

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
#define WORLD_META_VERSION 2u
#define WORLD_META_MIN_VERSION 1u
#define WORLD_GEN_FLAG_DESERT_LAVA_POOLS 0x00000001u
/* v2: appended water_level[H][Z][X] after the block grid for Minecraft-style
 * variable water height.
 * v3: appended redstone_data[H][Z][X] for per-cell redstone metadata such as
 * repeater delay. v2 loads still work and default redstone_data to zero. */
#define WORLD_CHUNK_VERSION 3u
#define WORLD_CHUNK_MIN_SUPPORTED_VERSION 2u
#define REDSTONE_BUTTON_PULSE_SECONDS 1.5f
#define REDSTONE_REPEATER_TICK_SECONDS 0.1f
#define REDSTONE_REPEATER_MIN_DELAY_TICKS 1u
#define REDSTONE_REPEATER_MAX_DELAY_TICKS 4u
#define REDSTONE_SETTLE_MAX_PASSES 16

/*
 * Save format v1:
 *   world.meta
 *     - fixed-size WorldSaveHeader
 *       - v2 names the trailing reserved field as generation_flags
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
    uint32_t generation_flags;
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

typedef struct {
    int wx;
    int wy;
    int wz;
} RedstoneCell;

typedef struct {
    int wx;
    int wy;
    int wz;
    BlockID type;
} RedstoneChange;

static bool chunk_in_window(int chunk_x, int chunk_z,
                            int origin_chunk_x, int origin_chunk_z,
                            int diameter);
static bool world_has_dirty_meshes_locked(const VoxelWorld *world);
static void mark_chunk_and_adjacent_dirty_for_block(VoxelWorld *world,
                                                    int wx,
                                                    int wz);

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

typedef struct {
    BlockID source;
    BlockID flow;
} FluidDef;

static const FluidDef WATER_FLUID = { BLOCK_WATER, BLOCK_WATER_FLOW };
static const FluidDef LAVA_FLUID = { BLOCK_LAVA, BLOCK_LAVA_FLOW };

static bool block_is_fluid_source(BlockID id)
{
    return id == BLOCK_WATER || id == BLOCK_LAVA;
}

static bool block_is_fluid_flow(BlockID id)
{
    return id == BLOCK_WATER_FLOW || id == BLOCK_LAVA_FLOW;
}

static int block_fluid_family(BlockID id)
{
    switch (id) {
    case BLOCK_WATER:
    case BLOCK_WATER_FLOW:
        return 1;
    case BLOCK_LAVA:
    case BLOCK_LAVA_FLOW:
        return 2;
    default:
        return 0;
    }
}

static bool block_is_any_fluid(BlockID id)
{
    return block_fluid_family(id) != 0;
}

static bool block_is_fluid_kind(BlockID id, const FluidDef *fluid)
{
    return fluid && (id == fluid->source || id == fluid->flow);
}

static bool block_is_gravity_affected(BlockID id)
{
    return id == BLOCK_SAND || id == BLOCK_GRAVEL;
}

static bool greedy_meshing_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = env_flag("BLOCK_GAME_GREEDY_MESH", true) ? 1 : 0;
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

static bool world_meta_version_supported(uint16_t version)
{
    return version >= WORLD_META_MIN_VERSION &&
           version <= WORLD_META_VERSION;
}

static uint32_t world_generation_flags(const VoxelWorld *world)
{
    if (!world)
        return 0;
    return world->desert_lava_pools_enabled ?
           WORLD_GEN_FLAG_DESERT_LAVA_POOLS : 0u;
}

static uint32_t world_header_generation_flags(const WorldSaveHeader *header)
{
    if (!header || header->version < 2u)
        return WORLD_GEN_FLAG_DESERT_LAVA_POOLS;
    return header->generation_flags;
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
        header.generation_flags = world_generation_flags(world);

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
        !world_meta_version_supported(header.version) ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT ||
        header.procedural_seed != world->procedural_seed ||
        header.stone_tries_per_chunk != (uint32_t)world->stone_tries_per_chunk ||
        world_header_generation_flags(&header) != world_generation_flags(world))
        return false;

    return true;
}

bool world_read_save_metadata(const char *save_root,
                              uint32_t *seed_out,
                              int *stone_tries_per_chunk_out,
                              bool *desert_lava_pools_enabled_out)
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
        !world_meta_version_supported(header.version) ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT)
        return false;

    if (seed_out)
        *seed_out = header.procedural_seed;
    if (stone_tries_per_chunk_out)
        *stone_tries_per_chunk_out = (int)header.stone_tries_per_chunk;
    if (desert_lava_pools_enabled_out)
        *desert_lava_pools_enabled_out =
            (world_header_generation_flags(&header) &
             WORLD_GEN_FLAG_DESERT_LAVA_POOLS) != 0u;
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
    size_t block_count = chunk_block_count();
    uint8_t *blocks;
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
    header.block_count = (uint32_t)block_count;

    blocks = malloc(block_count);
    if (!blocks)
        return false;

    size_t out = 0;
    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++)
                blocks[out++] = (uint8_t)chunk->blocks[y][z][x];
        }
    }

    file = fopen(temp_path, "wb");
    if (!file) {
        free(blocks);
        return false;
    }
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(blocks, block_count, 1, file) != 1 ||
        fwrite(chunk->water_level, sizeof(chunk->water_level), 1, file) != 1 ||
        fwrite(chunk->redstone_data, sizeof(chunk->redstone_data), 1, file) != 1) {
        fclose(file);
        unlink(temp_path);
        free(blocks);
        return false;
    }
    free(blocks);
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
    size_t block_count = chunk_block_count();
    uint8_t *blocks;
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

    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return false;
    }

    if (memcmp(header.magic, "VCHK", 4) != 0 ||
        header.version < WORLD_CHUNK_MIN_SUPPORTED_VERSION ||
        header.version > WORLD_CHUNK_VERSION ||
        header.chunk_x != chunk->chunk_x ||
        header.chunk_z != chunk->chunk_z ||
        header.chunk_size != WORLD_CHUNK_SIZE ||
        header.chunk_height != WORLD_CHUNK_HEIGHT ||
        header.block_count != block_count) {
        fclose(file);
        return false;
    }

    blocks = malloc(block_count);
    if (!blocks) {
        fclose(file);
        return false;
    }

    if (fread(blocks, block_count, 1, file) != 1 ||
        fread(chunk->water_level, sizeof(chunk->water_level), 1, file) != 1) {
        fclose(file);
        free(blocks);
        return false;
    }
    if (header.version >= 3u) {
        if (fread(chunk->redstone_data, sizeof(chunk->redstone_data), 1,
                  file) != 1) {
            fclose(file);
            free(blocks);
            return false;
        }
    } else {
        memset(chunk->redstone_data, 0, sizeof(chunk->redstone_data));
    }
    fclose(file);

    size_t in = 0;
    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                uint8_t id = blocks[in++];

                if (id >= NUM_BLOCK_TYPES) {
                    free(blocks);
                    return false;
                }
                chunk->blocks[y][z][x] = (BlockID)id;
                /* Sanitize water_level: only meaningful for fluid blocks. */
                if (block_is_fluid_source((BlockID)id))
                    chunk->water_level[y][z][x] = 0;
                else if (!block_is_fluid_flow((BlockID)id))
                    chunk->water_level[y][z][x] = 0;
                else if (chunk->water_level[y][z][x] < 1)
                    chunk->water_level[y][z][x] = 1;
                else if (chunk->water_level[y][z][x] > 7)
                    chunk->water_level[y][z][x] = 7;
                if (block_is_repeater((BlockID)id)) {
                    if (chunk->redstone_data[y][z][x] <
                        REDSTONE_REPEATER_MIN_DELAY_TICKS)
                        chunk->redstone_data[y][z][x] =
                            REDSTONE_REPEATER_MIN_DELAY_TICKS;
                    else if (chunk->redstone_data[y][z][x] >
                             REDSTONE_REPEATER_MAX_DELAY_TICKS)
                        chunk->redstone_data[y][z][x] =
                            REDSTONE_REPEATER_MAX_DELAY_TICKS;
                } else {
                    chunk->redstone_data[y][z][x] = 0;
                }
            }
        }
    }

    free(blocks);
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
    memset(chunk->water_level, 0, sizeof(chunk->water_level));
    memset(chunk->redstone_data, 0, sizeof(chunk->redstone_data));
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

static bool block_uses_cube_mesh(BlockID id)
{
    return block_render_model(id) == BLOCK_RENDER_CUBE;
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

static uint8_t world_get_redstone_data_locked(const VoxelWorld *world,
                                              int wx,
                                              int wy,
                                              int wz)
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

    return chunk->redstone_data[wy][lz][lx];
}

static bool world_set_redstone_data_locked(VoxelWorld *world,
                                           int wx,
                                           int wy,
                                           int wz,
                                           uint8_t value,
                                           bool mark_mesh_dirty)
{
    if (!world || wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);

    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;
    if (chunk->redstone_data[wy][lz][lx] == value)
        return true;

    chunk->redstone_data[wy][lz][lx] = value;
    chunk->flags |= CHUNK_FLAG_MODIFIED;
    if (mark_mesh_dirty) {
        chunk->generation++;
        mark_chunk_and_adjacent_dirty_for_block(world, wx, wz);
        world->meshes_rebuilt_last_stream = 0;
        world->meshes_dirty = true;
    }
    return true;
}

uint8_t world_repeater_delay_ticks(const VoxelWorld *world,
                                   int wx,
                                   int wy,
                                   int wz)
{
    uint8_t delay;

    if (!world || !block_is_repeater(world_get_block(world, wx, wy, wz)))
        return 0;
    delay = world_get_redstone_data_locked(world, wx, wy, wz);
    if (delay < REDSTONE_REPEATER_MIN_DELAY_TICKS)
        return REDSTONE_REPEATER_MIN_DELAY_TICKS;
    if (delay > REDSTONE_REPEATER_MAX_DELAY_TICKS)
        return REDSTONE_REPEATER_MAX_DELAY_TICKS;
    return delay;
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

static uint8_t read_water_level_cached(const Chunk *nb[3][3], int ccx, int ccz,
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

    return c->water_level[wy][lz][lx];
}

static uint8_t fluid_flow_height_from_level(uint8_t level)
{
    if (level > 7)
        level = 7;

    uint8_t height = (uint8_t)(8 - level);
    return height ? height : 1;
}

static uint8_t fluid_height_cached(const Chunk *nb[3][3], int ccx, int ccz,
                                   int wx, int wy, int wz)
{
    BlockID id = read_block_cached(nb, ccx, ccz, wx, wy, wz);
    int family = block_fluid_family(id);

    if (block_is_fluid_source(id))
        return 8;
    if (!block_is_fluid_flow(id))
        return 0;

    if (wy + 1 < WORLD_CHUNK_HEIGHT &&
        block_fluid_family(read_block_cached(nb, ccx, ccz,
                                             wx, wy + 1, wz)) == family)
        return 8;

    return fluid_flow_height_from_level(
        read_water_level_cached(nb, ccx, ccz, wx, wy, wz));
}

static bool mesh_face_should_render(const Chunk *nb[3][3], int ccx, int ccz,
                                    int wx, int wy, int wz, BlockFace face,
                                    BlockID current)
{
    BlockID neighbor = read_block_cached(nb, ccx, ccz,
                                         wx + FACE_NX[face],
                                         wy + FACE_NY[face],
                                         wz + FACE_NZ[face]);

    if (!block_is_any_fluid(current))
        return block_face_should_render(current, neighbor);

    if (neighbor == BLOCK_AIR)
        return true;
    if (block_fluid_family(neighbor) != block_fluid_family(current))
        return block_is_transparent(neighbor);

    switch (face) {
    case FACE_LEFT:
    case FACE_RIGHT:
    case FACE_FRONT:
    case FACE_BACK: {
        uint8_t current_height = fluid_height_cached(nb, ccx, ccz, wx, wy, wz);
        uint8_t neighbor_height = fluid_height_cached(nb, ccx, ccz,
                                                      wx + FACE_NX[face],
                                                      wy + FACE_NY[face],
                                                      wz + FACE_NZ[face]);
        return current_height > neighbor_height;
    }
    default:
        return false;
    }
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
     * Sky columns are fully contained in each chunk's (lx,lz) column. Walking
     * chunk arrays avoids hash-table probes per cell.
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

                if (!block_uses_cube_mesh(id)) {
                    count += block_render_model(id) == BLOCK_RENDER_FLAT ? 1 : 2;
                    continue;
                }

                int wx = ccx * WORLD_CHUNK_SIZE + x;
                int wz = ccz * WORLD_CHUNK_SIZE + z;

                for (int f = 0; f < NUM_FACES; f++) {
                    if (mesh_face_should_render(nb, ccx, ccz, wx, y, wz,
                                                (BlockFace)f, id))
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
                              uint8_t face, BlockID type,
                              int u_size, int v_size,
                              uint8_t sky_light, uint8_t block_light,
                              uint8_t height)
{
    faces[(*out)++] = (ChunkFace){
        .x = (uint8_t)x,
        .y = (uint8_t)y,
        .z = (uint8_t)z,
        .face = face,
        .type = (uint8_t)type,
        .u_size = (uint8_t)u_size,
        .v_size = (uint8_t)v_size,
        .sky_light = sky_light,
        .block_light = block_light,
        .height = height,
    };
}

typedef struct {
    BlockID mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE];
    uint8_t sky_mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE];
    uint8_t block_mask[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE];
    bool used[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE];
} FaceBuildScratch;

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

static bool append_chunk_face_checked(Chunk *chunk, int *out,
                                      int x, int y, int z,
                                      uint8_t face, BlockID type,
                                      int u_size, int v_size,
                                      uint8_t sky_light, uint8_t block_light,
                                      uint8_t height)
{
    int needed;

    if (!chunk || !out)
        return false;

    needed = *out + 1;
    if (needed < *out || !ensure_chunk_face_capacity(chunk, needed))
        return false;

    append_chunk_face(chunk->faces, out, x, y, z, face, type, u_size, v_size,
                      sky_light, block_light, height);
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
    FaceBuildScratch *scratch;
    bool ok = true;

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

    scratch = malloc(sizeof(*scratch));
    if (!scratch)
        return NULL;

    for (int f = 0; ok && f < NUM_FACES; f++) {
        int layers;
        int width;
        int height;

        face_grid_dims((BlockFace)f, &layers, &width, &height);
        for (int layer = 0; ok && layer < layers; layer++) {
            memset(scratch, 0, sizeof(*scratch));

            for (int v = 0; ok && v < height; v++) {
                for (int u = 0; ok && u < width; u++) {
                    int x, y, z;
                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);

                    BlockID id = chunk->blocks[y][z][x];
                    if (id == BLOCK_AIR || !block_uses_cube_mesh(id))
                        continue;

                    int wx = ccx * WORLD_CHUNK_SIZE + x;
                    int wz = ccz * WORLD_CHUNK_SIZE + z;
                    if (mesh_face_should_render(nb, ccx, ccz, wx, y, wz,
                                                (BlockFace)f, id)) {
                        int light_wx = wx + FACE_NX[f];
                        int light_wy = y + FACE_NY[f];
                        int light_wz = wz + FACE_NZ[f];

                        scratch->mask[v][u] = id;
                        scratch->sky_mask[v][u] =
                            read_sky_light_cached(nb, ccx, ccz,
                                                  light_wx, light_wy, light_wz);
                        scratch->block_mask[v][u] =
                            read_block_light_cached(nb, ccx, ccz,
                                                    light_wx, light_wy, light_wz);
                        if (block_emission_level(id) >
                            scratch->block_mask[v][u])
                            scratch->block_mask[v][u] =
                                block_emission_level(id);
                    }
                }
            }

            for (int v = 0; ok && v < height; v++) {
                for (int u = 0; ok && u < width; u++) {
                    BlockID id = scratch->mask[v][u];
                    uint8_t sky_light = scratch->sky_mask[v][u];
                    uint8_t block_light = scratch->block_mask[v][u];
                    int merge_w = 1;
                    int merge_h = 1;
                    int x, y, z;

                    if (id == BLOCK_AIR || scratch->used[v][u])
                        continue;

                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);
                    if (block_is_translucent(id)) {
                        /* Fluid flows render at a partial height set by
                         * their water_level (0..7, where higher = thinner).
                         * Sources and all other translucent blocks get the
                         * full 8/8 height. */
                        uint8_t h = 8;
                        if (block_is_fluid_flow(id)) {
                            h = fluid_height_cached(nb, ccx, ccz,
                                                    ccx * WORLD_CHUNK_SIZE + x,
                                                    y,
                                                    ccz * WORLD_CHUNK_SIZE + z);
                        }
                        scratch->used[v][u] = true;
                        ok = append_chunk_face_checked(chunk, &out, x, y, z,
                                                       (uint8_t)f, id, 1, 1,
                                                       sky_light, block_light, h);
                        continue;
                    }

                    /*
                     * Keep nearby chunks as unit quads where T-junctions and
                     * UV shimmer are visible, but merge far opaque faces by
                     * default to keep the FPGA descriptor stream small. Set
                     * BLOCK_GAME_GREEDY_MESH=0 to disable the far-chunk merge
                     * while investigating mesh artifacts.
                     */
                    if (!is_near && greedy_meshing_enabled()) {
                        while (u + merge_w < width &&
                               !scratch->used[v][u + merge_w] &&
                               scratch->mask[v][u + merge_w] == id &&
                               scratch->sky_mask[v][u + merge_w] == sky_light &&
                               scratch->block_mask[v][u + merge_w] == block_light) {
                            merge_w++;
                        }

                        bool can_extend = true;
                        while (v + merge_h < height && can_extend) {
                            for (int du = 0; du < merge_w; du++) {
                                if (scratch->used[v + merge_h][u + du] ||
                                    scratch->mask[v + merge_h][u + du] != id ||
                                    scratch->sky_mask[v + merge_h][u + du] != sky_light ||
                                    scratch->block_mask[v + merge_h][u + du] != block_light) {
                                    can_extend = false;
                                    break;
                                }
                            }
                            if (can_extend)
                                merge_h++;
                        }

                        for (int dv = 0; dv < merge_h; dv++) {
                            for (int du = 0; du < merge_w; du++)
                                scratch->used[v + dv][u + du] = true;
                        }
                    } else {
                        scratch->used[v][u] = true;
                    }

                    ok = append_chunk_face_checked(chunk, &out, x, y, z,
                                                   (uint8_t)f, id,
                                                   merge_w, merge_h,
                                                   sky_light, block_light, 8);
                }
            }
        }
    }

    if (!ok) {
        free(scratch);
        return NULL;
    }

    for (int y = 0; ok && y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; ok && z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; ok && x < WORLD_CHUNK_SIZE; x++) {
                BlockID id = chunk->blocks[y][z][x];

                if (id == BLOCK_AIR || block_uses_cube_mesh(id))
                    continue;

                int wx = ccx * WORLD_CHUNK_SIZE + x;
                int wz = ccz * WORLD_CHUNK_SIZE + z;
                uint8_t sky_light = read_sky_light_cached(nb, ccx, ccz,
                                                          wx, y, wz);
                uint8_t block_light = read_block_light_cached(nb, ccx, ccz,
                                                              wx, y, wz);

                if (block_emission_level(id) > block_light)
                    block_light = block_emission_level(id);

                if (block_render_model(id) == BLOCK_RENDER_FLAT) {
                    uint8_t flat_state = block_is_repeater(id) ?
                                         chunk->redstone_data[y][z][x] : 0;

                    ok = append_chunk_face_checked(chunk, &out, x, y, z,
                                                   CHUNK_FACE_FLAT, id, 1, 1,
                                                   sky_light, block_light,
                                                   flat_state);
                } else {
                    ok = append_chunk_face_checked(chunk, &out, x, y, z,
                                                   CHUNK_FACE_CROSS_A, id,
                                                   1, 1, sky_light,
                                                   block_light, 8);
                    if (ok)
                        ok = append_chunk_face_checked(chunk, &out, x, y, z,
                                                       CHUNK_FACE_CROSS_B, id,
                                                       1, 1, sky_light,
                                                       block_light, 8);
                }
            }
        }
    }

    if (!ok) {
        free(scratch);
        return NULL;
    }

    chunk->face_count = out;

    ChunkMesh *snapshot = chunk_mesh_alloc(out);
    if (!snapshot) {
        free(scratch);
        return NULL;
    }
    if (out > 0)
        memcpy(snapshot->faces, chunk->faces,
               (size_t)out * sizeof(ChunkFace));
    snapshot->generation = chunk->generation;
    snapshot->meshed_near = is_near;
    free(scratch);
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
    memcpy(dest->water_level, src->water_level, sizeof(dest->water_level));
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

/* Default: cap to 1 dirty chunk per world_stream / world_set_block /
 * idle-tick pass so chunk lighting + mesh work does not blow one frame.
 * Set VOXEL_MESH_REBUILDS_PER_FRAME=0 to rebuild every dirty chunk in one pass.
 * Larger positive values raise the per-pass cap (max 4096). */
static int mesh_rebuild_chunks_per_pass(void)
{
    return env_int_capped_or_default("VOXEL_MESH_REBUILDS_PER_FRAME",
                                     1,
                                     0,
                                     4096);
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
    world_generate_chunk_terrain(chunk,
                                 world->procedural_seed,
                                 world->stone_tries_per_chunk,
                                 world->desert_lava_pools_enabled);
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

    world_generate_chunk_terrain(scratch,
                                 world->procedural_seed,
                                 world->stone_tries_per_chunk,
                                 world->desert_lava_pools_enabled);
    /* load_chunk_snapshot only reads world->persistence_enabled and
     * world->save_root, both immutable post-init, so no lock needed. */
    if (!load_chunk_snapshot(world, scratch)) {
        free(scratch);
        return false;
    }
    rebuild_chunk_sky_lighting(scratch);

    memcpy(out->blocks, scratch->blocks, sizeof(out->blocks));
    memcpy(out->sky_light, scratch->sky_light, sizeof(out->sky_light));
    memcpy(out->water_level, scratch->water_level, sizeof(out->water_level));
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
            memcpy(chunk->water_level, result->water_level,
                   sizeof(chunk->water_level));
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
                                    bool desert_lava_pools_enabled,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z,
                                    const char *save_root)
{
    if (!world || render_distance_chunks < 0)
        return false;

    free_chunk_storage(world);
    world->redstone_pulse_count = 0;
    memset(world->redstone_pulses, 0, sizeof(world->redstone_pulses));
    world->redstone_repeater_state_count = 0;
    memset(world->redstone_repeater_states, 0,
           sizeof(world->redstone_repeater_states));

    world->procedural_seed = seed;
    world->stone_tries_per_chunk = stone_tries_per_chunk;
    world->desert_lava_pools_enabled = desert_lava_pools_enabled;
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

    if (world->falling_block_count > 0)
        world_update_falling_blocks(world, 60.0f);

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

    /* Maintain fluid level on type changes. Sources are always level 0;
     * flow placed via the public API defaults to level 1 (caller is the
     * player or a tool, not the tick - the tick uses its own internal
     * helper that picks a level explicitly); anything else clears it. */
    if (block_is_fluid_source(type))
        chunk->water_level[wy][lz][lx] = 0;
    else if (block_is_fluid_flow(type))
        chunk->water_level[wy][lz][lx] = 1;
    else
        chunk->water_level[wy][lz][lx] = 0;

    if (block_is_repeater(type)) {
        if (!block_is_repeater(old_type) ||
            chunk->redstone_data[wy][lz][lx] <
                REDSTONE_REPEATER_MIN_DELAY_TICKS ||
            chunk->redstone_data[wy][lz][lx] >
                REDSTONE_REPEATER_MAX_DELAY_TICKS)
            chunk->redstone_data[wy][lz][lx] =
                REDSTONE_REPEATER_MIN_DELAY_TICKS;
    } else {
        chunk->redstone_data[wy][lz][lx] = 0;
    }

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

static bool redstone_block_is_wire(BlockID id)
{
    return id == BLOCK_REDSTONE_WIRE_UNCONNECTED ||
           id == BLOCK_REDSTONE_WIRE_OFF ||
           id == BLOCK_REDSTONE_WIRE_ON;
}

static bool redstone_block_has_state(BlockID id)
{
    return id == BLOCK_REDSTONE_TORCH_OFF ||
           id == BLOCK_REDSTONE_TORCH_ON ||
           block_is_repeater(id) ||
           block_is_comparator(id) ||
           id == BLOCK_LAMP_OFF ||
           id == BLOCK_LAMP;
}

static bool redstone_block_is_component(BlockID id)
{
    return redstone_block_is_wire(id) ||
           redstone_block_has_state(id) ||
           id == BLOCK_REDSTONE_BLOCK ||
           id == BLOCK_BUTTON;
}

static bool redstone_block_connects_to_wire(BlockID id)
{
    if (redstone_block_is_component(id))
        return true;
    return id != BLOCK_AIR &&
           block_render_model(id) == BLOCK_RENDER_CUBE &&
           !block_is_passable(id);
}

static bool redstone_block_can_hold_power(BlockID id)
{
    return id != BLOCK_AIR &&
           id != BLOCK_REDSTONE_BLOCK &&
           block_render_model(id) == BLOCK_RENDER_CUBE &&
           !block_is_passable(id);
}

static bool redstone_torch_support_locked(const VoxelWorld *world,
                                          int wx,
                                          int wy,
                                          int wz,
                                          RedstoneCell *support_out)
{
    RedstoneCell support;

    if (!world)
        return false;

    /* Torches do not have saved facing yet. Infer support from nearby solid
     * blocks, preferring floor support so old saves keep their original shape. */
    if (wy > 0 &&
        redstone_block_can_hold_power(world_get_block(world, wx, wy - 1, wz))) {
        support = (RedstoneCell){ .wx = wx, .wy = wy - 1, .wz = wz };
        if (support_out)
            *support_out = support;
        return true;
    }

    for (int face = FACE_LEFT; face <= FACE_BACK; face++) {
        int sx = wx + FACE_NX[face];
        int sz = wz + FACE_NZ[face];

        if (!redstone_block_can_hold_power(world_get_block(world, sx, wy, sz)))
            continue;
        support = (RedstoneCell){ .wx = sx, .wy = wy, .wz = sz };
        if (support_out)
            *support_out = support;
        return true;
    }

    return false;
}

static void redstone_facing_delta(BlockDoorFacing facing, int *dx, int *dz)
{
    int out_dx = 0;
    int out_dz = -1;

    switch (facing) {
    case BLOCK_DOOR_FACING_EAST:
        out_dx = 1;
        out_dz = 0;
        break;
    case BLOCK_DOOR_FACING_SOUTH:
        out_dx = 0;
        out_dz = 1;
        break;
    case BLOCK_DOOR_FACING_WEST:
        out_dx = -1;
        out_dz = 0;
        break;
    case BLOCK_DOOR_FACING_NORTH:
    default:
        break;
    }

    if (dx)
        *dx = out_dx;
    if (dz)
        *dz = out_dz;
}

static bool redstone_cell_push(RedstoneCell **cells,
                               size_t *count,
                               size_t *cap,
                               RedstoneCell cell)
{
    if (*count >= *cap) {
        size_t next_cap = *cap ? *cap * 2u : 64u;
        RedstoneCell *next_cells;

        if (next_cap < *cap)
            return false;
        next_cells = realloc(*cells, next_cap * sizeof(**cells));
        if (!next_cells)
            return false;
        *cells = next_cells;
        *cap = next_cap;
    }

    (*cells)[(*count)++] = cell;
    return true;
}

static bool redstone_change_push(RedstoneChange **changes,
                                 size_t *count,
                                 size_t *cap,
                                 RedstoneChange change)
{
    if (*count >= *cap) {
        size_t next_cap = *cap ? *cap * 2u : 32u;
        RedstoneChange *next_changes;

        if (next_cap < *cap)
            return false;
        next_changes = realloc(*changes, next_cap * sizeof(**changes));
        if (!next_changes)
            return false;
        *changes = next_changes;
        *cap = next_cap;
    }

    (*changes)[(*count)++] = change;
    return true;
}

static uint32_t redstone_coord_hash(int wx, int wy, int wz)
{
    uint32_t h = (uint32_t)wx * 0x9e3779b1u;
    h ^= (uint32_t)wz * 0x85ebca6bu;
    h ^= (uint32_t)wy * 0xc2b2ae35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static bool redstone_build_wire_table(const RedstoneCell *wires,
                                      size_t wire_count,
                                      int **table_out,
                                      size_t *table_cap_out)
{
    size_t table_cap = 1u;
    int *table;

    *table_out = NULL;
    *table_cap_out = 0;
    if (wire_count == 0)
        return true;
    if (wire_count > (size_t)INT_MAX)
        return false;

    while (table_cap < wire_count * 2u) {
        if (table_cap > SIZE_MAX / 2u)
            return false;
        table_cap *= 2u;
    }

    table = malloc(table_cap * sizeof(*table));
    if (!table)
        return false;
    for (size_t i = 0; i < table_cap; i++)
        table[i] = -1;

    for (size_t i = 0; i < wire_count; i++) {
        size_t slot = redstone_coord_hash(wires[i].wx,
                                          wires[i].wy,
                                          wires[i].wz) & (table_cap - 1u);
        while (table[slot] >= 0)
            slot = (slot + 1u) & (table_cap - 1u);
        table[slot] = (int)i;
    }

    *table_out = table;
    *table_cap_out = table_cap;
    return true;
}

static int redstone_wire_lookup(const RedstoneCell *wires,
                                const int *table,
                                size_t table_cap,
                                int wx,
                                int wy,
                                int wz)
{
    if (!table || table_cap == 0)
        return -1;

    size_t slot = redstone_coord_hash(wx, wy, wz) & (table_cap - 1u);
    for (size_t probe = 0; probe < table_cap; probe++) {
        int index = table[slot];

        if (index < 0)
            return -1;
        if (wires[index].wx == wx &&
            wires[index].wy == wy &&
            wires[index].wz == wz)
            return index;
        slot = (slot + 1u) & (table_cap - 1u);
    }

    return -1;
}

static uint8_t redstone_wire_power_to_adjacent_cell_locked(
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap,
    int source_wx,
    int source_wy,
    int source_wz,
    int target_wx,
    int target_wy,
    int target_wz)
{
    int wire_index;
    int dx;
    int dz;

    if (!wire_power || source_wy != target_wy)
        return 0;

    dx = source_wx - target_wx;
    dz = source_wz - target_wz;
    if (dx < 0)
        dx = -dx;
    if (dz < 0)
        dz = -dz;
    if (dx + dz != 1)
        return 0;

    wire_index = redstone_wire_lookup(wires, wire_table, wire_table_cap,
                                      source_wx, source_wy, source_wz);
    return wire_index >= 0 ? wire_power[wire_index] : 0;
}

static bool redstone_button_pulse_active_locked(const VoxelWorld *world,
                                                int wx,
                                                int wy,
                                                int wz)
{
    for (int i = 0; i < world->redstone_pulse_count; i++) {
        const RedstonePulse *pulse = &world->redstone_pulses[i];

        if (pulse->wx == wx && pulse->wy == wy && pulse->wz == wz &&
            pulse->seconds_left > 0.0f)
            return true;
    }
    return false;
}

static uint8_t redstone_source_strength_to_locked(const VoxelWorld *world,
                                                  BlockID id,
                                                  int wx,
                                                  int wy,
                                                  int wz,
                                                  int target_wx,
                                                  int target_wy,
                                                  int target_wz)
{
    if (id == BLOCK_REDSTONE_BLOCK)
        return 15;

    if (id == BLOCK_REDSTONE_TORCH_ON) {
        RedstoneCell support;

        if (redstone_torch_support_locked(world, wx, wy, wz, &support) &&
            target_wx == support.wx &&
            target_wy == support.wy &&
            target_wz == support.wz)
            return 0;
        return 15;
    }

    if (block_is_redstone_directional(id)) {
        int dx;
        int dz;

        if (!block_redstone_directional_powered(id))
            return 0;
        redstone_facing_delta(block_redstone_facing(id), &dx, &dz);
        return target_wy == wy &&
               target_wx == wx + dx &&
               target_wz == wz + dz ? 15 : 0;
    }

    if (id == BLOCK_BUTTON &&
        redstone_button_pulse_active_locked(world, wx, wy, wz))
        return 15;

    return 0;
}

static bool redstone_start_button_pulse_locked(VoxelWorld *world,
                                               int wx,
                                               int wy,
                                               int wz)
{
    for (int i = 0; i < world->redstone_pulse_count; i++) {
        RedstonePulse *pulse = &world->redstone_pulses[i];

        if (pulse->wx == wx && pulse->wy == wy && pulse->wz == wz) {
            pulse->seconds_left = REDSTONE_BUTTON_PULSE_SECONDS;
            return true;
        }
    }

    if (world->redstone_pulse_count < WORLD_MAX_REDSTONE_PULSES) {
        world->redstone_pulses[world->redstone_pulse_count++] =
            (RedstonePulse){.wx = wx,
                            .wy = wy,
                            .wz = wz,
                            .seconds_left = REDSTONE_BUTTON_PULSE_SECONDS};
        return true;
    }

    int replace = 0;
    for (int i = 1; i < WORLD_MAX_REDSTONE_PULSES; i++) {
        if (world->redstone_pulses[i].seconds_left <
            world->redstone_pulses[replace].seconds_left)
            replace = i;
    }
    world->redstone_pulses[replace] =
        (RedstonePulse){.wx = wx,
                        .wy = wy,
                        .wz = wz,
                        .seconds_left = REDSTONE_BUTTON_PULSE_SECONDS};
    return true;
}

static void redstone_remove_pulse_locked(VoxelWorld *world, int index)
{
    if (index < 0 || index >= world->redstone_pulse_count)
        return;

    world->redstone_pulse_count--;
    if (index < world->redstone_pulse_count)
        world->redstone_pulses[index] =
            world->redstone_pulses[world->redstone_pulse_count];
}

static bool redstone_tick_button_pulses_locked(VoxelWorld *world, float dt)
{
    bool changed = false;

    for (int i = 0; i < world->redstone_pulse_count;) {
        RedstonePulse *pulse = &world->redstone_pulses[i];

        if (dt > 0.0f)
            pulse->seconds_left -= dt;
        if (pulse->seconds_left <= 0.0f ||
            world_get_block(world, pulse->wx, pulse->wy, pulse->wz) !=
                BLOCK_BUTTON) {
            redstone_remove_pulse_locked(world, i);
            changed = true;
            continue;
        }
        i++;
    }

    return changed;
}

static uint8_t redstone_repeater_delay_ticks_locked(const VoxelWorld *world,
                                                    int wx,
                                                    int wy,
                                                    int wz)
{
    uint8_t delay = world_get_redstone_data_locked(world, wx, wy, wz);

    if (delay < REDSTONE_REPEATER_MIN_DELAY_TICKS)
        return REDSTONE_REPEATER_MIN_DELAY_TICKS;
    if (delay > REDSTONE_REPEATER_MAX_DELAY_TICKS)
        return REDSTONE_REPEATER_MAX_DELAY_TICKS;
    return delay;
}

static int redstone_repeater_state_find_locked(const VoxelWorld *world,
                                               int wx,
                                               int wy,
                                               int wz)
{
    for (int i = 0; i < world->redstone_repeater_state_count; i++) {
        const RedstoneRepeaterState *state =
            &world->redstone_repeater_states[i];

        if (state->wx == wx && state->wy == wy && state->wz == wz)
            return i;
    }
    return -1;
}

static void redstone_remove_repeater_state_locked(VoxelWorld *world, int index)
{
    if (index < 0 || index >= world->redstone_repeater_state_count)
        return;

    world->redstone_repeater_state_count--;
    if (index < world->redstone_repeater_state_count)
        world->redstone_repeater_states[index] =
            world->redstone_repeater_states[world->redstone_repeater_state_count];
}

static void redstone_cancel_repeater_state_locked(VoxelWorld *world,
                                                  int wx,
                                                  int wy,
                                                  int wz)
{
    int index = redstone_repeater_state_find_locked(world, wx, wy, wz);

    if (index >= 0)
        redstone_remove_repeater_state_locked(world, index);
}

static void redstone_schedule_repeater_state_locked(VoxelWorld *world,
                                                    int wx,
                                                    int wy,
                                                    int wz,
                                                    bool target_powered)
{
    uint8_t delay_ticks =
        redstone_repeater_delay_ticks_locked(world, wx, wy, wz);
    RedstoneRepeaterState next = {
        .wx = wx,
        .wy = wy,
        .wz = wz,
        .target_powered = target_powered ? 1u : 0u,
        .seconds_left = (float)delay_ticks * REDSTONE_REPEATER_TICK_SECONDS,
    };
    int index = redstone_repeater_state_find_locked(world, wx, wy, wz);

    if (index >= 0) {
        RedstoneRepeaterState *state = &world->redstone_repeater_states[index];

        if (state->target_powered == next.target_powered)
            return;
        *state = next;
        return;
    }

    if (world->redstone_repeater_state_count <
        WORLD_MAX_REDSTONE_REPEATER_STATES) {
        world->redstone_repeater_states[world->redstone_repeater_state_count++] =
            next;
        return;
    }

    index = 0;
    for (int i = 1; i < WORLD_MAX_REDSTONE_REPEATER_STATES; i++) {
        if (world->redstone_repeater_states[i].seconds_left <
            world->redstone_repeater_states[index].seconds_left)
            index = i;
    }
    world->redstone_repeater_states[index] = next;
}

static bool redstone_tick_repeater_states_locked(VoxelWorld *world, float dt)
{
    bool changed = false;

    for (int i = 0; i < world->redstone_repeater_state_count;) {
        RedstoneRepeaterState *state = &world->redstone_repeater_states[i];
        BlockID current = world_get_block(world, state->wx, state->wy, state->wz);
        bool current_powered;
        BlockDoorFacing facing;

        if (!block_is_repeater(current)) {
            redstone_remove_repeater_state_locked(world, i);
            continue;
        }

        current_powered = block_redstone_directional_powered(current);
        if (current_powered == (state->target_powered != 0u)) {
            redstone_remove_repeater_state_locked(world, i);
            continue;
        }

        if (dt > 0.0f)
            state->seconds_left -= dt;
        if (state->seconds_left > 0.0f) {
            i++;
            continue;
        }

        facing = block_redstone_facing(current);
        if (world_set_block_locked(world, state->wx, state->wy, state->wz,
                                   block_repeater_make(
                                       facing, state->target_powered != 0u)))
            changed = true;
        redstone_remove_repeater_state_locked(world, i);
    }

    return changed;
}

static bool redstone_any_adjacent_component_locked(const VoxelWorld *world,
                                                  int wx,
                                                  int wy,
                                                  int wz)
{
    for (int face = 0; face < NUM_FACES; face++) {
        BlockID neighbor = world_get_block(world,
                                           wx + FACE_NX[face],
                                           wy + FACE_NY[face],
                                           wz + FACE_NZ[face]);
        if (redstone_block_is_component(neighbor))
            return true;
    }
    return false;
}

static bool redstone_block_change_needs_update_locked(const VoxelWorld *world,
                                                      int wx,
                                                      int wy,
                                                      int wz,
                                                      BlockID old_type,
                                                      BlockID new_type)
{
    if (old_type == new_type)
        return false;
    if (redstone_block_is_component(old_type) ||
        redstone_block_is_component(new_type))
        return true;
    if (!redstone_block_connects_to_wire(old_type) &&
        !redstone_block_connects_to_wire(new_type))
        return false;
    return redstone_any_adjacent_component_locked(world, wx, wy, wz);
}

static bool redstone_wire_has_connection_locked(const VoxelWorld *world,
                                                int wx,
                                                int wy,
                                                int wz)
{
    for (int face = FACE_LEFT; face <= FACE_BACK; face++) {
        BlockID neighbor = world_get_block(world,
                                           wx + FACE_NX[face],
                                           wy,
                                           wz + FACE_NZ[face]);
        if (redstone_block_connects_to_wire(neighbor))
            return true;
    }
    return false;
}

static uint8_t redstone_power_from_cell_locked(
    const VoxelWorld *world,
    int source_wx,
    int source_wy,
    int source_wz,
    int target_wx,
    int target_wy,
    int target_wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap);

static uint8_t redstone_wire_direct_source_strength_locked(const VoxelWorld *world,
                                                           int wx,
                                                           int wy,
                                                           int wz,
                                                           const RedstoneCell *wires,
                                                           const int *wire_table,
                                                           size_t wire_table_cap)
{
    uint8_t strength = 0;

    for (int face = 0; face < NUM_FACES; face++) {
        int nx = wx + FACE_NX[face];
        int ny = wy + FACE_NY[face];
        int nz = wz + FACE_NZ[face];
        uint8_t source_strength =
            redstone_power_from_cell_locked(world, nx, ny, nz,
                                            wx, wy, wz,
                                            wires, NULL,
                                            wire_table, wire_table_cap);

        if (source_strength > strength)
            strength = source_strength;
    }
    return strength;
}

static uint8_t redstone_direct_power_from_cell_locked(
    const VoxelWorld *world,
    int source_wx,
    int source_wy,
    int source_wz,
    int target_wx,
    int target_wy,
    int target_wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    BlockID source = world_get_block(world, source_wx, source_wy, source_wz);
    uint8_t strength =
        redstone_source_strength_to_locked(world, source,
                                           source_wx, source_wy, source_wz,
                                           target_wx, target_wy, target_wz);

    if (strength > 0)
        return strength;
    if (redstone_block_is_wire(source)) {
        return redstone_wire_power_to_adjacent_cell_locked(
            wires, wire_power, wire_table, wire_table_cap,
            source_wx, source_wy, source_wz,
            target_wx, target_wy, target_wz);
    }
    return 0;
}

static uint8_t redstone_position_direct_power_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap,
    int ignore_wx,
    int ignore_wy,
    int ignore_wz)
{
    uint8_t strength = 0;

    for (int face = 0; face < NUM_FACES; face++) {
        int nx = wx + FACE_NX[face];
        int ny = wy + FACE_NY[face];
        int nz = wz + FACE_NZ[face];
        uint8_t source_strength;

        if (nx == ignore_wx && ny == ignore_wy && nz == ignore_wz)
            continue;

        source_strength = redstone_direct_power_from_cell_locked(world,
                                                                 nx, ny, nz,
                                                                 wx, wy, wz,
                                                                 wires,
                                                                 wire_power,
                                                                 wire_table,
                                                                 wire_table_cap);
        if (source_strength > strength)
            strength = source_strength;
        if (strength >= 15)
            return strength;
    }

    return strength;
}

static uint8_t redstone_powered_block_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap,
    int ignore_wx,
    int ignore_wy,
    int ignore_wz)
{
    if (!redstone_block_can_hold_power(world_get_block(world, wx, wy, wz)))
        return 0;

    return redstone_position_direct_power_strength_locked(world,
                                                          wx, wy, wz,
                                                          wires, wire_power,
                                                          wire_table,
                                                          wire_table_cap,
                                                          ignore_wx,
                                                          ignore_wy,
                                                          ignore_wz);
}

static uint8_t redstone_power_from_cell_locked(
    const VoxelWorld *world,
    int source_wx,
    int source_wy,
    int source_wz,
    int target_wx,
    int target_wy,
    int target_wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    uint8_t strength =
        redstone_direct_power_from_cell_locked(world,
                                               source_wx, source_wy, source_wz,
                                               target_wx, target_wy, target_wz,
                                               wires, wire_power,
                                               wire_table, wire_table_cap);

    if (strength > 0)
        return strength;
    return redstone_powered_block_strength_locked(world,
                                                  source_wx, source_wy, source_wz,
                                                  wires, wire_power,
                                                  wire_table, wire_table_cap,
                                                  target_wx, target_wy,
                                                  target_wz);
}

static uint8_t redstone_position_power_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap,
    int ignore_wx,
    int ignore_wy,
    int ignore_wz)
{
    uint8_t strength = 0;

    for (int face = 0; face < NUM_FACES; face++) {
        int nx = wx + FACE_NX[face];
        int ny = wy + FACE_NY[face];
        int nz = wz + FACE_NZ[face];
        uint8_t source_strength;

        if (nx == ignore_wx && ny == ignore_wy && nz == ignore_wz)
            continue;

        source_strength = redstone_power_from_cell_locked(world,
                                                          nx, ny, nz,
                                                          wx, wy, wz,
                                                          wires, wire_power,
                                                          wire_table,
                                                          wire_table_cap);
        if (source_strength > strength)
            strength = source_strength;
        if (strength >= 15)
            return strength;
    }

    return strength;
}

static uint8_t redstone_directional_back_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    BlockDoorFacing facing,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    int dx;
    int dz;

    redstone_facing_delta(facing, &dx, &dz);
    return redstone_power_from_cell_locked(world,
                                           wx - dx, wy, wz - dz,
                                           wx, wy, wz,
                                           wires, wire_power,
                                           wire_table, wire_table_cap);
}

static uint8_t redstone_directional_side_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    BlockDoorFacing facing,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    int dx;
    int dz;
    uint8_t left;
    uint8_t right;

    redstone_facing_delta(facing, &dx, &dz);
    left = redstone_power_from_cell_locked(world,
                                           wx + dz, wy, wz - dx,
                                           wx, wy, wz,
                                           wires, wire_power,
                                           wire_table, wire_table_cap);
    right = redstone_power_from_cell_locked(world,
                                            wx - dz, wy, wz + dx,
                                            wx, wy, wz,
                                            wires, wire_power,
                                            wire_table, wire_table_cap);
    return left > right ? left : right;
}

static uint8_t redstone_torch_support_top_strength_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    uint8_t strength = 0;

    for (int face = FACE_LEFT; face <= FACE_BACK; face++) {
        int nx = wx + FACE_NX[face];
        int nz = wz + FACE_NZ[face];
        uint8_t source_strength =
            redstone_power_from_cell_locked(world,
                                            nx, wy, nz,
                                            wx, wy - 1, wz,
                                            wires, wire_power,
                                            wire_table, wire_table_cap);

        if (source_strength > strength)
            strength = source_strength;
    }

    return strength;
}

static bool redstone_torch_receives_power_locked(
    const VoxelWorld *world,
    int wx,
    int wy,
    int wz,
    const RedstoneCell *wires,
    const uint8_t *wire_power,
    const int *wire_table,
    size_t wire_table_cap)
{
    RedstoneCell support;
    BlockID support_id;

    if (!redstone_torch_support_locked(world, wx, wy, wz, &support))
        return false;

    support_id = world_get_block(world, support.wx, support.wy, support.wz);
    if (redstone_source_strength_to_locked(world, support_id,
                                           support.wx, support.wy, support.wz,
                                           wx, wy, wz) > 0)
        return true;
    if (redstone_position_power_strength_locked(world,
                                                support.wx, support.wy,
                                                support.wz,
                                                wires, wire_power,
                                                wire_table, wire_table_cap,
                                                wx, wy, wz) > 0)
        return true;
    if (support.wx == wx && support.wy == wy - 1 && support.wz == wz)
        return redstone_torch_support_top_strength_locked(world,
                                                          wx, wy, wz,
                                                          wires, wire_power,
                                                          wire_table,
                                                          wire_table_cap) > 0;
    return false;
}

static bool redstone_settle_pass_locked(VoxelWorld *world)
{
    RedstoneCell *wires = NULL;
    RedstoneCell *components = NULL;
    RedstoneChange *changes = NULL;
    size_t wire_count = 0, wire_cap = 0;
    size_t component_count = 0, component_cap = 0;
    size_t change_count = 0, change_cap = 0;
    int *wire_table = NULL;
    size_t wire_table_cap = 0;
    uint8_t *wire_power = NULL;
    bool changed = false;
    bool ok = true;

    for (int ci = 0; ci < world->chunk_count && ok; ci++) {
        Chunk *chunk = &world->chunks[ci];
        int ox;
        int oz;

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT && ok; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE && ok; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    BlockID id = chunk->blocks[y][z][x];
                    RedstoneCell cell = {.wx = ox + x,
                                          .wy = y,
                                          .wz = oz + z};

                    if (redstone_block_is_wire(id)) {
                        ok = redstone_cell_push(&wires, &wire_count,
                                                 &wire_cap, cell);
                    } else if (redstone_block_has_state(id)) {
                        ok = redstone_cell_push(&components,
                                                 &component_count,
                                                 &component_cap, cell);
                    }
                    if (!ok)
                        break;
                }
            }
        }
    }
    if (!ok)
        goto done;
    if (wire_count == 0 && component_count == 0)
        goto done;

    if (!redstone_build_wire_table(wires, wire_count,
                                   &wire_table, &wire_table_cap))
        goto done;

    if (wire_count > 0) {
        wire_power = calloc(wire_count, sizeof(*wire_power));
        if (!wire_power)
            goto done;

        for (size_t i = 0; i < wire_count; i++) {
            wire_power[i] =
                redstone_wire_direct_source_strength_locked(world,
                                                            wires[i].wx,
                                                            wires[i].wy,
                                                            wires[i].wz,
                                                            wires,
                                                            wire_table,
                                                            wire_table_cap);
        }

        for (int pass = 0; pass < 15; pass++) {
            bool propagated = false;

            for (size_t current = 0; current < wire_count; current++) {
                if (wire_power[current] <= 1)
                    continue;

                for (int face = FACE_LEFT; face <= FACE_BACK; face++) {
                    int nx = wires[current].wx + FACE_NX[face];
                    int ny = wires[current].wy;
                    int nz = wires[current].wz + FACE_NZ[face];
                    int next = redstone_wire_lookup(wires, wire_table,
                                                    wire_table_cap,
                                                    nx, ny, nz);
                    uint8_t next_power;

                    if (next < 0)
                        continue;
                    next_power = (uint8_t)(wire_power[current] - 1u);
                    if (next_power > wire_power[next]) {
                        wire_power[next] = next_power;
                        propagated = true;
                    }
                }
            }

            if (!propagated)
                break;
        }
    }

    for (size_t i = 0; i < wire_count; i++) {
        BlockID current = world_get_block(world,
                                          wires[i].wx,
                                          wires[i].wy,
                                          wires[i].wz);
        bool connected = redstone_wire_has_connection_locked(world,
                                                             wires[i].wx,
                                                             wires[i].wy,
                                                             wires[i].wz);
        BlockID target = wire_power && wire_power[i] > 0 ?
                         BLOCK_REDSTONE_WIRE_ON :
                         (connected ? BLOCK_REDSTONE_WIRE_OFF :
                                      BLOCK_REDSTONE_WIRE_UNCONNECTED);

        if (current != target &&
            !redstone_change_push(&changes, &change_count, &change_cap,
                                  (RedstoneChange){.wx = wires[i].wx,
                                                   .wy = wires[i].wy,
                                                   .wz = wires[i].wz,
                                                   .type = target}))
            goto done;
    }

    for (size_t i = 0; i < component_count; i++) {
        RedstoneCell cell = components[i];
        BlockID current = world_get_block(world, cell.wx, cell.wy, cell.wz);
        BlockID target = current;
        uint8_t power;

        if (current == BLOCK_REDSTONE_TORCH_OFF ||
            current == BLOCK_REDSTONE_TORCH_ON) {
            power = redstone_torch_receives_power_locked(world,
                                                         cell.wx,
                                                         cell.wy,
                                                         cell.wz,
                                                         wires,
                                                         wire_power,
                                                         wire_table,
                                                         wire_table_cap) ? 15 : 0;
            target = power > 0 ? BLOCK_REDSTONE_TORCH_OFF :
                                 BLOCK_REDSTONE_TORCH_ON;
        } else if (block_is_repeater(current)) {
            BlockDoorFacing facing = block_redstone_facing(current);
            bool current_powered = block_redstone_directional_powered(current);
            bool target_powered;

            power = redstone_directional_back_strength_locked(
                world, cell.wx, cell.wy, cell.wz, facing,
                wires, wire_power, wire_table, wire_table_cap);
            target_powered = power > 0;
            if (current_powered == target_powered)
                redstone_cancel_repeater_state_locked(world,
                                                      cell.wx, cell.wy,
                                                      cell.wz);
            else
                redstone_schedule_repeater_state_locked(world,
                                                        cell.wx, cell.wy,
                                                        cell.wz,
                                                        target_powered);
        } else if (block_is_comparator(current)) {
            BlockDoorFacing facing = block_redstone_facing(current);
            uint8_t rear = redstone_directional_back_strength_locked(
                world, cell.wx, cell.wy, cell.wz, facing,
                wires, wire_power, wire_table, wire_table_cap);
            uint8_t side = redstone_directional_side_strength_locked(
                world, cell.wx, cell.wy, cell.wz, facing,
                wires, wire_power, wire_table, wire_table_cap);
            target = block_comparator_make(facing, rear > 0 && rear >= side);
        } else if (current == BLOCK_LAMP_OFF ||
                   current == BLOCK_LAMP) {
            power = redstone_position_power_strength_locked(
                world, cell.wx, cell.wy, cell.wz,
                wires, wire_power, wire_table, wire_table_cap,
                INT_MIN, INT_MIN, INT_MIN);
            target = power > 0 ? BLOCK_LAMP : BLOCK_LAMP_OFF;
        }

        if (current != target &&
            !redstone_change_push(&changes, &change_count, &change_cap,
                                  (RedstoneChange){.wx = cell.wx,
                                                   .wy = cell.wy,
                                                   .wz = cell.wz,
                                                   .type = target}))
            goto done;
    }

    for (size_t i = 0; i < change_count; i++) {
        if (world_set_block_locked(world,
                                   changes[i].wx,
                                   changes[i].wy,
                                   changes[i].wz,
                                   changes[i].type)) {
            changed = true;
        }
    }

done:
    free(wires);
    free(components);
    free(changes);
    free(wire_table);
    free(wire_power);
    return changed;
}

static bool world_update_redstone_locked(VoxelWorld *world, float dt)
{
    bool changed = redstone_tick_button_pulses_locked(world, dt);

    changed |= redstone_tick_repeater_states_locked(world, dt);
    for (int pass = 0; pass < REDSTONE_SETTLE_MAX_PASSES; pass++) {
        bool pass_changed = redstone_settle_pass_locked(world);

        changed |= pass_changed;
        if (!pass_changed)
            break;
    }

    return changed;
}

bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type)
{
    bool ok;
    bool update_redstone = false;

    if (!world)
        return false;

    world_lock(world);
    BlockID old_type = world_get_block(world, wx, wy, wz);
    ok = world_set_block_locked(world, wx, wy, wz, type);
    if (ok) {
        update_redstone = redstone_block_change_needs_update_locked(world,
                                                                    wx, wy, wz,
                                                                    old_type,
                                                                    type);
    }
    world_unlock(world);
    if (ok && update_redstone)
        (void)world_update_redstone(world, 0.0f);
    return ok;
}

bool world_update_redstone(VoxelWorld *world, float dt)
{
    bool changed;

    if (!world)
        return false;

    world_lock(world);
    changed = world_update_redstone_locked(world, dt);
    world_unlock(world);
    return changed;
}

bool world_press_button(VoxelWorld *world, int wx, int wy, int wz)
{
    bool pressed = false;

    if (!world)
        return false;

    world_lock(world);
    if (world_get_block(world, wx, wy, wz) == BLOCK_BUTTON) {
        /* Buttons only start a power pulse. Every visible side effect must be
         * produced by the normal redstone update path below. */
        pressed = redstone_start_button_pulse_locked(world, wx, wy, wz);
        if (pressed)
            (void)world_update_redstone_locked(world, 0.0f);
    }
    world_unlock(world);
    return pressed;
}

bool world_cycle_repeater_delay(VoxelWorld *world,
                                int wx,
                                int wy,
                                int wz,
                                uint8_t *delay_ticks_out)
{
    bool ok = false;
    uint8_t next_delay = 0;

    if (!world)
        return false;

    world_lock(world);
    if (block_is_repeater(world_get_block(world, wx, wy, wz))) {
        uint8_t current_delay =
            redstone_repeater_delay_ticks_locked(world, wx, wy, wz);

        next_delay = (uint8_t)(current_delay + 1u);
        if (next_delay > REDSTONE_REPEATER_MAX_DELAY_TICKS)
            next_delay = REDSTONE_REPEATER_MIN_DELAY_TICKS;
        ok = world_set_redstone_data_locked(world, wx, wy, wz, next_delay,
                                            true);
        if (ok) {
            redstone_cancel_repeater_state_locked(world, wx, wy, wz);
            (void)world_update_redstone_locked(world, 0.0f);
        }
    }
    world_unlock(world);

    if (ok && delay_ticks_out)
        *delay_ticks_out = next_delay;
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
 * Minecraft-style environment simulation
 * -----------------------------------------------------------------------
 * Fluid sources carry water_level=0 and stay alive forever. Fluid flows carry
 * water_level 1..7, where higher = thinner. A flow at level 7 is terminal and
 * never spreads further.
 *
 * Spread is GRADUAL: each tick we take an immutable snapshot of the
 * currently-existing source/flow cells (the "active" set), and only those
 * cells push fluid out by one ring. New flows placed during this tick are NOT
 * in the snapshot, so they wait until the next tick to spread.
 *
 * Sand and gravel are updated after fluids and become smooth falling entities.
 * ----------------------------------------------------------------------- */
#define WATER_MAX_LEVEL          7   /* Minecraft level cap; level 7 = terminal */
#define FALLING_BLOCK_SPEED_BLOCKS_PER_SEC (2.0f / 0.75f)
/* Active-cell snapshot capacity. Each entry is 16 bytes, so 4096 entries =
 * 64 KB of static .bss - cheap, and well over the ~225 cells of a fully-
 * developed 7-radius pool plus typical natural-ocean coastline. */
#define WATER_TICK_MAX_ACTIVE 4096

static WaterTickStats g_water_tick_stats = {0};

WaterTickStats world_water_tick_stats(void)
{
    return g_water_tick_stats;
}

/* Place a flow at (wx, wy, wz) and force its water_level to the given value.
 * world_set_block_locked() defaults flow placements to level 1 (the
 * public-API meaning "freshly placed flow"); the tick needs to override that
 * with the actual propagated level. */
static bool fluid_set_flow_locked(VoxelWorld *world,
                                  const FluidDef *fluid,
                                  int wx, int wy, int wz, uint8_t level)
{
    if (level < 1 || level > WATER_MAX_LEVEL)
        return false;
    if (!fluid)
        return false;
    if (!world_set_block_locked(world, wx, wy, wz, fluid->flow))
        return false;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    Chunk *c = world_get_chunk_mut(world, chunk_x, chunk_z);
    if (!c)
        return false;
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    c->water_level[wy][lz][lx] = level;
    return true;
}

static uint8_t world_fluid_level_locked(const VoxelWorld *world,
                                        const FluidDef *fluid,
                                        int wx, int wy, int wz)
{
    if (!world || wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 0;

    int chunk_x = floor_div(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div(wz, WORLD_CHUNK_SIZE);
    int lx = positive_mod(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return 0;

    BlockID id = chunk->blocks[wy][lz][lx];
    if (fluid && id == fluid->source)
        return 0;
    if (!fluid || id != fluid->flow)
        return WATER_MAX_LEVEL + 1;

    uint8_t level = chunk->water_level[wy][lz][lx];
    if (level < 1)
        level = 1;
    return level > WATER_MAX_LEVEL ? WATER_MAX_LEVEL : level;
}

static bool fluid_flow_has_support_locked(const VoxelWorld *world,
                                          const FluidDef *fluid,
                                          int wx, int wy, int wz,
                                          uint8_t level)
{
    if (wy + 1 < WORLD_CHUNK_HEIGHT &&
        block_is_fluid_kind(world_get_block(world, wx, wy + 1, wz), fluid))
        return true;

    const int sx[4] = {-1, 1,  0, 0};
    const int sz[4] = { 0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        BlockID nb = world_get_block(world, wx + sx[d], wy, wz + sz[d]);

        if (fluid && nb == fluid->source)
            return true;
        if (fluid && nb == fluid->flow &&
            world_fluid_level_locked(world, fluid,
                                     wx + sx[d], wy, wz + sz[d]) < level)
            return true;
    }

    return false;
}

static bool fluid_cell_rests_on_solid_locked(const VoxelWorld *world,
                                             int wx, int wy, int wz)
{
    if (wy <= 0)
        return true;

    BlockID below = world_get_block(world, wx, wy - 1, wz);
    return below != BLOCK_AIR && !block_is_any_fluid(below);
}

static bool fluid_cell_can_spread_locked(const VoxelWorld *world,
                                         int wx, int wy, int wz)
{
    if (wy > 0 && world_get_block(world, wx, wy - 1, wz) == BLOCK_AIR)
        return true;
    if (!fluid_cell_rests_on_solid_locked(world, wx, wy, wz))
        return false;

    const int sx[4] = {-1, 1,  0, 0};
    const int sz[4] = { 0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        if (world_get_block(world, wx + sx[d], wy, wz + sz[d]) == BLOCK_AIR)
            return true;
    }

    return false;
}

static bool fluid_set_falling_lateral_locked(VoxelWorld *world,
                                             const FluidDef *fluid,
                                             int wx, int wy, int wz,
                                             uint8_t lateral_level,
                                             WaterTickStats *stats)
{
    bool placed = false;

    if (fluid_set_flow_locked(world, fluid, wx, wy, wz, lateral_level)) {
        placed = true;
        if (stats)
            stats->spread_placed++;
    }
    if (wy > 0 &&
        world_get_block(world, wx, wy - 1, wz) == BLOCK_AIR &&
        fluid_set_flow_locked(world, fluid, wx, wy - 1, wz, 1)) {
        placed = true;
        if (stats)
            stats->spread_placed++;
    }

    return placed;
}

static bool fluid_tick_locked(VoxelWorld *world,
                              const FluidDef *fluid,
                              WaterTickStats *stats)
{
    typedef struct { int wx, wy, wz; uint8_t level; } FluidCell;
    static FluidCell active[WATER_TICK_MAX_ACTIVE];
    int active_count = 0;
    bool changed = false;

    /* ---- Pass 1: evaporate unsupported flow blocks ---- */
    for (int ci = 0; ci < world->chunk_count; ci++) {
        Chunk *chunk = &world->chunks[ci];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        int ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        int oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    if (!fluid || chunk->blocks[y][z][x] != fluid->flow)
                        continue;

                    if (stats)
                        stats->flows_seen++;
                    int wx = ox + x, wy = y, wz = oz + z;
                    uint8_t level = chunk->water_level[y][z][x];
                    if (level < 1 || level > WATER_MAX_LEVEL)
                        level = 1;

                    if (!fluid_flow_has_support_locked(world, fluid,
                                                       wx, wy, wz, level)) {
                        world_set_block_locked(world, wx, wy, wz, BLOCK_AIR);
                        changed = true;
                        if (stats)
                            stats->evaporated++;
                    }
                }
            }
        }
    }

    /* ---- Pass 2: snapshot active spreaders ----
     * An "active" cell is a source or non-terminal, supported flow that can
     * place fluid into a face neighbour. Flows in a falling column wait until
     * they rest on solid ground before spreading sideways. */
    for (int ci = 0;
         ci < world->chunk_count && active_count < WATER_TICK_MAX_ACTIVE;
         ci++) {
        Chunk *chunk = &world->chunks[ci];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        int ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        int oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 0;
             y < WORLD_CHUNK_HEIGHT && active_count < WATER_TICK_MAX_ACTIVE;
             y++) {
            for (int z = 0;
                 z < WORLD_CHUNK_SIZE && active_count < WATER_TICK_MAX_ACTIVE;
                 z++) {
                for (int x = 0;
                     x < WORLD_CHUNK_SIZE && active_count < WATER_TICK_MAX_ACTIVE;
                     x++) {
                    BlockID b = chunk->blocks[y][z][x];
                    if (!block_is_fluid_kind(b, fluid))
                        continue;

                    if (fluid && b == fluid->source && stats)
                        stats->sources_seen++;

                    uint8_t lvl = (fluid && b == fluid->source)
                                      ? 0
                                      : chunk->water_level[y][z][x];
                    /* Terminal flows can't push further. */
                    if (fluid && b == fluid->flow && lvl >= WATER_MAX_LEVEL)
                        continue;

                    int wx = ox + x, wy = y, wz = oz + z;
                    if (fluid && b == fluid->flow &&
                        !fluid_flow_has_support_locked(world, fluid,
                                                       wx, wy, wz, lvl))
                        continue;
                    if (!fluid_cell_can_spread_locked(world, wx, wy, wz))
                        continue;

                    active[active_count++] = (FluidCell){wx, wy, wz, lvl};
                }
            }
        }
    }

    /* ---- Pass 3: spread one ring per cell ----
     * Only the cells captured in `active` get to spread this tick. Any
     * flows we place here are NOT added to `active`, so they don't
     * cascade further until the next tick - that's what bounds spread to
     * one ring/tick and produces the gradual pool growth. */
    for (int i = 0; i < active_count; i++) {
        FluidCell c = active[i];

        /* Gravity priority: falling fluid resets to level 1, and we skip
         * lateral spread for this cell so the column descends cleanly. */
        if (c.wy > 0 &&
            world_get_block(world, c.wx, c.wy - 1, c.wz) == BLOCK_AIR) {
            if (fluid_set_flow_locked(world, fluid,
                                      c.wx, c.wy - 1, c.wz, 1)) {
                changed = true;
                if (stats)
                    stats->spread_placed++;
            }
            continue;
        }

        if (!fluid_cell_rests_on_solid_locked(world, c.wx, c.wy, c.wz))
            continue;

        /* Lateral spread at one level higher than the source cell. */
        uint8_t next_level = (uint8_t)(c.level + 1);
        if (next_level > WATER_MAX_LEVEL)
            continue;

        const int sx[4] = {-1, 1,  0, 0};
        const int sz[4] = { 0, 0, -1, 1};
        bool downhill[4] = { false, false, false, false };
        bool has_downhill = false;

        for (int d = 0; d < 4; d++) {
            int nx = c.wx + sx[d];
            int nz = c.wz + sz[d];
            if (world_get_block(world, nx, c.wy, nz) != BLOCK_AIR)
                continue;
            if (c.wy > 0 &&
                world_get_block(world, nx, c.wy - 1, nz) == BLOCK_AIR) {
                downhill[d] = true;
                has_downhill = true;
            }
        }

        for (int d = 0; d < 4; d++) {
            int nx = c.wx + sx[d];
            int nz = c.wz + sz[d];
            if (has_downhill && !downhill[d])
                continue;
            if (world_get_block(world, nx, c.wy, nz) != BLOCK_AIR)
                continue;
            if (downhill[d]) {
                if (fluid_set_falling_lateral_locked(world, fluid,
                                                     nx, c.wy, nz,
                                                     next_level, stats))
                    changed = true;
            } else if (fluid_set_flow_locked(world, fluid,
                                             nx, c.wy, nz, next_level)) {
                changed = true;
                if (stats)
                    stats->spread_placed++;
            }
        }
    }

    return changed;
}

static bool falling_block_active_below_locked(const VoxelWorld *world,
                                              int wx, int wy, int wz)
{
    for (int i = 0; i < WORLD_MAX_FALLING_BLOCKS; i++) {
        const FallingBlock *block = &world->falling_blocks[i];

        if (!block->active)
            continue;
        if (block->wx == wx && block->wz == wz && block->y < (float)wy)
            return true;
    }

    return false;
}

static FallingBlock *falling_block_alloc_locked(VoxelWorld *world)
{
    for (int i = 0; i < WORLD_MAX_FALLING_BLOCKS; i++) {
        if (!world->falling_blocks[i].active)
            return &world->falling_blocks[i];
    }

    return NULL;
}

static bool falling_block_spawn_locked(VoxelWorld *world,
                                       int wx, int wy, int wz,
                                       BlockID type)
{
    FallingBlock *falling = falling_block_alloc_locked(world);

    if (!falling)
        return false;
    if (!world_set_block_locked(world, wx, wy, wz, BLOCK_AIR))
        return false;

    *falling = (FallingBlock){
        .active = true,
        .type = type,
        .wx = wx,
        .origin_y = wy,
        .wz = wz,
        .y = (float)wy,
    };
    world->falling_block_count++;
    return true;
}

static int falling_block_target_y_locked(const VoxelWorld *world,
                                         const FallingBlock *falling)
{
    int target_y = (int)floorf(falling->y);

    if (target_y >= WORLD_CHUNK_HEIGHT)
        target_y = WORLD_CHUNK_HEIGHT - 1;
    if (target_y < 0)
        target_y = 0;

    while (target_y > 0 &&
           world_get_block(world, falling->wx, target_y - 1,
                           falling->wz) == BLOCK_AIR) {
        target_y--;
    }

    while (target_y < WORLD_CHUNK_HEIGHT &&
           world_get_block(world, falling->wx, target_y, falling->wz) !=
               BLOCK_AIR) {
        target_y++;
    }

    return target_y < WORLD_CHUNK_HEIGHT ? target_y : -1;
}

static bool falling_block_deactivate_locked(VoxelWorld *world,
                                            FallingBlock *falling)
{
    if (!falling->active)
        return false;

    falling->active = false;
    if (world->falling_block_count > 0)
        world->falling_block_count--;
    return true;
}

static bool falling_block_settle_locked(VoxelWorld *world,
                                        FallingBlock *falling)
{
    int target_y = falling_block_target_y_locked(world, falling);
    BlockID type = falling->type;
    int wx = falling->wx;
    int wz = falling->wz;

    falling_block_deactivate_locked(world, falling);
    if (target_y < 0)
        return false;
    return world_set_block_locked(world, wx, target_y, wz, type);
}

static bool falling_blocks_tick_locked(VoxelWorld *world, WaterTickStats *stats)
{
    bool changed = false;

    for (int ci = 0; ci < world->chunk_count; ci++) {
        Chunk *chunk = &world->chunks[ci];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        int ox = chunk->chunk_x * WORLD_CHUNK_SIZE;
        int oz = chunk->chunk_z * WORLD_CHUNK_SIZE;

        for (int y = 1; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    BlockID id = chunk->blocks[y][z][x];
                    if (!block_is_gravity_affected(id))
                        continue;

                    int wx = ox + x;
                    int wz = oz + z;
                    if (world_get_block(world, wx, y - 1, wz) != BLOCK_AIR)
                        continue;
                    if (falling_block_active_below_locked(world, wx, y, wz))
                        continue;

                    if (!falling_block_spawn_locked(world, wx, y, wz, id))
                        continue;

                    changed = true;
                    if (stats)
                        stats->falling_moved++;
                }
            }
        }
    }

    return changed;
}

bool world_water_tick(VoxelWorld *world)
{
    if (!world)
        return false;

    bool changed = false;
    WaterTickStats stats = {0};

    world_lock(world);
    changed |= fluid_tick_locked(world, &WATER_FLUID, &stats);
    changed |= fluid_tick_locked(world, &LAVA_FLUID, &stats);
    changed |= falling_blocks_tick_locked(world, &stats);
    world_unlock(world);

    g_water_tick_stats = stats;
    return changed;
}

bool world_update_falling_blocks(VoxelWorld *world, float dt)
{
    bool changed = false;

    if (!world || dt <= 0.0f)
        return false;

    world_lock(world);
    for (int i = 0; i < WORLD_MAX_FALLING_BLOCKS; i++) {
        FallingBlock *falling = &world->falling_blocks[i];
        int chunk_x;
        int chunk_z;
        const Chunk *chunk;
        int target_y;
        float next_y;

        if (!falling->active)
            continue;

        chunk_x = floor_div(falling->wx, WORLD_CHUNK_SIZE);
        chunk_z = floor_div(falling->wz, WORLD_CHUNK_SIZE);
        chunk = world_get_chunk(world, chunk_x, chunk_z);
        if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED)) {
            falling_block_deactivate_locked(world, falling);
            changed = true;
            continue;
        }

        target_y = falling_block_target_y_locked(world, falling);
        if (target_y < 0) {
            falling_block_deactivate_locked(world, falling);
            changed = true;
            continue;
        }

        next_y = falling->y - FALLING_BLOCK_SPEED_BLOCKS_PER_SEC * dt;
        if (next_y <= (float)target_y) {
            falling->y = (float)target_y;
            changed |= falling_block_settle_locked(world, falling);
            continue;
        }

        falling->y = next_y;
        changed = true;
    }
    world_unlock(world);

    return changed;
}

int world_falling_block_count(const VoxelWorld *world)
{
    if (!world)
        return 0;
    return world->falling_block_count;
}
