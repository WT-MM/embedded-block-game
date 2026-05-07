#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void cleanup_temp_world_dir(const char *world_dir)
{
    char path[WORLD_SAVE_PATH_MAX];
    DIR *chunks_dir;
    struct dirent *entry;

    if (!world_dir || world_dir[0] == '\0')
        return;

    snprintf(path, sizeof(path), "%s/chunks", world_dir);
    chunks_dir = opendir(path);
    if (chunks_dir) {
        while ((entry = readdir(chunks_dir)) != NULL) {
            char chunk_path[WORLD_SAVE_PATH_MAX];

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            snprintf(chunk_path, sizeof(chunk_path), "%s/%s", path, entry->d_name);
            unlink(chunk_path);
        }
        closedir(chunks_dir);
        rmdir(path);
    }

    snprintf(path, sizeof(path), "%s/world.meta", world_dir);
    unlink(path);
    rmdir(world_dir);
}

static const ChunkFace *find_chunk_face(const Chunk *chunk,
                                        int x, int y, int z,
                                        BlockFace face, BlockID type)
{
    const ChunkMesh *mesh;

    if (!chunk)
        return NULL;
    mesh = atomic_load_explicit(&chunk->live_mesh, memory_order_acquire);
    if (!mesh)
        return NULL;

    for (int i = 0; i < mesh->face_count; i++) {
        const ChunkFace *candidate = &mesh->faces[i];

        if ((int)candidate->x == x &&
            (int)candidate->y == y &&
            (int)candidate->z == z &&
            (BlockFace)candidate->face == face &&
            (BlockID)candidate->type == type)
            return candidate;
    }

    return NULL;
}

int main(void)
{
    VoxelWorld world;
    VoxelWorld reloaded_world;
    int expected_chunks = expected_loaded_chunks(TEST_RENDER_DISTANCE);
    char world_dir_template[] = "/tmp/voxel-world-test-XXXXXX";
    char *world_dir;

    init_block_types();
    world_init(&world);
    world_init(&reloaded_world);
    world_dir = mkdtemp(world_dir_template);
    if (!world_dir)
        return check_failed("mkdtemp failed");

    if (!world_init_infinite_procedural(&world,
                                        TEST_WORLD_SEED,
                                        12,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f,
                                        world_dir))
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
    if (block_emission_level(BLOCK_LAMP) != 15 || !block_is_self_lit(BLOCK_LAMP))
        return check_failed("lamp metadata missing");

    const int lamp_x = 2;
    const int lamp_y = 8;
    const int lamp_z = 2;
    const int gap_x = lamp_x + 1;
    const int stone_x = lamp_x + 2;
    if (!world_set_block(&world, gap_x, lamp_y, lamp_z, BLOCK_AIR))
        return check_failed("lamp air gap clear failed");
    if (!world_set_block(&world, lamp_x, lamp_y, lamp_z, BLOCK_LAMP))
        return check_failed("lamp placement failed");
    if (!world_set_block(&world, stone_x, lamp_y, lamp_z, BLOCK_STONE))
        return check_failed("stone placement near lamp failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-edit mesh rebuild failed");

    const Chunk *lighting_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *lamp_top = find_chunk_face(lighting_chunk,
                                                lamp_x, lamp_y, lamp_z,
                                                FACE_TOP, BLOCK_LAMP);
    const ChunkFace *stone_left = find_chunk_face(lighting_chunk,
                                                  stone_x, lamp_y, lamp_z,
                                                  FACE_LEFT, BLOCK_STONE);
    if (!lamp_top || lamp_top->block_light != 15)
        return check_failed("lamp face was not self-lit");
    if (!stone_left || stone_left->block_light < 14)
        return check_failed("nearby visible face did not receive lamp light");

    if (!world_set_block(&world, lamp_x, lamp_y, lamp_z, BLOCK_AIR))
        return check_failed("lamp removal failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-removal mesh rebuild failed");
    lighting_chunk = world_get_chunk(&world, 0, 0);
    stone_left = find_chunk_face(lighting_chunk,
                                 stone_x, lamp_y, lamp_z,
                                 FACE_LEFT, BLOCK_STONE);
    if (!stone_left || stone_left->block_light != 0)
        return check_failed("lamp light did not clear after removal");
    if (!world_set_block(&world, stone_x, lamp_y, lamp_z, BLOCK_AIR))
        return check_failed("stone cleanup failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-cleanup mesh rebuild failed");

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
    world.meshes_rebuilt_last_stream = 0;
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-boundary-edit mesh rebuild failed");

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
                             (float)(5 * WORLD_CHUNK_SIZE + 1),
                             1.0f))
        return check_failed("eviction stream failed");
    if (world_get_chunk(&world, 1, 0))
        return check_failed("edited chunk did not stream out");
    if (!world_stream_around(&world,
                             (float)WORLD_CHUNK_SIZE + 1.0f,
                             1.0f))
        return check_failed("return stream failed");
    if (world_get_block(&world, WORLD_CHUNK_SIZE, 1, 0) != BLOCK_WOOD)
        return check_failed("saved block edit did not reload after eviction");

    if (!world_stream_around(&world,
                             (float)(-2 * WORLD_CHUNK_SIZE - 1),
                             (float)(-WORLD_CHUNK_SIZE - 1)))
        return check_failed("negative-coordinate stream failed");
    if (world_get_block(&world,
                        -2 * WORLD_CHUNK_SIZE - 1,
                        0,
                        -WORLD_CHUNK_SIZE - 1) == BLOCK_AIR)
        return check_failed("negative-coordinate ground lookup failed");

    if (!world_flush(&world))
        return check_failed("world_flush failed");
    world_free(&world);

    if (!world_init_infinite_procedural(&reloaded_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f,
                                        world_dir))
        return check_failed("reload world_init_infinite_procedural failed");
    if (world_get_block(&reloaded_world, WORLD_CHUNK_SIZE, 1, 0) != BLOCK_WOOD)
        return check_failed("saved block edit did not survive world reload");
    world_free(&reloaded_world);
    cleanup_temp_world_dir(world_dir);

    printf("world_chunk_test: ok\n");
    return 0;
}
