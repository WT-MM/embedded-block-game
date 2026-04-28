#include "world.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_LOOKUP_EMPTY (-1)

/* Greedy face merging runs only on chunks farther than this (Chebyshev)
 * distance from the player's chunk. Near chunks emit 1x1 faces so the
 * T-junction / UV-shimmer artifacts caused by merged quads stay out of
 * the viewer's foreground, while distant chunks (which dominate face
 * counts at large render distances) still get the merge win. */
#define NEAR_CHUNK_RADIUS 3
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
static const int FACE_NX[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
static const int FACE_NY[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
static const int FACE_NZ[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };

static bool chunk_is_near(const VoxelWorld *world, const Chunk *chunk)
{
    int dx = chunk->chunk_x - world->center_chunk_x;
    int dz = chunk->chunk_z - world->center_chunk_z;

    if (dx < 0)
        dx = -dx;
    if (dz < 0)
        dz = -dz;

    return (dx <= NEAR_CHUNK_RADIUS) && (dz <= NEAR_CHUNK_RADIUS);
}

static uint32_t rng_next(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static uint32_t chunk_seed(uint32_t base_seed, int chunk_x, int chunk_z)
{
    uint32_t xbits = (uint32_t)chunk_x * 0x9e3779b9u;
    uint32_t zbits = (uint32_t)chunk_z * 0x85ebca6bu;
    uint32_t seed = base_seed ^ xbits ^ zbits;

    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return seed;
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
            if ((chunk->flags & CHUNK_FLAG_LOADED) &&
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
            if ((chunk->flags & CHUNK_FLAG_LOADED) &&
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
        if ((world->chunks[i].flags & CHUNK_FLAG_LOADED) &&
            !chunk_lookup_insert(world, i))
            return false;
    }

    return true;
}

static void release_chunk(Chunk *chunk)
{
    if (!chunk)
        return;

    free(chunk->faces);
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

static void generate_chunk_terrain(Chunk *chunk, uint32_t base_seed,
                                   int stone_tries_per_chunk)
{
    uint32_t seed = chunk_seed(base_seed, chunk->chunk_x, chunk->chunk_z);

    clear_chunk_blocks(chunk);

    for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
        for (int x = 0; x < WORLD_CHUNK_SIZE; x++)
            chunk->blocks[0][z][x] = BLOCK_GRASS;
    }

    for (int i = 0; i < stone_tries_per_chunk; i++) {
        int lx = (int)(rng_next(&seed) % WORLD_CHUNK_SIZE);
        int lz = (int)(rng_next(&seed) % WORLD_CHUNK_SIZE);
        int wx = chunk->chunk_x * WORLD_CHUNK_SIZE + lx;
        int wz = chunk->chunk_z * WORLD_CHUNK_SIZE + lz;
        int height = 1 + (int)(rng_next(&seed) % 3u);
        /* Sprinkle glass columns at roughly 1-in-4 odds so we get a mix of
         * stone and glass formations, some stacked, some standalone. Glass
         * exercises the alpha-blend path at face edges and through-block
         * visibility. */
        BlockID kind = ((rng_next(&seed) & 3u) == 0u) ? BLOCK_GLASS : BLOCK_STONE;

        if (fabsf((float)wx) <= 1.0f && wz <= 5)
            continue;
        if (chunk->blocks[1][lz][lx] != BLOCK_AIR)
            continue;

        for (int y = 1; y <= height; y++)
            chunk->blocks[y][lz][lx] = kind;
    }
}

/*
 * A face between `current` and `neighbor` should be emitted when the
 * neighbor doesn't fully occlude it:
 *   - Any face next to air is visible.
 *   - An opaque block's face next to glass (or any other translucent
 *     neighbor) is visible: you see the opaque surface through the glass.
 *   - A glass block's face next to another glass block is hidden, which
 *     collapses interior walls of solid glass volumes into a single hull.
 *   - A glass face next to a solid block is hidden: the solid occludes
 *     the back of the glass.
 */
static bool face_should_render(BlockID current, BlockID neighbor)
{
    if (neighbor == BLOCK_AIR)
        return true;
    if (current == BLOCK_GLASS)
        return false;
    return block_is_transparent(neighbor);
}

void world_init(VoxelWorld *world)
{
    memset(world, 0, sizeof(*world));
}

void world_free(VoxelWorld *world)
{
    if (!world)
        return;

    free_chunk_storage(world);
    memset(world, 0, sizeof(*world));
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

            Chunk *c = world_get_chunk_mut(world, floor_div(nwx, WORLD_CHUNK_SIZE), floor_div(nwz, WORLD_CHUNK_SIZE));
            if (c) c->flags |= CHUNK_FLAG_MESH_DIRTY;

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
                Chunk *c = world_get_chunk_mut(world, floor_div(nwx, WORLD_CHUNK_SIZE), floor_div(nwz, WORLD_CHUNK_SIZE));
                if (c) c->flags |= CHUNK_FLAG_MESH_DIRTY;

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
        chunk->flags |= CHUNK_FLAG_MESH_DIRTY;
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
}

bool world_rebuild_lighting(VoxelWorld *world)
{
    LightNode *queue = NULL;
    size_t queue_head = 0;
    size_t queue_tail = 0;
    size_t queue_capacity = 0;

    if (!world)
        return false;
    if (world->chunk_count <= 0)
        return true;

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

                    chunk->block_light[y][z][x] = emission;
                    if (!light_queue_push(&queue, &queue_capacity, &queue_tail,
                                          (LightNode){
                                              .wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x,
                                              .wy = y,
                                              .wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z,
                                          })) {
                        free(queue);
                        return false;
                    }
                }
            }
        }
    }

    if (!propagate_block_light(world, &queue, &queue_capacity, queue_head, &queue_tail)) {
        free(queue);
        return false;
    }

    free(queue);
    mark_all_loaded_chunks_mesh_dirty(world);
    return true;
}

