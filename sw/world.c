#include "world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static void free_chunk_storage(VoxelWorld *world)
{
    if (!world || !world->chunks)
        return;

    for (int i = 0; i < world->chunk_count; i++)
        free(world->chunks[i].faces);

    free(world->chunks);
    world->chunks = NULL;
    world->chunk_count = 0;
    world->chunks_x = 0;
    world->chunks_z = 0;
    world->origin_chunk_x = 0;
    world->origin_chunk_z = 0;
    world->center_chunk_x = 0;
    world->center_chunk_z = 0;
}

static int chunk_index_for_coord(const VoxelWorld *world, int chunk_x, int chunk_z)
{
    int local_x = chunk_x - world->origin_chunk_x;
    int local_z = chunk_z - world->origin_chunk_z;

    if (local_x < 0 || local_x >= world->chunks_x)
        return -1;
    if (local_z < 0 || local_z >= world->chunks_z)
        return -1;

    return local_z * world->chunks_x + local_x;
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

static void clear_chunk(Chunk *chunk)
{
    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++)
                chunk->blocks[y][z][x] = BLOCK_AIR;
        }
    }
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
    int index;

    if (!world || !world->chunks)
        return NULL;

    index = chunk_index_for_coord(world, chunk_x, chunk_z);
    if (index < 0)
        return NULL;

    return &world->chunks[index];
}

static Chunk *world_get_chunk_mut(VoxelWorld *world, int chunk_x, int chunk_z)
{
    int index;

    if (!world || !world->chunks)
        return NULL;

    index = chunk_index_for_coord(world, chunk_x, chunk_z);
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

    if (!chunk)
        return BLOCK_AIR;

    return chunk->blocks[wy][lz][lx];
}

static void generate_chunk_terrain(Chunk *chunk, uint32_t base_seed,
                                   int stone_tries_per_chunk)
{
    uint32_t seed = chunk_seed(base_seed, chunk->chunk_x, chunk->chunk_z);

    clear_chunk(chunk);

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

static void rebuild_chunk_faces(VoxelWorld *world, Chunk *chunk)
{
    static const int nx[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
    static const int ny[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
    static const int nz[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };
    int max_face_count = count_exposed_faces_for_chunk(world, chunk);
    ChunkFace *faces = NULL;
    int out = 0;

    if (max_face_count == 0) {
        free(chunk->faces);
        chunk->faces = NULL;
        chunk->face_count = 0;
        return;
    }

    faces = malloc((size_t)max_face_count * sizeof(*faces));
    if (!faces)
        return;

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
                        append_chunk_face(faces, &out, x, y, z,
                                          (BlockFace)f, id, 1, 1);
                        continue;
                    }

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

                    append_chunk_face(faces, &out, x, y, z,
                                      (BlockFace)f, id, merge_w, merge_h);
                }
            }
        }
    }

    free(chunk->faces);
    chunk->faces = faces;
    chunk->face_count = out;
}

static void rebuild_chunk_if_present(VoxelWorld *world, int chunk_x, int chunk_z)
{
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);

    if (chunk)
        rebuild_chunk_faces(world, chunk);
}

static int chunk_coord_from_world(float world_pos)
{
    return (int)floorf(world_pos / (float)WORLD_CHUNK_SIZE);
}

static bool ensure_chunk_storage(VoxelWorld *world, int chunks_x, int chunks_z)
{
    int chunk_count = chunks_x * chunks_z;

    if (chunks_x <= 0 || chunks_z <= 0)
        return false;

    if (world->chunks &&
        world->chunks_x == chunks_x &&
        world->chunks_z == chunks_z &&
        world->chunk_count == chunk_count)
        return true;

    free_chunk_storage(world);

    world->chunks = calloc((size_t)chunk_count, sizeof(*world->chunks));
    if (!world->chunks)
        return false;

    world->chunk_count = chunk_count;
    world->chunks_x = chunks_x;
    world->chunks_z = chunks_z;
    return true;
}

static bool populate_chunk_window(VoxelWorld *world, int origin_chunk_x, int origin_chunk_z)
{
    int ci = 0;

    if (!world || !world->chunks)
        return false;

    world->origin_chunk_x = origin_chunk_x;
    world->origin_chunk_z = origin_chunk_z;

    for (int cz = 0; cz < world->chunks_z; cz++) {
        for (int cx = 0; cx < world->chunks_x; cx++) {
            Chunk *chunk = &world->chunks[ci++];

            chunk->chunk_x = origin_chunk_x + cx;
            chunk->chunk_z = origin_chunk_z + cz;

            generate_chunk_terrain(chunk,
                                   world->procedural_seed,
                                   world->stone_tries_per_chunk);
        }
    }

    for (int i = 0; i < world->chunk_count; i++)
        rebuild_chunk_faces(world, &world->chunks[i]);

    return true;
}

static bool stream_world_to_chunk_center(VoxelWorld *world,
                                         int center_chunk_x,
                                         int center_chunk_z)
{
    int origin_chunk_x;
    int origin_chunk_z;
    int diameter;

    if (!world)
        return false;

    diameter = world->load_radius_chunks * 2 + 1;
    origin_chunk_x = center_chunk_x - world->load_radius_chunks;
    origin_chunk_z = center_chunk_z - world->load_radius_chunks;

    if (world->chunks &&
        world->center_chunk_x == center_chunk_x &&
        world->center_chunk_z == center_chunk_z &&
        world->origin_chunk_x == origin_chunk_x &&
        world->origin_chunk_z == origin_chunk_z &&
        world->chunks_x == diameter &&
        world->chunks_z == diameter)
        return true;

    if (!ensure_chunk_storage(world, diameter, diameter))
        return false;

    world->center_chunk_x = center_chunk_x;
    world->center_chunk_z = center_chunk_z;
    return populate_chunk_window(world, origin_chunk_x, origin_chunk_z);
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

    if (!world || wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
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
    rebuild_chunk_faces(world, chunk);

    if (lx == 0)
        rebuild_chunk_if_present(world, chunk_x - 1, chunk_z);
    if (lx == WORLD_CHUNK_SIZE - 1)
        rebuild_chunk_if_present(world, chunk_x + 1, chunk_z);
    if (lz == 0)
        rebuild_chunk_if_present(world, chunk_x, chunk_z - 1);
    if (lz == WORLD_CHUNK_SIZE - 1)
        rebuild_chunk_if_present(world, chunk_x, chunk_z + 1);

    return true;
}

int world_total_faces(const VoxelWorld *world)
{
    int total = 0;

    for (int i = 0; i < world->chunk_count; i++)
        total += world->chunks[i].face_count;

    return total;
}

int world_total_blocks(const VoxelWorld *world)
{
    int total = 0;

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

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
