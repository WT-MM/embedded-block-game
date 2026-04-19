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

    if (world->chunks) {
        for (int i = 0; i < world->chunk_count; i++)
            free(world->chunks[i].faces);
    }

    free(world->chunks);
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

static void rebuild_chunk_faces(VoxelWorld *world, Chunk *chunk)
{
    static const int nx[NUM_FACES] = { 0, 0, -1, 1, 0, 0 };
    static const int ny[NUM_FACES] = { 1, -1, 0, 0, 0, 0 };
    static const int nz[NUM_FACES] = { 0, 0, 0, 0, -1, 1 };
    int face_count = count_exposed_faces_for_chunk(world, chunk);
    ChunkFace *faces = NULL;
    int out = 0;

    if (face_count == 0) {
        free(chunk->faces);
        chunk->faces = NULL;
        chunk->face_count = 0;
        return;
    }

    faces = malloc((size_t)face_count * sizeof(*faces));
    if (!faces)
        return;

    for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                BlockID id = chunk->blocks[y][z][x];

                if (id == BLOCK_AIR)
                    continue;

                int wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x;
                int wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z;

                for (int f = 0; f < NUM_FACES; f++) {
                    if (world_get_block(world, wx + nx[f], y + ny[f], wz + nz[f]) != BLOCK_AIR)
                        continue;

                    faces[out++] = (ChunkFace){
                        .x = (uint8_t)x,
                        .y = (uint8_t)y,
                        .z = (uint8_t)z,
                        .face = (uint8_t)f,
                        .type = (uint8_t)id,
                    };
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

bool world_generate_flat_random_stone(VoxelWorld *world,
                                      int origin_chunk_x,
                                      int origin_chunk_z,
                                      int chunks_x,
                                      int chunks_z,
                                      uint32_t seed,
                                      int stone_tries_per_chunk,
                                      int render_distance_chunks)
{
    int chunk_count = chunks_x * chunks_z;

    if (chunks_x <= 0 || chunks_z <= 0)
        return false;

    world_free(world);
    world_init(world);

    world->chunks = calloc((size_t)chunk_count, sizeof(*world->chunks));
    if (!world->chunks)
        return false;

    world->chunk_count = chunk_count;
    world->chunks_x = chunks_x;
    world->chunks_z = chunks_z;
    world->origin_chunk_x = origin_chunk_x;
    world->origin_chunk_z = origin_chunk_z;
    world->render_distance_chunks = render_distance_chunks;

    int ci = 0;
    for (int cz = 0; cz < chunks_z; cz++) {
        for (int cx = 0; cx < chunks_x; cx++) {
            Chunk *chunk = &world->chunks[ci++];

            chunk->chunk_x = origin_chunk_x + cx;
            chunk->chunk_z = origin_chunk_z + cz;
            chunk->faces = NULL;
            chunk->face_count = 0;

            generate_chunk_terrain(chunk, seed, stone_tries_per_chunk);
        }
    }

    for (int i = 0; i < world->chunk_count; i++)
        rebuild_chunk_faces(world, &world->chunks[i]);

    return true;
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
