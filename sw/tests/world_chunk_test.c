#include <stdio.h>

#include "block_types.h"
#include "world.h"

#define TEST_RENDER_DISTANCE 2
#define TEST_WORLD_SEED 0x1234abcdu

static int check_failed(const char *message)
{
    fprintf(stderr, "world_chunk_test: %s\n", message);
    return 1;
}

static int expected_loaded_chunks(int render_distance)
{
    int load_radius = render_distance + 1;
    int diameter = load_radius * 2 + 1;

    return diameter * diameter;
}

int main(void)
{
    VoxelWorld world;
    int expected_chunks = expected_loaded_chunks(TEST_RENDER_DISTANCE);

    init_block_types();
    world_init(&world);

    if (!world_init_infinite_procedural(&world,
                                        TEST_WORLD_SEED,
                                        12,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f))
        return check_failed("initial procedural load failed");

    if (world.chunk_count != expected_chunks ||
        world_loaded_chunk_count(&world) != expected_chunks ||
        world_chunk_capacity(&world) != expected_chunks)
        return check_failed("initial chunk window did not fill capacity");
    if (world.chunks_generated_last_stream != expected_chunks)
        return check_failed("initial stream did not generate every chunk");
    if (world.meshes_rebuilt_last_stream != expected_chunks)
        return check_failed("initial stream did not build every chunk mesh");
    if (world_total_blocks(&world) <= 0 || world_total_faces(&world) <= 0)
        return check_failed("initial world has no cached blocks/faces");
    if (!world_get_chunk(&world, 0, 0))
        return check_failed("center chunk missing after initial load");

    if (!world_stream_around(&world, 1.0f, 1.0f))
        return check_failed("same-center stream failed");
    if (world.chunks_generated_last_stream != 0)
        return check_failed("same-center stream generated chunks");
    if (world.chunks_reused_last_stream != expected_chunks)
        return check_failed("same-center stream did not report all chunks reused");
    if (world.meshes_rebuilt_last_stream != 0)
        return check_failed("same-center stream rebuilt clean meshes");

    if (!world_stream_around(&world,
                             (float)WORLD_CHUNK_SIZE + 1.0f,
                             1.0f))
        return check_failed("one-chunk stream failed");
    if (world.chunk_count != expected_chunks)
        return check_failed("one-chunk stream changed loaded chunk count");
    if (world.chunks_generated_last_stream != world.chunks_z)
        return check_failed("one-chunk stream did not generate exactly one column");
    if (world.chunks_reused_last_stream != expected_chunks - world.chunks_z)
        return check_failed("one-chunk stream did not reuse the overlap");
    if (world.meshes_rebuilt_last_stream <= world.chunks_generated_last_stream)
        return check_failed("one-chunk stream did not rebuild new-neighbor meshes");
    if (world_get_chunk(&world, -3, 0))
        return check_failed("evicted chunk is still addressable");
    if (!world_get_chunk(&world, 4, 0))
        return check_failed("new streamed chunk is missing");

    const Chunk *edited = world_get_chunk(&world, 1, 0);
    const Chunk *neighbor = world_get_chunk(&world, 0, 0);
    if (!edited || !neighbor)
        return check_failed("edit test chunks missing");
    uint32_t edited_generation = edited->generation;
    uint32_t neighbor_generation = neighbor->generation;

    if (!world_set_block(&world, WORLD_CHUNK_SIZE, 1, 0, BLOCK_WOOD))
        return check_failed("boundary block edit failed");
    if (world_get_block(&world, WORLD_CHUNK_SIZE, 1, 0) != BLOCK_WOOD)
        return check_failed("boundary block edit did not persist in loaded chunk");

    edited = world_get_chunk(&world, 1, 0);
    neighbor = world_get_chunk(&world, 0, 0);
    if (!edited || !neighbor)
        return check_failed("edit chunks disappeared");
    if (!(edited->flags & CHUNK_FLAG_MODIFIED))
        return check_failed("edited chunk was not marked modified");
    if (!(edited->flags & CHUNK_FLAG_MESH_READY) ||
        (edited->flags & CHUNK_FLAG_MESH_DIRTY))
        return check_failed("edited chunk mesh was not rebuilt");
    if (!(neighbor->flags & CHUNK_FLAG_MESH_READY) ||
        (neighbor->flags & CHUNK_FLAG_MESH_DIRTY))
        return check_failed("boundary neighbor mesh was not rebuilt");
    if (edited->generation == edited_generation)
        return check_failed("edited chunk generation did not advance");
    if (neighbor->generation != neighbor_generation)
        return check_failed("unchanged neighbor content generation changed");
    if (world.meshes_rebuilt_last_stream < 2)
        return check_failed("boundary edit did not rebuild multiple meshes");

    if (!world_stream_around(&world,
                             (float)(-2 * WORLD_CHUNK_SIZE - 1),
                             (float)(-WORLD_CHUNK_SIZE - 1)))
        return check_failed("negative-coordinate stream failed");
    if (world_get_block(&world,
                        -2 * WORLD_CHUNK_SIZE - 1,
                        0,
                        -WORLD_CHUNK_SIZE - 1) == BLOCK_AIR)
        return check_failed("negative-coordinate ground lookup failed");

    world_free(&world);
    printf("world_chunk_test: ok\n");
    return 0;
}