static int count_exposed_faces_for_chunk_nb(const VoxelWorld *world, const Chunk *chunk,
                                              const Chunk *nb[3][3])
{
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
        *x = v;
        *y = u;
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
        *width = WORLD_CHUNK_HEIGHT; /* u axis: y */
        *height = WORLD_CHUNK_SIZE;  /* v axis: x */
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

static bool rebuild_chunk_faces(VoxelWorld *world, Chunk *chunk)
{
    int max_face_count;
    int out = 0;
    const Chunk *nb[3][3];
    int ccx;
    int ccz;

    if (!world || !chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    ccx = chunk->chunk_x;
    ccz = chunk->chunk_z;
    fill_neighbor_chunk_cache(world, ccx, ccz, nb);

    bool is_near = chunk_is_near(world, chunk);

    max_face_count = count_exposed_faces_for_chunk_nb(world, chunk, nb);
    if (max_face_count == 0) {
        chunk->face_count = 0;
        chunk->flags &= ~CHUNK_FLAG_MESH_DIRTY;
        chunk->flags |= CHUNK_FLAG_MESH_READY;
        if (is_near)
            chunk->flags |= CHUNK_FLAG_MESHED_NEAR;
        else
            chunk->flags &= ~CHUNK_FLAG_MESHED_NEAR;
        return true;
    }

    if (!ensure_chunk_face_capacity(chunk, max_face_count))
        return false;

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
                     * shimmer. Both artifacts are conspicuous up close but
                     * disappear into the depth/perspective noise far away, so
                     * run the merge only on distant chunks where the face-
                     * count win matters most.
                     */
                    if (!is_near) {
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
    chunk->flags &= ~CHUNK_FLAG_MESH_DIRTY;
    chunk->flags |= CHUNK_FLAG_MESH_READY;
    if (is_near)
        chunk->flags |= CHUNK_FLAG_MESHED_NEAR;
    else
        chunk->flags &= ~CHUNK_FLAG_MESHED_NEAR;
    return true;
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

bool world_rebuild_dirty_meshes(VoxelWorld *world)
{
    bool ok = true;
    int per_pass = mesh_rebuild_chunks_per_pass();
    int limit = (per_pass <= 0) ? INT_MAX : per_pass;
    int rebuilt = 0;

    if (!world)
        return false;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED) ||
            !(chunk->flags & CHUNK_FLAG_MESH_DIRTY))
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
    int out = 0;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk chunk = world->chunks[i];
        bool keep = (chunk.flags & CHUNK_FLAG_LOADED) &&
                    chunk_in_window(chunk.chunk_x, chunk.chunk_z,
                                    origin_chunk_x, origin_chunk_z,
                                    diameter);

        if (keep) {
            if (out != i) {
                world->chunks[out] = chunk;
                memset(&world->chunks[i], 0, sizeof(world->chunks[i]));
            }
            world->chunks[out].last_used_epoch = world->stream_epoch;
            world->chunks_reused_last_stream++;
            out++;
        } else {
            release_chunk(&world->chunks[i]);
        }
    }

    for (int i = out; i < world->chunk_capacity; i++) {
        if (i >= world->chunk_count)
            memset(&world->chunks[i], 0, sizeof(world->chunks[i]));
    }

    world->chunk_count = out;
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

    if (world->chunk_count == world->chunk_capacity &&
        world->center_chunk_x == center_chunk_x &&
        world->center_chunk_z == center_chunk_z &&
        world->origin_chunk_x == origin_chunk_x &&
        world->origin_chunk_z == origin_chunk_z &&
        world->chunks_x == diameter &&
        world->chunks_z == diameter) {
        if (world->chunk_count > 0 &&
            chunk_lookup_find_index(world,
                                    world->chunks[0].chunk_x,
                                    world->chunks[0].chunk_z) < 0 &&
            !rebuild_chunk_lookup(world))
            return false;
        world->chunks_reused_last_stream = world->chunk_count;
        return world_rebuild_dirty_meshes(world);
    }

    world->stream_epoch++;
    if (world->stream_epoch == 0)
        world->stream_epoch = 1;

    int old_origin_x = world->origin_chunk_x;
    int old_origin_z = world->origin_chunk_z;

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

    for (int cz = origin_chunk_z; cz < origin_chunk_z + diameter; cz++) {
        for (int cx = origin_chunk_x; cx < origin_chunk_x + diameter; cx++) {
            if (chunk_lookup_find_index(world, cx, cz) >= 0)
                continue;
            if (world->chunk_count >= world->chunk_capacity)
                return false;

            Chunk *chunk = &world->chunks[world->chunk_count];
            initialize_chunk_slot(chunk, cx, cz, world->stream_epoch);
            generate_chunk_terrain(chunk,
                                   world->procedural_seed,
                                   world->stone_tries_per_chunk);
            if (!load_chunk_snapshot(world, chunk))
                return false;

            world->chunk_count++;
            if (!chunk_lookup_insert(world, world->chunk_count - 1))
                return false;

            world->chunks_generated_last_stream++;
            mark_chunk_and_neighbors_dirty(world, cx, cz);
        }
    }

    if (!world_rebuild_lighting(world))
        return false;

    mark_trailing_perimeter_dirty(world,
                                  old_origin_x, old_origin_z,
                                  origin_chunk_x, origin_chunk_z,
                                  diameter);
    mark_near_far_transitions_dirty(world);
    return world_rebuild_dirty_meshes(world);
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
    if (!world)
        return false;

    return stream_world_to_chunk_center(world,
                                        chunk_coord_from_world(world_x),
                                        chunk_coord_from_world(world_z));
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

bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type)
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
    if (!chunk)
        return false;

    old_type = chunk->blocks[wy][lz][lx];
    if (old_type == type)
        return true;

    chunk->blocks[wy][lz][lx] = type;
    chunk->flags |= CHUNK_FLAG_MODIFIED;
    chunk->flags |= CHUNK_FLAG_MESH_DIRTY;
    chunk->generation++;

    if (lx == 0) { Chunk *n = world_get_chunk_mut(world, chunk_x - 1, chunk_z); if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY; }
    if (lx == WORLD_CHUNK_SIZE - 1) { Chunk *n = world_get_chunk_mut(world, chunk_x + 1, chunk_z); if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY; }
    if (lz == 0) { Chunk *n = world_get_chunk_mut(world, chunk_x, chunk_z - 1); if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY; }
    if (lz == WORLD_CHUNK_SIZE - 1) { Chunk *n = world_get_chunk_mut(world, chunk_x, chunk_z + 1); if (n) n->flags |= CHUNK_FLAG_MESH_DIRTY; }

    update_column_sky_light(world, wx, wz);

    old_emission = block_emission_level(old_type);
    new_emission = block_emission_level(type);
    current_light = world_get_block_light(world, wx, wy, wz);

    if (block_blocks_light(type)) {
        if (current_light > 0) {
            world_set_block_light(world, wx, wy, wz, 0);
            success &= light_removal_queue_push(&rem_queue, &rem_cap, &rem_tail,
                                                (LightRemovalNode){.wx = wx, .wy = wy, .wz = wz, .val = current_light});
        }
    } else {
        if (old_emission > 0) {
            world_set_block_light(world, wx, wy, wz, 0);
            success &= light_removal_queue_push(&rem_queue, &rem_cap, &rem_tail,
                                                (LightRemovalNode){.wx = wx, .wy = wy, .wz = wz, .val = current_light});
        }
        if (new_emission > 0) {
            world_set_block_light(world, wx, wy, wz, new_emission);
            success &= light_queue_push(&add_queue, &add_cap, &add_tail,
                                        (LightNode){.wx = wx, .wy = wy, .wz = wz});
        } else {
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

    world->meshes_rebuilt_last_stream = 0;
    world->meshes_dirty = true;
    return success;
}

int world_total_faces(const VoxelWorld *world)
{
    int total = 0;

    if (!world)
        return 0;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];
        if ((chunk->flags & CHUNK_FLAG_LOADED) &&
            (chunk->flags & CHUNK_FLAG_MESH_READY))
            total += chunk->face_count;
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
