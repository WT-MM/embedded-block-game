#include "world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_LOOKUP_EMPTY (-1)

/* Greedy face merging runs only on chunks farther than this (Chebyshev)
 * distance from the player's chunk. Near chunks emit 1x1 faces so the
 * T-junction / UV-shimmer artifacts caused by merged quads stay out of
 * the viewer's foreground, while distant chunks (which dominate face
 * counts at large render distances) still get the merge win. */
#define NEAR_CHUNK_RADIUS 3

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

static void initialize_chunk_slot(Chunk *chunk, int chunk_x, int chunk_z,
                                  uint32_t stream_epoch)
{
    uint32_t generation = chunk->generation + 1u;

    clear_chunk_blocks(chunk);
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

static void mark_window_perimeter_dirty(VoxelWorld *world,
                                        int origin_chunk_x,
                                        int origin_chunk_z,
                                        int diameter)
{
    int max_chunk_x = origin_chunk_x + diameter - 1;
    int max_chunk_z = origin_chunk_z + diameter - 1;

    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        if (chunk->chunk_x == origin_chunk_x ||
            chunk->chunk_x == max_chunk_x ||
            chunk->chunk_z == origin_chunk_z ||
            chunk->chunk_z == max_chunk_z)
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

static int count_exposed_faces_for_chunk(const VoxelWorld *world, const Chunk *chunk)
{
    static const int nx[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
    static const int ny[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
    static const int nz[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };
    int count = 0;

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                BlockID id = chunk->blocks[y][z][x];

                if (id == BLOCK_AIR)
                    continue;

                int wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x;
                int wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z;

                for (int f = 0; f < NUM_FACES; f++) {
                    BlockID neighbor = world_get_block(world,
                                                       wx + nx[f],
                                                       y + ny[f],
                                                       wz + nz[f]);
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
                              int u_size, int v_size)
{
    faces[(*out)++] = (ChunkFace){
        .x = (uint8_t)x,
        .y = (uint8_t)y,
        .z = (uint8_t)z,
        .face = (uint8_t)face,
        .type = (uint8_t)type,
        .u_size = (uint8_t)u_size,
        .v_size = (uint8_t)v_size,
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
    static const int nx[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
    static const int ny[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
    static const int nz[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };
    int max_face_count;
    int out = 0;

    if (!world || !chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return false;

    bool is_near = chunk_is_near(world, chunk);

    max_face_count = count_exposed_faces_for_chunk(world, chunk);
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
            bool used[WORLD_CHUNK_HEIGHT][WORLD_CHUNK_SIZE] = {{0}};

            for (int v = 0; v < height; v++) {
                for (int u = 0; u < width; u++) {
                    int x, y, z;
                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);

                    BlockID id = chunk->blocks[y][z][x];
                    if (id == BLOCK_AIR)
                        continue;

                    int wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x;
                    int wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z;
                    BlockID neighbor = world_get_block(world,
                                                       wx + nx[f],
                                                       y + ny[f],
                                                       wz + nz[f]);
                    if (face_should_render(id, neighbor))
                        mask[v][u] = id;
                }
            }

            for (int v = 0; v < height; v++) {
                for (int u = 0; u < width; u++) {
                    BlockID id = mask[v][u];
                    int merge_w = 1;
                    int merge_h = 1;
                    int x, y, z;

                    if (id == BLOCK_AIR || used[v][u])
                        continue;

                    face_cell_to_block((BlockFace)f, layer, u, v, &x, &y, &z);
                    if (block_is_translucent(id)) {
                        used[v][u] = true;
                        append_chunk_face(chunk->faces, &out, x, y, z,
                                          (BlockFace)f, id, 1, 1);
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
                               mask[v][u + merge_w] == id) {
                            merge_w++;
                        }

                        bool can_extend = true;
                        while (v + merge_h < height && can_extend) {
                            for (int du = 0; du < merge_w; du++) {
                                if (used[v + merge_h][u + du] ||
                                    mask[v + merge_h][u + du] != id) {
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
                                      (BlockFace)f, id, merge_w, merge_h);
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

static bool rebuild_dirty_chunk_meshes(VoxelWorld *world)
{
    bool ok = true;

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
        return rebuild_dirty_chunk_meshes(world);
    }

    world->stream_epoch++;
    if (world->stream_epoch == 0)
        world->stream_epoch = 1;

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

            world->chunk_count++;
            if (!chunk_lookup_insert(world, world->chunk_count - 1))
                return false;

            world->chunks_generated_last_stream++;
            mark_chunk_and_neighbors_dirty(world, cx, cz);
        }
    }

    mark_window_perimeter_dirty(world, origin_chunk_x, origin_chunk_z, diameter);
    mark_near_far_transitions_dirty(world);
    return rebuild_dirty_chunk_meshes(world);
}

bool world_init_infinite_procedural(VoxelWorld *world,
                                    uint32_t seed,
                                    int stone_tries_per_chunk,
                                    int render_distance_chunks,
                                    float center_x,
                                    float center_z)
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

bool world_set_block(VoxelWorld *world, int wx, int wy, int wz, BlockID type)
{
    int chunk_x;
    int chunk_z;
    int lx;
    int lz;
    Chunk *chunk;

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

    if (chunk->blocks[wy][lz][lx] == type)
        return true;

    chunk->blocks[wy][lz][lx] = type;
    chunk->flags |= CHUNK_FLAG_MODIFIED;
    chunk->generation++;

    mark_chunk_mesh_dirty(chunk);
    if (lx == 0)
        mark_chunk_coord_dirty(world, chunk_x - 1, chunk_z);
    if (lx == WORLD_CHUNK_SIZE - 1)
        mark_chunk_coord_dirty(world, chunk_x + 1, chunk_z);
    if (lz == 0)
        mark_chunk_coord_dirty(world, chunk_x, chunk_z - 1);
    if (lz == WORLD_CHUNK_SIZE - 1)
        mark_chunk_coord_dirty(world, chunk_x, chunk_z + 1);

    world->meshes_rebuilt_last_stream = 0;
    return rebuild_dirty_chunk_meshes(world);
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
