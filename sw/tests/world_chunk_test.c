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

typedef struct {
    int grass_surfaces;
    int sand_surfaces;
    int clay_surfaces;
    int stone_surfaces;
    int water_blocks;
    int flowers;
    int mushrooms;
    int cacti;
    int lowest_surface_y;
    int highest_surface_y;
} WorldgenSample;

static bool block_is_worldgen_decoration(BlockID id)
{
    return id == BLOCK_WOOD ||
           id == BLOCK_LEAVES ||
           id == BLOCK_RED_FLOWER ||
           id == BLOCK_YELLOW_FLOWER ||
           id == BLOCK_RED_MUSHROOM ||
           id == BLOCK_BROWN_MUSHROOM ||
           id == BLOCK_CACTUS;
}

static WorldgenSample sample_worldgen(const VoxelWorld *world)
{
    WorldgenSample sample = {
        .lowest_surface_y = WORLD_CHUNK_HEIGHT,
        .highest_surface_y = -1,
    };

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
            for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                BlockID surface = BLOCK_AIR;
                int surface_y = -1;

                for (int y = WORLD_CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockID id = chunk->blocks[y][z][x];

                    if (id == BLOCK_WATER || id == BLOCK_WATER_FLOW)
                        sample.water_blocks++;
                    if (id == BLOCK_RED_FLOWER || id == BLOCK_YELLOW_FLOWER)
                        sample.flowers++;
                    if (id == BLOCK_RED_MUSHROOM ||
                        id == BLOCK_BROWN_MUSHROOM)
                        sample.mushrooms++;
                    if (id == BLOCK_CACTUS)
                        sample.cacti++;
                    if (id == BLOCK_AIR ||
                        id == BLOCK_WATER ||
                        id == BLOCK_WATER_FLOW ||
                        block_is_worldgen_decoration(id))
                        continue;

                    if (surface_y < 0) {
                        surface_y = y;
                        surface = id;
                    }
                }

                if (surface_y < 0)
                    continue;
                if (surface_y < sample.lowest_surface_y)
                    sample.lowest_surface_y = surface_y;
                if (surface_y > sample.highest_surface_y)
                    sample.highest_surface_y = surface_y;

                if (surface == BLOCK_GRASS)
                    sample.grass_surfaces++;
                else if (surface == BLOCK_SAND)
                    sample.sand_surfaces++;
                else if (surface == BLOCK_CLAY)
                    sample.clay_surfaces++;
                else if (surface == BLOCK_STONE)
                    sample.stone_surfaces++;
            }
        }
    }

    return sample;
}

