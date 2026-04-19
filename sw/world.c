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
        free(world->chunks[i].quads);

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

        if (fabsf((float)wx) <= 1.0f && wz <= 5)
            continue;
        if (chunk->blocks[1][lz][lx] != BLOCK_AIR)
            continue;

        for (int y = 1; y <= height; y++)
            chunk->blocks[y][lz][lx] = BLOCK_STONE;
    }
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
                    if (world_get_block(world, wx + nx[f], y + ny[f], wz + nz[f]) == BLOCK_AIR)
                        count++;
                }
            }
        }
    }

    return count;
}

/*
 * Greedy mesh a chunk: merge adjacent same-type coplanar exposed faces into
 * larger quads.  This eliminates thousands of internal shared edges on flat
 * terrain, removing the Q24.8 rounding-induced cracks that the fill rule
 * cannot reliably prevent at those precision levels.
 *
 * For each face direction we iterate over "layers" (slices perpendicular to
 * the face normal) and run a 2D greedy rectangle merge on the 16×16 type grid.
 *
 * Face dimension layout (fixed = coordinate perpendicular to face, d0/d1 are
 * the two axes in the face plane):
 *   TOP/BOTTOM : fixed=y,  d0=x, d1=z   (w=x-extent, h=z-extent)
 *   LEFT/RIGHT : fixed=x,  d0=z, d1=y   (w=z-extent, h=y-extent)
 *   BACK/FRONT : fixed=z,  d0=x, d1=y   (w=x-extent, h=y-extent)
 */
static void rebuild_chunk_quads(VoxelWorld *world, Chunk *chunk)
{
    static const int nx[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
    static const int ny[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
    static const int nz[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };
    /* For each face: which local coord is the "layer" axis */
    static const int face_fixed_dim[NUM_FACES] = { 1, 1, 0, 0, 2, 2 };
    /* First grid axis (d0) */
    static const int face_d0_dim[NUM_FACES]    = { 0, 0, 2, 2, 0, 0 };
    /* Second grid axis (d1) */
    static const int face_d1_dim[NUM_FACES]    = { 2, 2, 1, 1, 1, 1 };

    int max_quads = count_exposed_faces_for_chunk(world, chunk);

    if (max_quads == 0) {
        free(chunk->quads);
        chunk->quads = NULL;
        chunk->quad_count = 0;
        return;
    }

    ChunkQuad *quads = malloc((size_t)max_quads * sizeof(*quads));
    if (!quads)
        return;

    int out = 0;
    int wx_base = chunk->chunk_x * WORLD_CHUNK_SIZE;
    int wz_base = chunk->chunk_z * WORLD_CHUNK_SIZE;

    /* All chunk dimensions are 16 in both SIZE and HEIGHT. */
    static const int dim_max[3] = {
        WORLD_CHUNK_SIZE,
        WORLD_CHUNK_HEIGHT,
        WORLD_CHUNK_SIZE,
    };

    uint8_t type_grid[16][16];
    uint8_t done[16][16];

    for (int f = 0; f < NUM_FACES; f++) {
        int fixed_dim = face_fixed_dim[f];
        int d0_dim    = face_d0_dim[f];
        int d1_dim    = face_d1_dim[f];
        int layer_max = dim_max[fixed_dim];
        int d0_max    = dim_max[d0_dim];
        int d1_max    = dim_max[d1_dim];

        for (int layer = 0; layer < layer_max; layer++) {
            /* Build the 2D type grid for this layer/face. */
            for (int d0 = 0; d0 < d0_max; d0++) {
                for (int d1 = 0; d1 < d1_max; d1++) {
                    int local[3];
                    local[fixed_dim] = layer;
                    local[d0_dim]    = d0;
                    local[d1_dim]    = d1;

                    int lx = local[0], ly = local[1], lz = local[2];
                    BlockID id = chunk->blocks[ly][lz][lx];

                    if (id == BLOCK_AIR) {
                        type_grid[d0][d1] = 0;
                        continue;
                    }

                    int wx = wx_base + lx + nx[f];
                    int wy = ly + ny[f];
                    int wz = wz_base + lz + nz[f];

                    type_grid[d0][d1] =
                        (world_get_block(world, wx, wy, wz) == BLOCK_AIR)
                        ? (uint8_t)id : 0;
                }
            }

            /* Greedy 2D rectangle merge. */
            memset(done, 0, (size_t)d0_max * sizeof(done[0]));
            for (int d0 = 0; d0 < d0_max; d0++) {
                for (int d1 = 0; d1 < d1_max; d1++) {
                    uint8_t t = type_grid[d0][d1];
                    if (!t || done[d0][d1])
                        continue;

                    /* Extend in d0. */
                    int w = 1;
                    while (d0 + w < d0_max &&
                           type_grid[d0 + w][d1] == t &&
                           !done[d0 + w][d1])
                        w++;

                    /* Extend in d1 while entire d0-row matches. */
                    int h = 1;
                    while (d1 + h < d1_max) {
                        bool ok = true;
                        for (int i = 0; i < w; i++) {
                            if (type_grid[d0 + i][d1 + h] != t ||
                                done[d0 + i][d1 + h]) {
                                ok = false;
                                break;
                            }
                        }
                        if (!ok)
                            break;
                        h++;
                    }

                    /* Mark rectangle as processed. */
                    for (int i = 0; i < w; i++)
                        for (int j = 0; j < h; j++)
                            done[d0 + i][d1 + j] = 1;

                    if (out < max_quads) {
                        int local[3];
                        local[fixed_dim] = layer;
                        local[d0_dim]    = d0;
                        local[d1_dim]    = d1;
                        quads[out++] = (ChunkQuad){
                            .x    = (uint8_t)local[0],
                            .y    = (uint8_t)local[1],
                            .z    = (uint8_t)local[2],
                            .w    = (uint8_t)w,
                            .h    = (uint8_t)h,
                            .face = (uint8_t)f,
                            .type = t,
                        };
                    }
                }
            }
        }
    }

    free(chunk->quads);
    chunk->quads = quads;
    chunk->quad_count = out;
}

static void rebuild_chunk_if_present(VoxelWorld *world, int chunk_x, int chunk_z)
{
    Chunk *chunk = world_get_chunk_mut(world, chunk_x, chunk_z);

    if (chunk)
        rebuild_chunk_quads(world, chunk);
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
        rebuild_chunk_quads(world, &world->chunks[i]);

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

bool world_generate_flat_random_stone(VoxelWorld *world,
                                      int origin_chunk_x,
                                      int origin_chunk_z,
                                      int chunks_x,
                                      int chunks_z,
                                      uint32_t seed,
                                      int stone_tries_per_chunk,
                                      int render_distance_chunks)
{
    if (!world)
        return false;

    if (!ensure_chunk_storage(world, chunks_x, chunks_z))
        return false;

    world->procedural_seed = seed;
    world->stone_tries_per_chunk = stone_tries_per_chunk;
    world->render_distance_chunks = render_distance_chunks;
    world->load_radius_chunks = render_distance_chunks;
    world->center_chunk_x = origin_chunk_x + chunks_x / 2;
    world->center_chunk_z = origin_chunk_z + chunks_z / 2;
    return populate_chunk_window(world, origin_chunk_x, origin_chunk_z);
}

bool world_init_infinite_flat_random_stone(VoxelWorld *world,
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
    rebuild_chunk_quads(world, chunk);

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
        total += world->chunks[i].quad_count;

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