int main(void)
{
    VoxelWorld world;
    VoxelWorld reloaded_world;
    VoxelWorld capped_world;
    VoxelWorld biome_world;
    int expected_chunks = expected_loaded_chunks(TEST_RENDER_DISTANCE);
    char world_dir_template[] = "/tmp/voxel-world-test-XXXXXX";
    char *world_dir;

    init_block_types();
    world_init(&world);
    world_init(&reloaded_world);
    world_init(&capped_world);
    world_init(&biome_world);
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
    if (world_near_chunk_radius(&world) != 1)
        return check_failed("default near chunk radius changed");
    if (world_stream_chunks_per_frame(&world) != 0)
        return check_failed("default stream chunk cap changed");

    WorldgenSample initial_sample = sample_worldgen(&world);
    if (initial_sample.grass_surfaces <= 0 ||
        initial_sample.sand_surfaces <= 0 ||
        initial_sample.clay_surfaces <= 0 ||
        initial_sample.water_blocks <= 0)
        return check_failed("biome worldgen did not create grass, beach, clay, and water");
    if (initial_sample.highest_surface_y - initial_sample.lowest_surface_y < 4)
        return check_failed("biome worldgen terrain was too flat");
    bool found_desert_biome = false;
    for (int z = -128; z <= 128 && !found_desert_biome; z += 16) {
        for (int x = -128; x <= 128; x += 16) {
            if (world_biome_at(&world, x, z) == WORLD_BIOME_DESERT) {
                found_desert_biome = true;
                break;
            }
        }
    }
    if (!found_desert_biome ||
        strcmp(world_biome_name(WORLD_BIOME_DESERT), "desert") != 0)
        return check_failed("desert biome was not reachable by worldgen");

    if (!world_init_infinite_procedural(&biome_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        3,
                                        0.0f,
                                        0.0f,
                                        NULL))
        return check_failed("mountain biome sample load failed");
    WorldgenSample mountain_sample = sample_worldgen(&biome_world);
    if (mountain_sample.stone_surfaces <= 0 ||
        mountain_sample.highest_surface_y < 22)
        return check_failed("mountain biome sample did not create rocky highlands");
    world_free(&biome_world);

    /* Most of this test validates full-window synchronous streaming behavior;
     * keep that mode explicit for the checks below. */
    world_set_stream_chunks_per_frame(&world, 0);
    if (block_emission_level(BLOCK_LAMP) != 15 || !block_is_self_lit(BLOCK_LAMP))
        return check_failed("lamp metadata missing");

    const int lamp_x = 2;
    /* Keep the lighting fixture high enough for this deterministic seed, then
     * explicitly clear/overwrite the cells it needs below. */
    const int lamp_y = 26;
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

    if (block_emission_level(BLOCK_LAVA) != 15 ||
        !block_is_self_lit(BLOCK_LAVA) ||
        !block_is_translucent(BLOCK_LAVA) ||
        !block_is_passable(BLOCK_LAVA) ||
        block_emission_level(BLOCK_LAVA_FLOW) != 15 ||
        !block_is_self_lit(BLOCK_LAVA_FLOW) ||
        !block_is_translucent(BLOCK_LAVA_FLOW) ||
        !block_is_passable(BLOCK_LAVA_FLOW))
        return check_failed("lava metadata missing");
    const int lava_x = 6;
    const int lava_y = 26;
    const int lava_z = 2;
    const int lava_gap_x = lava_x + 1;
    const int lava_stone_x = lava_x + 2;
    if (!world_set_block(&world, lava_gap_x, lava_y, lava_z, BLOCK_AIR))
        return check_failed("lava air gap clear failed");
    if (!world_set_block(&world, lava_x, lava_y, lava_z, BLOCK_LAVA))
        return check_failed("lava placement failed");
    if (!world_set_block(&world, lava_stone_x, lava_y, lava_z, BLOCK_STONE))
        return check_failed("stone placement near lava failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-lava mesh rebuild failed");
    const Chunk *lava_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *lava_lit_stone = find_chunk_face(lava_chunk,
                                                      lava_stone_x, lava_y, lava_z,
                                                      FACE_LEFT, BLOCK_STONE);
    if (!lava_lit_stone || lava_lit_stone->block_light < 14)
        return check_failed("nearby visible face did not receive lava light");
    if (!world_set_block(&world, lava_x, lava_y, lava_z, BLOCK_AIR) ||
        !world_set_block(&world, lava_stone_x, lava_y, lava_z, BLOCK_AIR))
        return check_failed("lava cleanup failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-lava-cleanup mesh rebuild failed");

    if (block_render_model(BLOCK_RED_FLOWER) != BLOCK_RENDER_CROSS ||
        !block_is_alpha_keyed(BLOCK_RED_FLOWER) ||
        !block_is_passable(BLOCK_RED_FLOWER) ||
        block_render_model(BLOCK_RED_MUSHROOM) != BLOCK_RENDER_CROSS ||
        !block_is_alpha_keyed(BLOCK_RED_MUSHROOM) ||
        !block_is_passable(BLOCK_RED_MUSHROOM) ||
        block_render_model(BLOCK_CACTUS) != BLOCK_RENDER_CUBE ||
        !block_is_alpha_keyed(BLOCK_CACTUS) ||
        block_is_passable(BLOCK_CACTUS))
        return check_failed("plant metadata missing");
    const int flower_x = 9;
    const int flower_y = 26;
    const int flower_z = 2;
    if (!world_set_block(&world, flower_x, flower_y, flower_z, BLOCK_RED_FLOWER))
        return check_failed("flower placement failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-flower mesh rebuild failed");
    const Chunk *flower_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *flower_cross_a = find_chunk_face(flower_chunk,
                                                      flower_x, flower_y, flower_z,
                                                      (BlockFace)CHUNK_FACE_CROSS_A,
                                                      BLOCK_RED_FLOWER);
    const ChunkFace *flower_cross_b = find_chunk_face(flower_chunk,
                                                      flower_x, flower_y, flower_z,
                                                      (BlockFace)CHUNK_FACE_CROSS_B,
                                                      BLOCK_RED_FLOWER);
    const ChunkFace *flower_cube_top = find_chunk_face(flower_chunk,
                                                       flower_x, flower_y, flower_z,
                                                       FACE_TOP, BLOCK_RED_FLOWER);
    if (!flower_cross_a || !flower_cross_b || flower_cube_top)
        return check_failed("flower did not mesh as crossed planes");
    if (!world_set_block(&world, flower_x, flower_y, flower_z, BLOCK_AIR))
        return check_failed("flower cleanup failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-flower-cleanup mesh rebuild failed");

    const int water_x = 4;
    const int water_y = 24;
    const int water_z = 4;
    const int water_floor_y = water_y - 1;
    for (int y = water_y - 6; y <= water_y + 2; y++) {
        for (int z = water_z - 1; z <= water_z + 1; z++) {
            for (int x = water_x - 1; x <= water_x + 3; x++) {
                if (!world_set_block(&world, x, y, z, BLOCK_AIR))
                    return check_failed("water test volume clear failed");
            }
        }
    }
    if (!world_set_block(&world, water_x, water_floor_y, water_z, BLOCK_STONE) ||
        !world_set_block(&world, water_x + 2, water_floor_y, water_z, BLOCK_STONE) ||
        !world_set_block(&world, water_x - 1, water_y, water_z, BLOCK_STONE) ||
        !world_set_block(&world, water_x, water_y, water_z - 1, BLOCK_STONE) ||
        !world_set_block(&world, water_x, water_y, water_z + 1, BLOCK_STONE) ||
        !world_set_block(&world, water_x + 1, water_y, water_z - 1, BLOCK_STONE) ||
        !world_set_block(&world, water_x + 1, water_y, water_z + 1, BLOCK_STONE))
        return check_failed("water test fixture build failed");
    if (!world_set_block(&world, water_x, water_y, water_z, BLOCK_WATER))
        return check_failed("water source placement failed");
    world_water_tick(&world);
    if (world_get_block(&world, water_x + 1, water_y, water_z) != BLOCK_WATER_FLOW ||
        world_get_block(&world, water_x + 1, water_floor_y, water_z) != BLOCK_WATER_FLOW)
        return check_failed("water left a suspended surface over the hole");
    for (int i = 0; i < 4; i++)
        world_water_tick(&world);
    if (world_get_block(&world, water_x + 1, water_y, water_z) != BLOCK_WATER_FLOW ||
        world_get_block(&world, water_x + 1, water_floor_y, water_z) != BLOCK_WATER_FLOW)
        return check_failed("water did not fall into the hole");
    if (world_get_block(&world, water_x + 2, water_y, water_z) != BLOCK_AIR)
        return check_failed("water skipped across a falling column");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-water mesh rebuild failed");
    const Chunk *water_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *fall_side = find_chunk_face(water_chunk,
                                                 water_x + 1, water_floor_y, water_z,
                                                 FACE_FRONT, BLOCK_WATER_FLOW);
    const ChunkFace *source_step = find_chunk_face(water_chunk,
                                                   water_x, water_y, water_z,
                                                   FACE_RIGHT, BLOCK_WATER);
    if (!fall_side || fall_side->height != 8)
        return check_failed("falling water side did not render full height");
    if (!source_step || source_step->height != 8)
        return check_failed("water step face between source and flow missing");
    if (!world_set_block(&world, water_x, water_y, water_z, BLOCK_AIR))
        return check_failed("water source removal failed");
    for (int i = 0; i < 8; i++)
        world_water_tick(&world);
    if (world_get_block(&world, water_x + 1, water_y, water_z) != BLOCK_AIR ||
        world_get_block(&world, water_x + 1, water_floor_y, water_z) != BLOCK_AIR)
        return check_failed("unsupported water flow did not evaporate");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-water-cleanup mesh rebuild failed");

    const int lava_flow_x = 8;
    const int lava_flow_y = 24;
    const int lava_flow_z = 8;
    const int lava_flow_floor_y = lava_flow_y - 1;
    for (int y = lava_flow_y - 3; y <= lava_flow_y + 1; y++) {
        for (int z = lava_flow_z - 1; z <= lava_flow_z + 1; z++) {
            for (int x = lava_flow_x - 1; x <= lava_flow_x + 1; x++) {
                if (!world_set_block(&world, x, y, z, BLOCK_AIR))
                    return check_failed("lava-flow test volume clear failed");
            }
        }
    }
    for (int z = lava_flow_z - 1; z <= lava_flow_z + 1; z++) {
        for (int x = lava_flow_x - 1; x <= lava_flow_x + 1; x++) {
            if (!world_set_block(&world, x, lava_flow_floor_y, z, BLOCK_STONE))
                return check_failed("lava-flow floor build failed");
        }
    }
    if (!world_set_block(&world, lava_flow_x, lava_flow_y, lava_flow_z, BLOCK_LAVA))
        return check_failed("lava source placement failed");
    world_water_tick(&world);
    if (world_get_block(&world, lava_flow_x + 1, lava_flow_y, lava_flow_z) != BLOCK_LAVA_FLOW)
        return check_failed("lava did not spread like water");
    if (!world_set_block(&world, lava_flow_x, lava_flow_y, lava_flow_z, BLOCK_AIR))
        return check_failed("lava source removal failed");
    for (int i = 0; i < 8; i++)
        world_water_tick(&world);
    if (world_get_block(&world, lava_flow_x + 1, lava_flow_y, lava_flow_z) != BLOCK_AIR)
        return check_failed("unsupported lava flow did not evaporate");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-lava-flow-cleanup mesh rebuild failed");

    const int gravity_x = 12;
    const int gravity_z = 8;
    for (int y = 20; y <= 26; y++) {
        if (!world_set_block(&world, gravity_x, y, gravity_z, BLOCK_AIR) ||
            !world_set_block(&world, gravity_x + 1, y, gravity_z, BLOCK_AIR))
            return check_failed("gravity test volume clear failed");
    }
    if (!world_set_block(&world, gravity_x, 20, gravity_z, BLOCK_STONE) ||
        !world_set_block(&world, gravity_x + 1, 20, gravity_z, BLOCK_STONE) ||
        !world_set_block(&world, gravity_x, 24, gravity_z, BLOCK_SAND) ||
        !world_set_block(&world, gravity_x + 1, 22, gravity_z, BLOCK_GRAVEL))
        return check_failed("gravity test fixture build failed");
    world_water_tick(&world);
    if (world_get_block(&world, gravity_x, 24, gravity_z) != BLOCK_AIR ||
        world_get_block(&world, gravity_x + 1, 22, gravity_z) != BLOCK_AIR)
        return check_failed("gravity blocks did not leave the integer grid");
    if (world_falling_block_count(&world) != 2)
        return check_failed("gravity blocks did not become falling entities");
    world_update_falling_blocks(&world, 0.75f);
    if (world_get_block(&world, gravity_x + 1, 21, gravity_z) != BLOCK_GRAVEL)
        return check_failed("gravel did not settle after smooth fall");
    if (world_get_block(&world, gravity_x, 21, gravity_z) != BLOCK_AIR ||
        world_falling_block_count(&world) != 1)
        return check_failed("sand settled too early during smooth fall");
    world_update_falling_blocks(&world, 0.40f);
    if (world_get_block(&world, gravity_x, 21, gravity_z) != BLOCK_SAND ||
        world_get_block(&world, gravity_x + 1, 21, gravity_z) != BLOCK_GRAVEL)
        return check_failed("falling blocks did not settle above solid ground");
    if (world_falling_block_count(&world) != 0)
        return check_failed("falling entities did not clear after settling");
    if (!world_set_block(&world, gravity_x, 21, gravity_z, BLOCK_AIR) ||
        !world_set_block(&world, gravity_x + 1, 21, gravity_z, BLOCK_AIR) ||
        !world_set_block(&world, gravity_x, 20, gravity_z, BLOCK_AIR) ||
        !world_set_block(&world, gravity_x + 1, 20, gravity_z, BLOCK_AIR))
        return check_failed("gravity cleanup failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-gravity-cleanup mesh rebuild failed");

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

    const int seam_x = WORLD_CHUNK_SIZE + 8;
    const int seam_y = 10;
    const int seam_z = WORLD_CHUNK_SIZE - 1;
    if (!world_set_block(&world, seam_x, seam_y, seam_z, BLOCK_WOOD))
        return check_failed("seam self block edit failed");
    if (!world_set_block(&world, seam_x, seam_y, seam_z + 1, BLOCK_WOOD))
        return check_failed("seam neighbor block edit failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("seam sync mesh rebuild failed");

    const Chunk *seam_chunk = world_get_chunk(&world, 1, 0);
    if (!seam_chunk)
        return check_failed("seam chunk missing");
    if (find_chunk_face(seam_chunk, 8, seam_y, WORLD_CHUNK_SIZE - 1,
                        FACE_BACK, BLOCK_WOOD))
        return check_failed("sync mesh exposed occluded z-boundary face");

    ChunkMeshWorkerScratch *mesh_scratch = chunk_mesh_worker_scratch_create();
    if (!mesh_scratch)
        return check_failed("mesh worker scratch alloc failed");
    if (!world_run_mesh_job(&world, mesh_scratch,
                            1, 0, seam_chunk->generation))
        return check_failed("mesh worker seam rebuild failed");
    chunk_mesh_worker_scratch_destroy(mesh_scratch);
    seam_chunk = world_get_chunk(&world, 1, 0);
    if (!seam_chunk ||
        find_chunk_face(seam_chunk, 8, seam_y, WORLD_CHUNK_SIZE - 1,
                        FACE_BACK, BLOCK_WOOD))
        return check_failed("mesh worker exposed occluded z-boundary face");

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

    if (!world_init_infinite_procedural(&capped_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f,
                                        NULL))
        return check_failed("capped world init failed");
    world_set_stream_chunks_per_frame(&capped_world, 2);
    if (!world_stream_around(&capped_world,
                             (float)WORLD_CHUNK_SIZE + 1.0f,
                             1.0f))
        return check_failed("capped one-chunk stream failed");
    if (capped_world.chunks_generated_last_stream != 2)
        return check_failed("capped stream did not limit new chunks");
    if (capped_world.chunk_count >= expected_chunks)
        return check_failed("capped stream unexpectedly filled whole window");
    for (int guard = 0;
         guard < expected_chunks && capped_world.chunk_count < expected_chunks;
         guard++) {
        if (!world_stream_around(&capped_world,
                                 (float)WORLD_CHUNK_SIZE + 1.0f,
                                 1.0f))
            return check_failed("capped follow-up stream failed");
    }
    if (capped_world.chunk_count != expected_chunks)
        return check_failed("capped stream did not eventually fill window");
    world_set_near_chunk_radius(&capped_world, 0);
    if (world_near_chunk_radius(&capped_world) != 0)
        return check_failed("near chunk radius setter failed");
    if (!capped_world.meshes_dirty)
        return check_failed("near radius change did not dirty meshes");
    world_free(&capped_world);
    cleanup_temp_world_dir(world_dir);

    /* Async-gen smoke test: simulate the gen worker without actually
     * spawning a thread. Flip async_chunk_gen_enabled, stream past the
     * initial fill so new slots come in as LOADING, then run the
     * offline + finalize halves by hand and confirm the result matches
     * what the synchronous path would have produced. */
    VoxelWorld async_world;
    char async_world_dir_template[] = "/tmp/voxel-world-async-XXXXXX";
    char *async_world_dir = mkdtemp(async_world_dir_template);
    if (!async_world_dir)
        return check_failed("async mkdtemp failed");
    world_init(&async_world);
    if (!world_init_infinite_procedural(&async_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f,
                                        async_world_dir))
        return check_failed("async world init failed");

    /* Initial stream is sync; turn on async only for follow-up streams. */
    async_world.async_chunk_gen_enabled = true;
    if (!world_stream_around(&async_world,
                             (float)WORLD_CHUNK_SIZE + 1.0f,
                             1.0f))
        return check_failed("async follow-up stream failed");
    if (async_world.chunks_generated_last_stream <= 0)
        return check_failed("async stream did not allocate new slots");

    int loading_count = 0;
    int loading_x = 0, loading_z = 0;
    uint32_t loading_gen = 0;
    for (int i = 0; i < async_world.chunk_count; i++) {
        const Chunk *c = &async_world.chunks[i];
        if ((c->flags & CHUNK_FLAG_LOADING) &&
            !(c->flags & CHUNK_FLAG_LOADED)) {
            loading_count++;
            loading_x = c->chunk_x;
            loading_z = c->chunk_z;
            loading_gen = c->generation;
        }
    }
    if (loading_count == 0)
        return check_failed("async stream did not produce LOADING slots");
    /* LOADING chunks must be transparent to world_get_block. */
    if (world_get_block(&async_world,
                        loading_x * WORLD_CHUNK_SIZE,
                        0,
                        loading_z * WORLD_CHUNK_SIZE) != BLOCK_AIR)
        return check_failed("LOADING chunk leaked block data to world_get_block");

    ChunkGenResult result;
    if (!world_async_chunk_gen_offline(&async_world,
                                       loading_x, loading_z, &result))
        return check_failed("offline gen failed");
    if (!world_finalize_async_chunk_load(&async_world,
                                         loading_x, loading_z,
                                         loading_gen, &result))
        return check_failed("finalize did not integrate async chunk");
    const Chunk *finalized = world_get_chunk(&async_world, loading_x, loading_z);
    if (!finalized || !(finalized->flags & CHUNK_FLAG_LOADED) ||
        (finalized->flags & CHUNK_FLAG_LOADING))
        return check_failed("finalize did not flip LOADING -> LOADED");
    if (world_get_block(&async_world,
                        loading_x * WORLD_CHUNK_SIZE,
                        0,
                        loading_z * WORLD_CHUNK_SIZE) == BLOCK_AIR)
        return check_failed("finalized chunk has no block data");

    /* Stale generation must drop. */
    if (world_finalize_async_chunk_load(&async_world,
                                        loading_x, loading_z,
                                        loading_gen + 999u, &result))
        return check_failed("finalize accepted stale generation");

    world_free(&async_world);
    cleanup_temp_world_dir(async_world_dir);

    printf("world_chunk_test: ok\n");
    return 0;
}
