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

static int test_floor_div(int value, int divisor)
{
    int q = value / divisor;
    int r = value % divisor;

    if (r < 0)
        q--;
    return q;
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

static bool test_set_wire(VoxelWorld *world, int wx, int wy, int wz)
{
    return world_set_block(world, wx, wy, wz, BLOCK_REDSTONE_WIRE_UNCONNECTED);
}

static bool test_wire_line_x(VoxelWorld *world,
                             int x0,
                             int x1,
                             int wy,
                             int wz)
{
    int step = x0 <= x1 ? 1 : -1;

    for (int wx = x0;; wx += step) {
        if (!test_set_wire(world, wx, wy, wz))
            return false;
        if (wx == x1)
            break;
    }

    return true;
}

static bool test_wire_line_z(VoxelWorld *world,
                             int wx,
                             int wy,
                             int z0,
                             int z1)
{
    int step = z0 <= z1 ? 1 : -1;

    for (int wz = z0;; wz += step) {
        if (!test_set_wire(world, wx, wy, wz))
            return false;
        if (wz == z1)
            break;
    }

    return true;
}

static void test_tick_redstone(VoxelWorld *world, int ticks)
{
    for (int tick = 0; tick < ticks; tick++)
        world_update_redstone(world, 0.1f);
}

static bool test_place_wide_comparator_latch(VoxelWorld *world,
                                             int wx,
                                             int wy,
                                             int wz)
{
    static const int feedback[][2] = {
        { 1, 0 }, { 2, 0 }, { 3, 0 }, { 3, 1 }, { 3, 2 }, { 3, 3 },
        { 2, 3 }, { 1, 3 }, { 0, 3 }, { -1, 3 }, { -2, 3 },
        { -2, 2 }, { -2, 1 }, { -2, 0 }, { -1, 0 },
    };

    if (!world_set_block(world, wx, wy, wz, BLOCK_COMPARATOR_EAST_OFF))
        return false;
    for (size_t i = 0; i < sizeof(feedback) / sizeof(feedback[0]); i++) {
        if (!test_set_wire(world,
                           wx + feedback[i][0],
                           wy,
                           wz + feedback[i][1]))
            return false;
    }

    return true;
}

static bool test_place_t_flip_flop(VoxelWorld *world,
                                   int wx,
                                   int wy,
                                   int wz,
                                   bool button_input)
{
    if (!world_set_block(world, wx, wy, wz,
                         button_input ? BLOCK_BUTTON :
                                        BLOCK_REPEATER_EAST_OFF) ||
        !test_set_wire(world, wx + 1, wy, wz) ||
        !test_set_wire(world, wx + 2, wy, wz) ||
        !world_set_block(world, wx + 3, wy, wz,
                         BLOCK_COMPARATOR_EAST_OFF) ||
        !world_set_block(world, wx + 3, wy, wz - 1,
                         BLOCK_REPEATER_SOUTH_OFF) ||
        !test_set_wire(world, wx + 1, wy, wz - 1) ||
        !test_set_wire(world, wx + 1, wy, wz - 2) ||
        !test_set_wire(world, wx + 2, wy, wz - 2) ||
        !test_set_wire(world, wx + 3, wy, wz - 2))
        return false;

    if (!test_wire_line_x(world, wx + 4, wx + 10, wy, wz) ||
        !world_set_block(world, wx + 11, wy, wz,
                         BLOCK_REPEATER_EAST_OFF) ||
        !test_wire_line_x(world, wx + 12, wx + 16, wy, wz) ||
        !test_wire_line_z(world, wx + 16, wy, wz - 1, wz - 4) ||
        !test_wire_line_x(world, wx + 17, wx + 20, wy, wz - 4))
        return false;

    if (!world_set_block(world, wx + 17, wy, wz,
                         BLOCK_COMPARATOR_EAST_OFF) ||
        !world_set_block(world, wx + 20, wy, wz - 3,
                         BLOCK_COMPARATOR_SOUTH_OFF) ||
        !test_place_wide_comparator_latch(world, wx + 20, wy, wz))
        return false;

    if (!world_set_block(world, wx + 17, wy, wz + 1,
                         BLOCK_REPEATER_OFF) ||
        !test_set_wire(world, wx + 17, wy, wz + 2) ||
        !test_set_wire(world, wx + 18, wy, wz + 2) ||
        !test_set_wire(world, wx + 20, wy, wz - 2) ||
        !test_set_wire(world, wx + 20, wy, wz - 1))
        return false;

    if (!world_set_block(world, wx + 24, wy, wz, BLOCK_STONE) ||
        !world_set_block(world, wx + 25, wy, wz,
                         BLOCK_REDSTONE_TORCH_ON) ||
        !test_wire_line_z(world, wx + 25, wy, wz - 1, wz - 3) ||
        !test_wire_line_x(world, wx + 22, wx + 24, wy, wz - 3) ||
        !world_set_block(world, wx + 21, wy, wz - 3,
                         BLOCK_REPEATER_WEST_OFF))
        return false;

    return true;
}

static bool test_reset_t_flip_flop(VoxelWorld *world,
                                   int wx,
                                   int wy,
                                   int wz)
{
    if (world_get_block(world, wx + 21, wy, wz - 1) != BLOCK_BUTTON &&
        !world_set_block(world, wx + 21, wy, wz - 1, BLOCK_BUTTON))
        return false;
    if (!world_press_button(world, wx + 21, wy, wz - 1))
        return false;
    test_tick_redstone(world, 20);
    return true;
}

static bool test_t_flip_flop_q(const VoxelWorld *world,
                               int wx,
                               int wy,
                               int wz)
{
    return world_get_block(world, wx + 20, wy, wz) ==
           BLOCK_COMPARATOR_EAST_ON;
}

static int test_ripple_counter_value(const VoxelWorld *world,
                                     int wx,
                                     int wy,
                                     int wz,
                                     int bit_stride,
                                     int bit_count)
{
    int value = 0;

    for (int bit = 0; bit < bit_count; bit++) {
        if (test_t_flip_flop_q(world,
                               wx + bit * bit_stride,
                               wy,
                               wz))
            value |= 1 << bit;
    }

    return value;
}

enum {
    TEST_SEG_A = 1u << 0,
    TEST_SEG_B = 1u << 1,
    TEST_SEG_C = 1u << 2,
    TEST_SEG_D = 1u << 3,
    TEST_SEG_E = 1u << 4,
    TEST_SEG_F = 1u << 5,
    TEST_SEG_G = 1u << 6,
};

static uint8_t test_seven_segment_pattern(int value)
{
    static const uint8_t patterns[8] = {
        TEST_SEG_A | TEST_SEG_B | TEST_SEG_C | TEST_SEG_D |
            TEST_SEG_E | TEST_SEG_F,
        TEST_SEG_B | TEST_SEG_C,
        TEST_SEG_A | TEST_SEG_B | TEST_SEG_D | TEST_SEG_E | TEST_SEG_G,
        TEST_SEG_A | TEST_SEG_B | TEST_SEG_C | TEST_SEG_D | TEST_SEG_G,
        TEST_SEG_B | TEST_SEG_C | TEST_SEG_F | TEST_SEG_G,
        TEST_SEG_A | TEST_SEG_C | TEST_SEG_D | TEST_SEG_F | TEST_SEG_G,
        TEST_SEG_A | TEST_SEG_C | TEST_SEG_D | TEST_SEG_E |
            TEST_SEG_F | TEST_SEG_G,
        TEST_SEG_A | TEST_SEG_B | TEST_SEG_C,
    };

    return patterns[value & 7];
}

static int test_decoder_q_bus_x(int wx, int bit)
{
    return wx + bit * 5;
}

static int test_decoder_nq_bus_x(int wx, int bit)
{
    return wx + bit * 5 + 2;
}

static int test_decoder_row_z(int wz, int value)
{
    return wz + value * 3;
}

static int test_decoder_support_x(int wx)
{
    return wx + 15;
}

static int test_decoder_segment_bus_x(int wx, int segment)
{
    return test_decoder_support_x(wx) + 4 + segment * 2;
}

static int test_decoder_segment_lamp_z(int wz)
{
    return test_decoder_row_z(wz, 7) + 4;
}

static bool test_place_seven_segment_decoder(VoxelWorld *world,
                                             int wx,
                                             int wy,
                                             int wz)
{
    const int support_x = test_decoder_support_x(wx);
    const int row_start_x = wx - 1;
    const int row_end_x = support_x - 1;
    const int source_z = wz - 2;
    const int bus_z0 = wz - 1;
    const int bus_z1 = test_decoder_row_z(wz, 7) + 1;
    const int segment_bus_z1 = test_decoder_segment_lamp_z(wz) - 1;

    for (int bit = 0; bit < 3; bit++) {
        int qx = test_decoder_q_bus_x(wx, bit);
        int nqx = test_decoder_nq_bus_x(wx, bit);

        if (!test_wire_line_z(world, qx, wy + 2, bus_z0, bus_z1) ||
            !test_wire_line_z(world, nqx, wy + 2, bus_z0, bus_z1))
            return false;
        for (int rz = bus_z0 + 3; rz < bus_z1; rz += 9) {
            if (!world_set_block(world, qx, wy + 2, rz,
                                 BLOCK_REPEATER_SOUTH_OFF) ||
                !world_set_block(world, nqx, wy + 2, rz,
                                 BLOCK_REPEATER_SOUTH_OFF))
                return false;
        }
        if (!world_set_block(world, qx, wy + 2, source_z, BLOCK_AIR) ||
            !world_set_block(world, nqx, wy + 2, source_z, BLOCK_AIR))
            return false;
    }

    for (int value = 0; value < 8; value++) {
        int row_z = test_decoder_row_z(wz, value);

        if (!test_wire_line_x(world, row_start_x, row_end_x, wy, row_z) ||
            !world_set_block(world, wx + 4, wy, row_z,
                             BLOCK_REPEATER_EAST_OFF) ||
            !world_set_block(world, wx + 9, wy, row_z,
                             BLOCK_REPEATER_EAST_OFF) ||
            !world_set_block(world, support_x, wy, row_z, BLOCK_STONE) ||
            !world_set_block(world, support_x + 1, wy, row_z,
                             BLOCK_REDSTONE_TORCH_ON) ||
            !test_wire_line_x(world, support_x + 2,
                              test_decoder_segment_bus_x(wx, 6) + 1,
                              wy, row_z) ||
            !world_set_block(world, support_x + 5, wy, row_z,
                             BLOCK_REPEATER_EAST_OFF) ||
            !world_set_block(world, support_x + 11, wy, row_z,
                             BLOCK_REPEATER_EAST_OFF))
            return false;

        for (int bit = 0; bit < 3; bit++) {
            bool expected_bit = (value & (1 << bit)) != 0;
            int bus_x = expected_bit ? test_decoder_nq_bus_x(wx, bit) :
                                       test_decoder_q_bus_x(wx, bit);

            if (!world_set_block(world, bus_x, wy + 1, row_z, BLOCK_STONE))
                return false;
        }
    }

    for (int segment = 0; segment < 7; segment++) {
        int bus_x = test_decoder_segment_bus_x(wx, segment);

        if (!test_wire_line_z(world, bus_x, wy - 2, wz, segment_bus_z1) ||
            !world_set_block(world, bus_x, wy - 2,
                             test_decoder_segment_lamp_z(wz),
                             BLOCK_LAMP_OFF))
            return false;
        for (int rz = wz + 2; rz < segment_bus_z1; rz += 9) {
            if (!world_set_block(world, bus_x, wy - 2, rz,
                                 BLOCK_REPEATER_SOUTH_OFF))
                return false;
        }
    }

    for (int value = 0; value < 8; value++) {
        int row_z = test_decoder_row_z(wz, value);
        uint8_t pattern = test_seven_segment_pattern(value);

        for (int segment = 0; segment < 7; segment++) {
            if (!(pattern & (1u << segment)))
                continue;

            int bus_x = test_decoder_segment_bus_x(wx, segment);
            if (!world_set_block(world, bus_x, wy - 1,
                                 row_z, BLOCK_STONE))
                return false;
        }
    }

    return true;
}

static bool test_drive_seven_segment_decoder(VoxelWorld *world,
                                             int wx,
                                             int wy,
                                             int wz,
                                             int value)
{
    const int source_z = wz - 2;

    for (int bit = 0; bit < 3; bit++) {
        bool powered = (value & (1 << bit)) != 0;
        int qx = test_decoder_q_bus_x(wx, bit);
        int nqx = test_decoder_nq_bus_x(wx, bit);

        if (!world_set_block(world, qx, wy + 2, source_z,
                             powered ? BLOCK_REDSTONE_BLOCK : BLOCK_AIR) ||
            !world_set_block(world, nqx, wy + 2, source_z,
                             powered ? BLOCK_AIR : BLOCK_REDSTONE_BLOCK))
            return false;
    }

    test_tick_redstone(world, 20);
    return true;
}

static bool test_seven_segment_decoder_matches(const VoxelWorld *world,
                                               int wx,
                                               int wy,
                                               int wz,
                                               int value)
{
    uint8_t pattern = test_seven_segment_pattern(value);
    int lamp_z = test_decoder_segment_lamp_z(wz);

    for (int segment = 0; segment < 7; segment++) {
        int bus_x = test_decoder_segment_bus_x(wx, segment);
        BlockID lamp = world_get_block(world, bus_x, wy - 2, lamp_z);
        bool expected = (pattern & (1u << segment)) != 0;

        if ((lamp == BLOCK_LAMP) != expected)
            return false;
    }

    return true;
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
    int sugar_cane;
    int lava_blocks;
    int ore_blocks;
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
           id == BLOCK_CACTUS ||
           id == BLOCK_SUGAR_CANE;
}

static bool block_is_ore(BlockID id)
{
    return id == BLOCK_COAL_ORE ||
           id == BLOCK_IRON_ORE ||
           id == BLOCK_GOLD_ORE ||
           id == BLOCK_DIAMOND_ORE ||
           id == BLOCK_REDSTONE_ORE;
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
                    if (id == BLOCK_SUGAR_CANE)
                        sample.sugar_cane++;
                    if (id == BLOCK_LAVA || id == BLOCK_LAVA_FLOW)
                        sample.lava_blocks++;
                    if (block_is_ore(id))
                        sample.ore_blocks++;
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

static bool loaded_world_sugar_cane_is_shoreline(const VoxelWorld *world)
{
    bool found_base = false;
    static const int dirs[][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
    };

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        for (int y = 0; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    int wx;
                    int wz;
                    bool touches_water = false;

                    if (chunk->blocks[y][z][x] != BLOCK_SUGAR_CANE)
                        continue;
                    if (y > 0 &&
                        chunk->blocks[y - 1][z][x] == BLOCK_SUGAR_CANE)
                        continue;
                    if (y <= 0 || chunk->blocks[y - 1][z][x] != BLOCK_SAND)
                        return false;

                    wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x;
                    wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z;
                    for (size_t dir = 0; dir < sizeof(dirs) / sizeof(dirs[0]);
                         dir++) {
                        BlockID neighbor = world_get_block(world,
                                                           wx + dirs[dir][0],
                                                           y - 1,
                                                           wz + dirs[dir][1]);

                        if (neighbor == BLOCK_WATER ||
                            neighbor == BLOCK_WATER_FLOW) {
                            touches_water = true;
                            break;
                        }
                    }
                    if (!touches_water)
                        return false;
                    found_base = true;
                }
            }
        }
    }

    return found_base;
}

static bool loaded_world_has_uncontained_lava(const VoxelWorld *world)
{
    static const int dx[4] = { -1, 1, 0, 0 };
    static const int dz[4] = { 0, 0, -1, 1 };

    for (int i = 0; i < world->chunk_count; i++) {
        const Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;

        for (int y = 1; y < WORLD_CHUNK_HEIGHT; y++) {
            for (int z = 0; z < WORLD_CHUNK_SIZE; z++) {
                for (int x = 0; x < WORLD_CHUNK_SIZE; x++) {
                    int wx;
                    int wz;

                    if (chunk->blocks[y][z][x] != BLOCK_LAVA)
                        continue;

                    wx = chunk->chunk_x * WORLD_CHUNK_SIZE + x;
                    wz = chunk->chunk_z * WORLD_CHUNK_SIZE + z;
                    if (block_is_passable(world_get_block(world,
                                                          wx, y - 1, wz)))
                        return true;

                    for (int d = 0; d < 4; d++) {
                        int nx = wx + dx[d];
                        int nz = wz + dz[d];
                        int ncx = test_floor_div(nx, WORLD_CHUNK_SIZE);
                        int ncz = test_floor_div(nz, WORLD_CHUNK_SIZE);
                        BlockID neighbor;

                        if (!world_get_chunk(world, ncx, ncz))
                            continue;

                        neighbor = world_get_block(world, nx, y, nz);
                        if (neighbor != BLOCK_LAVA &&
                            block_is_passable(neighbor))
                            return true;
                    }
                }
            }
        }
    }

    return false;
}

int main(void)
{
    /* Each test expects one world_rebuild_dirty_meshes() to drain every dirty
     * chunk; override the runtime default (one chunk per pass). */
    if (setenv("VOXEL_MESH_REBUILDS_PER_FRAME", "0", 1) != 0)
        return check_failed("setenv VOXEL_MESH_REBUILDS_PER_FRAME failed");

    static VoxelWorld world;
    static VoxelWorld reloaded_world;
    static VoxelWorld capped_world;
    static VoxelWorld biome_world;
    static VoxelWorld lava_world;
    int expected_chunks = expected_loaded_chunks(TEST_RENDER_DISTANCE);
    char world_dir_template[] = "/tmp/voxel-world-test-XXXXXX";
    char *world_dir;

    init_block_types();
    world_init(&world);
    world_init(&reloaded_world);
    world_init(&capped_world);
    world_init(&biome_world);
    world_init(&lava_world);
    world_dir = mkdtemp(world_dir_template);
    if (!world_dir)
        return check_failed("mkdtemp failed");

    if (!world_init_infinite_procedural(&world,
                                        TEST_WORLD_SEED,
                                        12,
                                        true,
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
    if (initial_sample.ore_blocks <= 0)
        return check_failed("ore worldgen did not place any ore blocks");
    if (initial_sample.sugar_cane <= 0 ||
        !loaded_world_sugar_cane_is_shoreline(&world))
        return check_failed("sugar cane did not generate on sand next to water");
    bool found_desert_biome = false;
    bool found_ocean_biome = false;
    for (int z = -128; z <= 128 && (!found_desert_biome ||
                                    !found_ocean_biome); z += 16) {
        for (int x = -128; x <= 128; x += 16) {
            WorldBiome biome = world_biome_at(&world, x, z);

            if (biome == WORLD_BIOME_DESERT) {
                found_desert_biome = true;
            } else if (biome == WORLD_BIOME_OCEAN) {
                found_ocean_biome = true;
            }
        }
    }
    if (!found_desert_biome ||
        strcmp(world_biome_name(WORLD_BIOME_DESERT), "desert") != 0)
        return check_failed("desert biome was not reachable by worldgen");
    if (!found_ocean_biome ||
        strcmp(world_biome_name(WORLD_BIOME_OCEAN), "ocean") != 0)
        return check_failed("ocean biome was not reachable by worldgen");

    if (!world_init_infinite_procedural(&lava_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        true,
                                        5,
                                        0.0f,
                                        0.0f,
                                        NULL))
        return check_failed("desert lava pool sample load failed");
    WorldgenSample lava_sample = sample_worldgen(&lava_world);
    if (lava_sample.lava_blocks <= 0 || !lava_world.has_light_emitters)
        return check_failed("desert lava pools did not generate lit lava");
    if (loaded_world_has_uncontained_lava(&lava_world))
        return check_failed("desert lava pools generated spill-prone lava");
    world_free(&lava_world);

    if (!world_init_infinite_procedural(&biome_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        true,
                                        3,
                                        -192.0f,
                                        -256.0f,
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
    const int lamp_power_y = lamp_y - 1;
    if (!world_set_block(&world, gap_x, lamp_y, lamp_z, BLOCK_AIR))
        return check_failed("lamp air gap clear failed");
    if (!world_set_block(&world, lamp_x, lamp_power_y, lamp_z,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("lamp power placement failed");
    if (!world_set_block(&world, lamp_x, lamp_y, lamp_z, BLOCK_LAMP_OFF))
        return check_failed("lamp placement failed");
    if (world_get_block(&world, lamp_x, lamp_y, lamp_z) != BLOCK_LAMP)
        return check_failed("powered lamp did not turn on");
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
    if (!world_set_block(&world, lamp_x, lamp_power_y, lamp_z, BLOCK_AIR))
        return check_failed("lamp power cleanup failed");
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

    if (block_face_texture_id(BLOCK_FURNACE, FACE_TOP) != TEX_TILE_FURNACE_TOP ||
        block_face_texture_id(BLOCK_FURNACE, FACE_FRONT) != TEX_TILE_FURNACE_FRONT ||
        block_render_model(BLOCK_FURNACE) != BLOCK_RENDER_CUBE ||
        block_render_model(BLOCK_TORCH) != BLOCK_RENDER_TORCH ||
        block_emission_level(BLOCK_TORCH) != 14 ||
        !block_is_self_lit(BLOCK_TORCH) ||
        !block_is_alpha_keyed(BLOCK_TORCH) ||
        !block_is_passable(BLOCK_TORCH))
        return check_failed("furnace/torch metadata missing");
    if (block_render_model(BLOCK_REDSTONE_WIRE_ON) != BLOCK_RENDER_FLAT ||
        !block_is_alpha_keyed(BLOCK_REDSTONE_WIRE_ON) ||
        !block_is_passable(BLOCK_REDSTONE_WIRE_ON) ||
        block_emission_level(BLOCK_REDSTONE_WIRE_ON) != 5 ||
        block_render_model(BLOCK_REDSTONE_TORCH_ON) != BLOCK_RENDER_TORCH ||
        block_emission_level(BLOCK_REDSTONE_TORCH_ON) != 7 ||
        block_emission_level(BLOCK_REDSTONE_TORCH_OFF) != 0 ||
        block_render_model(BLOCK_REPEATER_ON) != BLOCK_RENDER_FLAT ||
        block_emission_level(BLOCK_REPEATER_ON) != 5 ||
        block_render_model(BLOCK_COMPARATOR_ON) != BLOCK_RENDER_FLAT ||
        block_face_texture_id(BLOCK_COMPARATOR_OFF, FACE_FRONT) !=
            TEX_TILE_COMPARATOR_OFF ||
        block_emission_level(BLOCK_COMPARATOR_ON) != 5 ||
        block_face_texture_id(BLOCK_LAMP_OFF, FACE_FRONT) != TEX_TILE_LAMP_OFF ||
        block_emission_level(BLOCK_LAMP_OFF) != 0 ||
        block_render_model(BLOCK_BUTTON) != BLOCK_RENDER_FLAT ||
        block_render_model(BLOCK_BUTTON_PRESSED) != BLOCK_RENDER_FLAT ||
        block_render_model(BLOCK_WOOD_PRESSURE_PLATE) != BLOCK_RENDER_FLAT ||
        block_render_model(BLOCK_WOOD_PRESSURE_PLATE_PRESSED) !=
            BLOCK_RENDER_FLAT ||
        block_face_texture_id(BLOCK_WOOD_PRESSURE_PLATE, FACE_FRONT) !=
            TEX_TILE_WOOD_PLANK ||
        block_render_model(BLOCK_STONE_PRESSURE_PLATE) != BLOCK_RENDER_FLAT ||
        block_render_model(BLOCK_STONE_PRESSURE_PLATE_PRESSED) !=
            BLOCK_RENDER_FLAT ||
        block_face_texture_id(BLOCK_STONE_PRESSURE_PLATE, FACE_FRONT) !=
            TEX_TILE_STONE ||
        block_render_model(BLOCK_LEVER_OFF) != BLOCK_RENDER_FLAT ||
        block_face_texture_id(BLOCK_LEVER_OFF, FACE_FRONT) != TEX_TILE_LEVER_OFF ||
        block_face_texture_id(BLOCK_LEVER_ON, FACE_FRONT) != TEX_TILE_LEVER_ON ||
        block_emission_level(BLOCK_LEVER_ON) != 5 ||
        block_lever_powered(BLOCK_LEVER_OFF) ||
        !block_lever_powered(BLOCK_LEVER_ON) ||
        !block_is_pressure_plate(BLOCK_WOOD_PRESSURE_PLATE) ||
        !block_is_wood_pressure_plate(BLOCK_WOOD_PRESSURE_PLATE_PRESSED) ||
        !block_is_stone_pressure_plate(BLOCK_STONE_PRESSURE_PLATE) ||
        block_pressure_plate_powered(BLOCK_WOOD_PRESSURE_PLATE) ||
        !block_pressure_plate_powered(BLOCK_WOOD_PRESSURE_PLATE_PRESSED) ||
        !block_pressure_plate_powered(BLOCK_STONE_PRESSURE_PLATE_PRESSED) ||
        block_pressure_plate_unpressed(BLOCK_STONE_PRESSURE_PLATE_PRESSED) !=
            BLOCK_STONE_PRESSURE_PLATE)
        return check_failed("redstone metadata missing");
    if (!block_is_repeater(BLOCK_REPEATER_ON) ||
        !block_is_comparator(BLOCK_COMPARATOR_ON) ||
        block_repeater_make(BLOCK_DOOR_FACING_EAST, true) !=
            BLOCK_REPEATER_EAST_ON ||
        block_comparator_make(BLOCK_DOOR_FACING_SOUTH, false) !=
            BLOCK_COMPARATOR_SOUTH_OFF ||
        block_redstone_facing(BLOCK_REPEATER_WEST_OFF) !=
            BLOCK_DOOR_FACING_WEST ||
        !block_redstone_directional_powered(BLOCK_COMPARATOR_EAST_ON) ||
        block_redstone_directional_powered(BLOCK_COMPARATOR_EAST_OFF))
        return check_failed("redstone directional metadata missing");
    if (!block_is_door(BLOCK_DOOR) ||
        !block_is_door(block_door_make(BLOCK_DOOR_FACING_EAST, true, true)) ||
        block_render_model(BLOCK_DOOR) != BLOCK_RENDER_DOOR ||
        block_door_toggle(BLOCK_DOOR) !=
            block_door_make(BLOCK_DOOR_FACING_NORTH, true, false) ||
        !block_is_door_upper(block_door_make(BLOCK_DOOR_FACING_WEST, false, true)) ||
        !block_is_door_open(block_door_make(BLOCK_DOOR_FACING_SOUTH, true, false)) ||
        block_is_passable(BLOCK_DOOR) ||
        !block_is_passable(block_door_make(BLOCK_DOOR_FACING_NORTH, true, false)) ||
        !block_is_alpha_keyed(BLOCK_DOOR))
        return check_failed("door metadata missing");

    if (block_render_model(BLOCK_RED_FLOWER) != BLOCK_RENDER_CROSS ||
        !block_is_alpha_keyed(BLOCK_RED_FLOWER) ||
        !block_is_passable(BLOCK_RED_FLOWER) ||
        block_render_model(BLOCK_RED_MUSHROOM) != BLOCK_RENDER_CROSS ||
        !block_is_alpha_keyed(BLOCK_RED_MUSHROOM) ||
        !block_is_passable(BLOCK_RED_MUSHROOM) ||
        block_render_model(BLOCK_SUGAR_CANE) != BLOCK_RENDER_CROSS ||
        !block_is_alpha_keyed(BLOCK_SUGAR_CANE) ||
        !block_is_passable(BLOCK_SUGAR_CANE) ||
        block_render_model(BLOCK_CACTUS) != BLOCK_RENDER_CUBE ||
        block_is_alpha_keyed(BLOCK_CACTUS) ||
        block_is_passable(BLOCK_CACTUS))
        return check_failed("plant metadata missing");
    const int wire_x = 8;
    const int wire_y = 26;
    const int wire_z = 3;
    const int wire_neighbor_x = wire_x + 1;
    if (!world_set_block(&world, wire_x, wire_y, wire_z,
                         BLOCK_REDSTONE_WIRE_UNCONNECTED))
        return check_failed("redstone wire placement failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-redstone mesh rebuild failed");
    const Chunk *wire_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *wire_flat = find_chunk_face(wire_chunk,
                                                 wire_x, wire_y, wire_z,
                                                 (BlockFace)CHUNK_FACE_FLAT,
                                                 BLOCK_REDSTONE_WIRE_UNCONNECTED);
    if (!wire_flat)
        return check_failed("redstone unconnected flat mesh missing");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_STONE))
        return check_failed("redstone neighbor placement failed");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("redstone wire did not connect as off line");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-redstone-off mesh rebuild failed");
    wire_chunk = world_get_chunk(&world, 0, 0);
    wire_flat = find_chunk_face(wire_chunk,
                                wire_x, wire_y, wire_z,
                                (BlockFace)CHUNK_FACE_FLAT,
                                BLOCK_REDSTONE_WIRE_OFF);
    if (!wire_flat)
        return check_failed("redstone connected-off flat mesh missing");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("redstone source placement failed");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_ON)
        return check_failed("redstone wire did not power on");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-redstone-on mesh rebuild failed");
    wire_chunk = world_get_chunk(&world, 0, 0);
    wire_flat = find_chunk_face(wire_chunk,
                                wire_x, wire_y, wire_z,
                                (BlockFace)CHUNK_FACE_FLAT,
                                BLOCK_REDSTONE_WIRE_ON);
    if (!wire_flat)
        return check_failed("redstone connected-on flat mesh missing");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_BUTTON))
        return check_failed("redstone button placement failed");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("redstone wire did not return to off line");
    if (!world_press_button(&world, wire_neighbor_x, wire_y, wire_z))
        return check_failed("redstone button press failed");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_BUTTON_PRESSED)
        return check_failed("redstone button did not become pressed");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_ON)
        return check_failed("redstone button did not power wire");
    world_update_redstone(&world, 2.0f);
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_BUTTON)
        return check_failed("redstone button did not release");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("redstone button pulse did not expire");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_LEVER_OFF))
        return check_failed("redstone lever placement failed");
    bool lever_powered = false;
    if (!world_toggle_lever(&world, wire_neighbor_x, wire_y, wire_z,
                            &lever_powered) ||
        !lever_powered)
        return check_failed("redstone lever did not toggle on");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_LEVER_ON)
        return check_failed("redstone lever on block missing");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_ON)
        return check_failed("redstone lever did not power wire");
    if (!world_toggle_lever(&world, wire_neighbor_x, wire_y, wire_z,
                            &lever_powered) ||
        lever_powered)
        return check_failed("redstone lever did not toggle off");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_LEVER_OFF)
        return check_failed("redstone lever off block missing");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("redstone lever did not unpower wire");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_WOOD_PRESSURE_PLATE))
        return check_failed("wood pressure plate placement failed");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("wood pressure plate powered wire while unpressed");
    WorldPressurePlateTrigger item_trigger = {
        .min_x = (float)wire_neighbor_x + 0.25f,
        .max_x = (float)wire_neighbor_x + 0.75f,
        .min_y = (float)wire_y,
        .max_y = (float)wire_y + 0.4f,
        .min_z = (float)wire_z + 0.25f,
        .max_z = (float)wire_z + 0.75f,
        .mask = WORLD_PRESSURE_TRIGGER_WOOD,
    };
    if (!world_update_pressure_plates_for_triggers(&world, &item_trigger, 1))
        return check_failed("wood pressure plate did not depress under item");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_WOOD_PRESSURE_PLATE_PRESSED)
        return check_failed("wood pressure plate pressed block missing");
    if (world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_ON)
        return check_failed("wood pressure plate did not power wire");
    if (!world_update_pressure_plates_for_triggers(&world, NULL, 0))
        return check_failed("wood pressure plate did not release");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_WOOD_PRESSURE_PLATE ||
        world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("wood pressure plate release did not unpower wire");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z,
                         BLOCK_STONE_PRESSURE_PLATE))
        return check_failed("stone pressure plate placement failed");
    (void)world_update_pressure_plates_for_triggers(&world, &item_trigger, 1);
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_STONE_PRESSURE_PLATE ||
        world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("stone pressure plate depressed under item");
    if (!world_update_pressure_plates(&world,
                                      (float)wire_neighbor_x + 0.25f,
                                      (float)wire_neighbor_x + 0.75f,
                                      (float)wire_y,
                                      (float)wire_y + 1.8f,
                                      (float)wire_z + 0.25f,
                                      (float)wire_z + 0.75f))
        return check_failed("stone pressure plate did not depress under player");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_STONE_PRESSURE_PLATE_PRESSED ||
        world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_ON)
        return check_failed("stone pressure plate did not power wire");
    if (!world_update_pressure_plates_for_triggers(&world, NULL, 0))
        return check_failed("stone pressure plate did not release");
    if (world_get_block(&world, wire_neighbor_x, wire_y, wire_z) !=
        BLOCK_STONE_PRESSURE_PLATE ||
        world_get_block(&world, wire_x, wire_y, wire_z) !=
        BLOCK_REDSTONE_WIRE_OFF)
        return check_failed("stone pressure plate release did not unpower wire");
    if (!world_set_block(&world, wire_neighbor_x, wire_y, wire_z, BLOCK_AIR) ||
        !world_set_block(&world, wire_x, wire_y, wire_z, BLOCK_AIR))
        return check_failed("redstone cleanup failed");

    const int door_power_x = 12;
    const int door_power_y = 24;
    const int door_power_z = 6;
    const int plate_power_x = door_power_x - 1;
    if (!world_set_block(&world, door_power_x, door_power_y, door_power_z,
                         BLOCK_DOOR) ||
        !world_set_block(&world, door_power_x, door_power_y + 1, door_power_z,
                         BLOCK_DOOR_NORTH_UPPER) ||
        !world_set_block(&world, plate_power_x, door_power_y, door_power_z,
                         BLOCK_WOOD_PRESSURE_PLATE))
        return check_failed("pressure-plate door fixture build failed");
    if (!world_update_pressure_plates(&world,
                                      (float)plate_power_x + 0.25f,
                                      (float)plate_power_x + 0.75f,
                                      (float)door_power_y,
                                      (float)door_power_y + 1.8f,
                                      (float)door_power_z + 0.25f,
                                      (float)door_power_z + 0.75f))
        return check_failed("pressure plate did not press for door");
    if (!block_is_door_open(world_get_block(&world,
                                            door_power_x,
                                            door_power_y,
                                            door_power_z)) ||
        !block_is_door_open(world_get_block(&world,
                                            door_power_x,
                                            door_power_y + 1,
                                            door_power_z)))
        return check_failed("pressure plate did not open door");
    if (!world_set_block(&world, door_power_x, door_power_y, door_power_z,
                         BLOCK_DOOR) ||
        !world_set_block(&world, door_power_x, door_power_y + 1, door_power_z,
                         BLOCK_DOOR_NORTH_UPPER))
        return check_failed("powered door manual close fixture failed");
    if (!block_is_door_open(world_get_block(&world,
                                            door_power_x,
                                            door_power_y,
                                            door_power_z)) ||
        !block_is_door_open(world_get_block(&world,
                                            door_power_x,
                                            door_power_y + 1,
                                            door_power_z)))
        return check_failed("powered redstone door stayed closed");
    if (!world_update_pressure_plates(&world,
                                      1000.0f, 1001.0f,
                                      0.0f, 1.0f,
                                      1000.0f, 1001.0f))
        return check_failed("pressure plate did not release for door");
    if (block_is_door_open(world_get_block(&world,
                                           door_power_x,
                                           door_power_y,
                                           door_power_z)) ||
        block_is_door_open(world_get_block(&world,
                                           door_power_x,
                                           door_power_y + 1,
                                           door_power_z)))
        return check_failed("redstone door did not close after plate release");
    if (!world_set_block(&world, plate_power_x, door_power_y, door_power_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, door_power_x, door_power_y, door_power_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, door_power_x, door_power_y + 1, door_power_z,
                         BLOCK_AIR))
        return check_failed("pressure-plate door cleanup failed");

    const int decoder_x = -42;
    const int decoder_y = 28;
    const int decoder_z = 34;
    if (!test_place_seven_segment_decoder(&world,
                                          decoder_x,
                                          decoder_y,
                                          decoder_z))
        return check_failed("seven segment decoder fixture build failed");
    for (int value = 0; value < 8; value++) {
        if (!test_drive_seven_segment_decoder(&world,
                                              decoder_x,
                                              decoder_y,
                                              decoder_z,
                                              value))
            return check_failed("seven segment decoder drive failed");
        if (!test_seven_segment_decoder_matches(&world,
                                                decoder_x,
                                                decoder_y,
                                                decoder_z,
                                                value))
            return check_failed("seven segment decoder output mismatch");
    }

    const int button_hook_x = 5;
    const int button_hook_y = 12;
    const int button_hook_z = 9;
    const int display_anchor_x = 3;
    const int display_anchor_y = 12;
    const int display_anchor_z = 9;
    if (!world_set_block(&world, display_anchor_x, display_anchor_y,
                         display_anchor_z, BLOCK_CRAFTING_TABLE) ||
        !world_set_block(&world, button_hook_x, button_hook_y - 1,
                         button_hook_z, BLOCK_DIAMOND_BLOCK) ||
        !world_set_block(&world, button_hook_x, button_hook_y,
                         button_hook_z, BLOCK_BUTTON))
        return check_failed("button side-effect fixture build failed");
    if (!world_press_button(&world, button_hook_x, button_hook_y,
                            button_hook_z))
        return check_failed("button side-effect press failed");
    if (world_get_block(&world, display_anchor_x + 6, display_anchor_y + 5,
                        display_anchor_z) != BLOCK_AIR ||
        world_get_block(&world, display_anchor_x + 6, display_anchor_y + 5,
                        display_anchor_z + 1) != BLOCK_AIR)
        return check_failed("button mutated display blocks outside redstone");
    if (!world_set_block(&world, display_anchor_x, display_anchor_y,
                         display_anchor_z, BLOCK_AIR) ||
        !world_set_block(&world, button_hook_x, button_hook_y - 1,
                         button_hook_z, BLOCK_AIR) ||
        !world_set_block(&world, button_hook_x, button_hook_y,
                         button_hook_z, BLOCK_AIR))
        return check_failed("button side-effect cleanup failed");

    const int edge_x = -7;
    const int edge_y = 26;
    const int edge_z = 9;
    const int edge_button_x = edge_x - 3;
    const int edge_delay_path[][2] = {
        { -2, -1 }, { -2, -2 }, { -1, -2 }, { 0, -2 },
    };
    if (!world_set_block(&world, edge_button_x, edge_y, edge_z,
                         BLOCK_BUTTON) ||
        !world_set_block(&world, edge_x - 2, edge_y, edge_z,
                         BLOCK_REDSTONE_WIRE_UNCONNECTED) ||
        !world_set_block(&world, edge_x - 1, edge_y, edge_z,
                         BLOCK_REDSTONE_WIRE_UNCONNECTED) ||
        !world_set_block(&world, edge_x, edge_y, edge_z,
                         BLOCK_COMPARATOR_EAST_OFF) ||
        !world_set_block(&world, edge_x, edge_y, edge_z - 1,
                         BLOCK_REPEATER_SOUTH_OFF) ||
        !world_set_block(&world, edge_x + 1, edge_y, edge_z,
                         BLOCK_LAMP_OFF))
        return check_failed("edge detector fixture build failed");
    for (size_t i = 0; i < sizeof(edge_delay_path) / sizeof(edge_delay_path[0]);
         i++) {
        if (!world_set_block(&world,
                             edge_x + edge_delay_path[i][0],
                             edge_y,
                             edge_z + edge_delay_path[i][1],
                             BLOCK_REDSTONE_WIRE_UNCONNECTED))
            return check_failed("edge detector delay path build failed");
    }
    if (!world_press_button(&world, edge_button_x, edge_y, edge_z))
        return check_failed("edge detector button press failed");
    if (world_get_block(&world, edge_x, edge_y, edge_z) !=
            BLOCK_COMPARATOR_EAST_ON ||
        world_get_block(&world, edge_x + 1, edge_y, edge_z) != BLOCK_LAMP)
        return check_failed("edge detector did not emit rising pulse");
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, edge_x, edge_y, edge_z) !=
            BLOCK_COMPARATOR_EAST_OFF ||
        world_get_block(&world, edge_x, edge_y, edge_z - 1) !=
            BLOCK_REPEATER_SOUTH_ON ||
        world_get_block(&world, edge_x + 1, edge_y, edge_z) != BLOCK_LAMP_OFF) {
        fprintf(stderr,
                "edge states comparator=%d repeater=%d lamp=%d rear=%d side=%d delaywire=%d\n",
                (int)world_get_block(&world, edge_x, edge_y, edge_z),
                (int)world_get_block(&world, edge_x, edge_y, edge_z - 1),
                (int)world_get_block(&world, edge_x + 1, edge_y, edge_z),
                (int)world_get_block(&world, edge_x - 1, edge_y, edge_z),
                (int)world_get_block(&world, edge_x - 2, edge_y, edge_z - 2),
                (int)world_get_block(&world, edge_x, edge_y, edge_z - 2));
        return check_failed("edge detector pulse was not limited by delay");
    }
    for (int tick = 0; tick < 20; tick++) {
        world_update_redstone(&world, 0.1f);
        if (world_get_block(&world, edge_x + 1, edge_y, edge_z) !=
            BLOCK_LAMP_OFF)
            return check_failed("edge detector emitted a falling/held pulse");
    }
    if (!world_set_block(&world, edge_button_x, edge_y, edge_z, BLOCK_AIR) ||
        !world_set_block(&world, edge_x - 2, edge_y, edge_z, BLOCK_AIR) ||
        !world_set_block(&world, edge_x - 1, edge_y, edge_z, BLOCK_AIR) ||
        !world_set_block(&world, edge_x, edge_y, edge_z, BLOCK_AIR) ||
        !world_set_block(&world, edge_x, edge_y, edge_z - 1, BLOCK_AIR) ||
        !world_set_block(&world, edge_x + 1, edge_y, edge_z, BLOCK_AIR))
        return check_failed("edge detector cleanup failed");
    for (size_t i = 0; i < sizeof(edge_delay_path) / sizeof(edge_delay_path[0]);
         i++) {
        if (!world_set_block(&world,
                             edge_x + edge_delay_path[i][0],
                             edge_y,
                             edge_z + edge_delay_path[i][1],
                             BLOCK_AIR))
            return check_failed("edge detector delay path cleanup failed");
    }

    const int tff_x = -42;
    const int tff_y = 26;
    const int tff_z = -18;
    if (!test_place_t_flip_flop(&world, tff_x, tff_y, tff_z, true))
        return check_failed("t flip flop fixture build failed");
    test_tick_redstone(&world, 20);
    if (test_t_flip_flop_q(&world, tff_x, tff_y, tff_z) ||
        world_get_block(&world, tff_x + 25, tff_y, tff_z) !=
            BLOCK_REDSTONE_TORCH_ON)
        return check_failed("t flip flop did not initialize to reset state");
    for (int press = 1; press <= 4; press++) {
        bool expected_q = (press & 1) != 0;

        if (!world_press_button(&world, tff_x, tff_y, tff_z))
            return check_failed("t flip flop button press failed");
        test_tick_redstone(&world, 30);
        if (test_t_flip_flop_q(&world, tff_x, tff_y, tff_z) !=
            expected_q)
            return check_failed("t flip flop did not toggle on input edge");
        if ((world_get_block(&world, tff_x + 25, tff_y, tff_z) ==
             BLOCK_REDSTONE_TORCH_ON) == expected_q)
            return check_failed("t flip flop complement torch disagreed");
    }

    const int counter_x = -42;
    const int counter_y = 26;
    const int counter_z = -30;
    const int counter_bit_stride = 30;
    if (!test_place_t_flip_flop(&world,
                                counter_x,
                                counter_y,
                                counter_z,
                                true) ||
        !test_place_t_flip_flop(&world,
                                counter_x + counter_bit_stride,
                                counter_y,
                                counter_z,
                                false) ||
        !test_place_t_flip_flop(&world,
                                counter_x + 2 * counter_bit_stride,
                                counter_y,
                                counter_z,
                                false) ||
        !test_wire_line_x(&world,
                          counter_x + 26,
                          counter_x + counter_bit_stride - 1,
                          counter_y,
                          counter_z) ||
        !test_wire_line_x(&world,
                          counter_x + counter_bit_stride + 26,
                          counter_x + 2 * counter_bit_stride - 1,
                          counter_y,
                          counter_z))
        return check_failed("button counter fixture build failed");
    test_tick_redstone(&world, 30);
    if (!test_reset_t_flip_flop(&world,
                                counter_x,
                                counter_y,
                                counter_z) ||
        !test_reset_t_flip_flop(&world,
                                counter_x + counter_bit_stride,
                                counter_y,
                                counter_z) ||
        !test_reset_t_flip_flop(&world,
                                counter_x + 2 * counter_bit_stride,
                                counter_y,
                                counter_z))
        return check_failed("button counter reset failed");
    if (test_ripple_counter_value(&world,
                                  counter_x,
                                  counter_y,
                                  counter_z,
                                  counter_bit_stride,
                                  3) != 0)
        return check_failed("button counter reset did not clear all bits");
    for (int press = 1; press <= 16; press++) {
        int expected = press & 7;

        if (!world_press_button(&world, counter_x, counter_y, counter_z))
            return check_failed("button counter press failed");
        test_tick_redstone(&world, 30);
        if (test_ripple_counter_value(&world,
                                      counter_x,
                                      counter_y,
                                      counter_z,
                                      counter_bit_stride,
                                      3) != expected)
            return check_failed("button counter did not ripple count");
    }
    if (!test_reset_t_flip_flop(&world,
                                counter_x,
                                counter_y,
                                counter_z) ||
        !test_reset_t_flip_flop(&world,
                                counter_x + counter_bit_stride,
                                counter_y,
                                counter_z) ||
        !test_reset_t_flip_flop(&world,
                                counter_x + 2 * counter_bit_stride,
                                counter_y,
                                counter_z))
        return check_failed("button counter final reset failed");

    const int repeater_x = 11;
    const int repeater_y = 26;
    const int repeater_z = 3;
    if (!world_set_block(&world, repeater_x - 1, repeater_y, repeater_z,
                         BLOCK_REDSTONE_BLOCK) ||
        !world_set_block(&world, repeater_x, repeater_y, repeater_z,
                         BLOCK_REPEATER_EAST_OFF) ||
        !world_set_block(&world, repeater_x + 1, repeater_y, repeater_z,
                         BLOCK_LAMP_OFF))
        return check_failed("repeater fixture build failed");
    if (world_repeater_delay_ticks(&world, repeater_x, repeater_y,
                                   repeater_z) != 1)
        return check_failed("repeater default delay missing");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, repeater_x, repeater_y, repeater_z) !=
            BLOCK_REPEATER_EAST_OFF ||
        world_get_block(&world, repeater_x + 1, repeater_y, repeater_z) !=
            BLOCK_LAMP_OFF)
        return check_failed("repeater ignored its delay");
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, repeater_x, repeater_y, repeater_z) !=
            BLOCK_REPEATER_EAST_ON ||
        world_get_block(&world, repeater_x + 1, repeater_y, repeater_z) !=
            BLOCK_LAMP)
        return check_failed("repeater did not power only its front side");
    uint8_t repeater_delay = 0;
    if (!world_cycle_repeater_delay(&world, repeater_x, repeater_y,
                                    repeater_z, &repeater_delay) ||
        repeater_delay != 2 ||
        world_repeater_delay_ticks(&world, repeater_x, repeater_y,
                                   repeater_z) != 2)
        return check_failed("repeater delay cycle failed");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("post-repeater-delay mesh rebuild failed");
    const Chunk *repeater_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *repeater_flat = find_chunk_face(
        repeater_chunk,
        repeater_x, repeater_y, repeater_z,
        (BlockFace)CHUNK_FACE_FLAT, BLOCK_REPEATER_EAST_ON);
    if (!repeater_flat || repeater_flat->height != 2)
        return check_failed("repeater delay mesh state missing");
    if (!world_set_block(&world, repeater_x - 1, repeater_y, repeater_z,
                         BLOCK_AIR))
        return check_failed("repeater source removal failed");
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, repeater_x, repeater_y, repeater_z) !=
            BLOCK_REPEATER_EAST_ON ||
        world_get_block(&world, repeater_x + 1, repeater_y, repeater_z) !=
            BLOCK_LAMP)
        return check_failed("configured repeater released too early");
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, repeater_x, repeater_y, repeater_z) !=
            BLOCK_REPEATER_EAST_OFF ||
        world_get_block(&world, repeater_x + 1, repeater_y, repeater_z) !=
            BLOCK_LAMP_OFF)
        return check_failed("configured repeater did not release");
    if (!world_set_block(&world, repeater_x, repeater_y, repeater_z - 1,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("repeater side-source fixture failed");
    world_update_redstone(&world, 0.0f);
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, repeater_x, repeater_y, repeater_z) !=
            BLOCK_REPEATER_EAST_OFF ||
        world_get_block(&world, repeater_x + 1, repeater_y, repeater_z) !=
            BLOCK_LAMP_OFF)
        return check_failed("repeater accepted a side/front power source");
    if (!world_set_block(&world, repeater_x, repeater_y, repeater_z - 1,
                         BLOCK_AIR) ||
        !world_set_block(&world, repeater_x, repeater_y, repeater_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, repeater_x + 1, repeater_y, repeater_z,
                         BLOCK_AIR))
        return check_failed("repeater cleanup failed");

    const int torch_line_x = 7;
    const int torch_line_y = 26;
    const int torch_line_z = 5;
    if (!world_set_block(&world, torch_line_x, torch_line_y - 1,
                         torch_line_z, BLOCK_STONE) ||
        !world_set_block(&world, torch_line_x, torch_line_y,
                         torch_line_z, BLOCK_REDSTONE_TORCH_ON) ||
        !world_set_block(&world, torch_line_x + 1, torch_line_y,
                         torch_line_z, BLOCK_REDSTONE_WIRE_UNCONNECTED) ||
        !world_set_block(&world, torch_line_x + 2, torch_line_y,
                         torch_line_z, BLOCK_REDSTONE_WIRE_UNCONNECTED) ||
        !world_set_block(&world, torch_line_x + 3, torch_line_y,
                         torch_line_z, BLOCK_REDSTONE_WIRE_UNCONNECTED) ||
        !world_set_block(&world, torch_line_x + 4, torch_line_y,
                         torch_line_z, BLOCK_REPEATER_EAST_OFF) ||
        !world_set_block(&world, torch_line_x + 5, torch_line_y,
                         torch_line_z, BLOCK_LAMP_OFF))
        return check_failed("torch line fixture build failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, torch_line_x, torch_line_y, torch_line_z) !=
            BLOCK_REDSTONE_TORCH_ON ||
        world_get_block(&world, torch_line_x + 1, torch_line_y,
                        torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
        world_get_block(&world, torch_line_x + 2, torch_line_y,
                        torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
        world_get_block(&world, torch_line_x + 3, torch_line_y,
                        torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
        world_get_block(&world, torch_line_x + 4, torch_line_y,
                        torch_line_z) != BLOCK_REPEATER_EAST_OFF ||
        world_get_block(&world, torch_line_x + 5, torch_line_y,
                        torch_line_z) != BLOCK_LAMP_OFF)
        return check_failed("torch line did not settle before repeater delay");
    for (int tick = 0; tick < 6; tick++) {
        world_update_redstone(&world, 0.1f);
        if (world_get_block(&world, torch_line_x, torch_line_y,
                            torch_line_z) != BLOCK_REDSTONE_TORCH_ON ||
            world_get_block(&world, torch_line_x + 1, torch_line_y,
                            torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
            world_get_block(&world, torch_line_x + 2, torch_line_y,
                            torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
            world_get_block(&world, torch_line_x + 3, torch_line_y,
                            torch_line_z) != BLOCK_REDSTONE_WIRE_ON ||
            world_get_block(&world, torch_line_x + 4, torch_line_y,
                            torch_line_z) != BLOCK_REPEATER_EAST_ON ||
            world_get_block(&world, torch_line_x + 5, torch_line_y,
                            torch_line_z) != BLOCK_LAMP)
            return check_failed("torch line flashed instead of staying stable");
    }
    if (!world_set_block(&world, torch_line_x, torch_line_y - 1,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x, torch_line_y,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x + 1, torch_line_y,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x + 2, torch_line_y,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x + 3, torch_line_y,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x + 4, torch_line_y,
                         torch_line_z, BLOCK_AIR) ||
        !world_set_block(&world, torch_line_x + 5, torch_line_y,
                         torch_line_z, BLOCK_AIR))
        return check_failed("torch line cleanup failed");

    const int not_x = 11;
    const int not_y = 26;
    const int not_z = 4;
    if (!world_set_block(&world, not_x - 1, not_y, not_z,
                         BLOCK_REDSTONE_BLOCK) ||
        !world_set_block(&world, not_x, not_y, not_z,
                         BLOCK_REPEATER_EAST_OFF) ||
        !world_set_block(&world, not_x + 1, not_y, not_z,
                         BLOCK_STONE) ||
        !world_set_block(&world, not_x + 2, not_y, not_z,
                         BLOCK_LAMP_OFF) ||
        !world_set_block(&world, not_x + 1, not_y + 1, not_z,
                         BLOCK_REDSTONE_TORCH_ON))
        return check_failed("not-gate fixture build failed");
    world_update_redstone(&world, 0.0f);
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, not_x, not_y, not_z) !=
            BLOCK_REPEATER_EAST_ON ||
        world_get_block(&world, not_x + 2, not_y, not_z) !=
            BLOCK_LAMP ||
        world_get_block(&world, not_x + 1, not_y + 1, not_z) !=
            BLOCK_REDSTONE_TORCH_OFF)
        return check_failed("powered block did not drive lamp and invert torch");
    if (!world_set_block(&world, not_x - 1, not_y, not_z, BLOCK_AIR))
        return check_failed("not-gate source removal failed");
    world_update_redstone(&world, 0.0f);
    world_update_redstone(&world, 0.1f);
    if (world_get_block(&world, not_x, not_y, not_z) !=
            BLOCK_REPEATER_EAST_OFF ||
        world_get_block(&world, not_x + 2, not_y, not_z) !=
            BLOCK_LAMP_OFF ||
        world_get_block(&world, not_x + 1, not_y + 1, not_z) !=
            BLOCK_REDSTONE_TORCH_ON)
        return check_failed("powered block not-gate did not release");
    if (!world_set_block(&world, not_x, not_y, not_z, BLOCK_AIR) ||
        !world_set_block(&world, not_x + 1, not_y, not_z, BLOCK_AIR) ||
        !world_set_block(&world, not_x + 2, not_y, not_z, BLOCK_AIR) ||
        !world_set_block(&world, not_x + 1, not_y + 1, not_z, BLOCK_AIR))
        return check_failed("not-gate cleanup failed");

    const int comparator_x = 11;
    const int comparator_y = 26;
    const int comparator_z = 6;
    if (!world_set_block(&world, comparator_x - 1, comparator_y, comparator_z,
                         BLOCK_REDSTONE_BLOCK) ||
        !world_set_block(&world, comparator_x, comparator_y, comparator_z,
                         BLOCK_COMPARATOR_EAST_OFF) ||
        !world_set_block(&world, comparator_x + 1, comparator_y, comparator_z,
                         BLOCK_LAMP_OFF))
        return check_failed("comparator fixture build failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, comparator_x, comparator_y, comparator_z) !=
            BLOCK_COMPARATOR_EAST_ON ||
        world_get_block(&world, comparator_x + 1, comparator_y, comparator_z) !=
            BLOCK_LAMP)
        return check_failed("comparator did not accept rear input");
    if (!world_set_block(&world, comparator_x - 1, comparator_y, comparator_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, comparator_x, comparator_y, comparator_z - 1,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("comparator side-only fixture failed");
    world_update_redstone(&world, 0.0f);
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, comparator_x, comparator_y, comparator_z) !=
            BLOCK_COMPARATOR_EAST_OFF ||
        world_get_block(&world, comparator_x + 1, comparator_y, comparator_z) !=
            BLOCK_LAMP_OFF)
        return check_failed("comparator powered from a side input");
    if (!world_set_block(&world, comparator_x, comparator_y, comparator_z - 1,
                         BLOCK_AIR) ||
        !world_set_block(&world, comparator_x, comparator_y, comparator_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, comparator_x + 1, comparator_y, comparator_z,
                         BLOCK_AIR))
        return check_failed("comparator cleanup failed");

    const int side_torch_x = 7;
    const int side_torch_y = 27;
    const int side_torch_z = 6;
    if (!world_set_block(&world, side_torch_x, side_torch_y,
                         side_torch_z, BLOCK_STONE) ||
        !world_set_block(&world, side_torch_x + 1, side_torch_y,
                         side_torch_z, BLOCK_REDSTONE_TORCH_ON) ||
        !world_set_block(&world, side_torch_x + 2, side_torch_y,
                         side_torch_z, BLOCK_LAMP_OFF))
        return check_failed("side torch fixture build failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, side_torch_x + 1, side_torch_y,
                        side_torch_z) != BLOCK_REDSTONE_TORCH_ON ||
        world_get_block(&world, side_torch_x + 2, side_torch_y,
                        side_torch_z) != BLOCK_LAMP)
        return check_failed("side torch did not power adjacent load");
    if (!world_rebuild_dirty_meshes(&world))
        return check_failed("side torch mesh rebuild failed");
    const Chunk *side_torch_chunk = world_get_chunk(&world, 0, 0);
    const ChunkFace *side_torch_face =
        find_chunk_face(side_torch_chunk,
                        side_torch_x + 1,
                        side_torch_y,
                        side_torch_z,
                        (BlockFace)CHUNK_FACE_CROSS_A,
                        BLOCK_REDSTONE_TORCH_ON);
    if (!side_torch_face || side_torch_face->height != FACE_LEFT)
        return check_failed("side torch mesh did not record side support");
    if (!world_set_block(&world, side_torch_x - 1, side_torch_y,
                         side_torch_z, BLOCK_REDSTONE_BLOCK))
        return check_failed("side torch input failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, side_torch_x + 1, side_torch_y,
                        side_torch_z) != BLOCK_REDSTONE_TORCH_OFF ||
        world_get_block(&world, side_torch_x + 2, side_torch_y,
                        side_torch_z) != BLOCK_LAMP_OFF)
        return check_failed("side torch did not invert powered support");
    if (!world_set_block(&world, side_torch_x - 1, side_torch_y,
                         side_torch_z, BLOCK_AIR) ||
        !world_set_block(&world, side_torch_x, side_torch_y,
                         side_torch_z, BLOCK_AIR) ||
        !world_set_block(&world, side_torch_x + 1, side_torch_y,
                         side_torch_z, BLOCK_AIR) ||
        !world_set_block(&world, side_torch_x + 2, side_torch_y,
                         side_torch_z, BLOCK_AIR))
        return check_failed("side torch cleanup failed");

    const int latch_x = 11;
    const int latch_y = 27;
    const int latch_z = 6;
    const int latch_feedback[][2] = {
        { 1, 0 }, { 2, 0 }, { 2, 1 }, { 2, 2 }, { 1, 2 }, { 0, 2 },
        { -1, 2 }, { -2, 2 }, { -2, 1 }, { -2, 0 }, { -1, 0 },
    };
    if (!world_set_block(&world, latch_x, latch_y, latch_z,
                         BLOCK_COMPARATOR_EAST_OFF))
        return check_failed("comparator latch build failed");
    for (size_t i = 0; i < sizeof(latch_feedback) / sizeof(latch_feedback[0]);
         i++) {
        if (!world_set_block(&world,
                             latch_x + latch_feedback[i][0],
                             latch_y,
                             latch_z + latch_feedback[i][1],
                             BLOCK_REDSTONE_WIRE_UNCONNECTED))
            return check_failed("comparator latch feedback build failed");
    }
    if (!world_set_block(&world, latch_x - 3, latch_y, latch_z,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("comparator latch set failed");
    world_update_redstone(&world, 0.0f);
    if (!world_set_block(&world, latch_x - 3, latch_y, latch_z,
                         BLOCK_AIR))
        return check_failed("comparator latch set release failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, latch_x, latch_y, latch_z) !=
            BLOCK_COMPARATOR_EAST_ON)
        return check_failed("comparator latch did not hold");
    if (!world_set_block(&world, latch_x, latch_y, latch_z - 1,
                         BLOCK_REDSTONE_BLOCK))
        return check_failed("comparator latch reset failed");
    world_update_redstone(&world, 0.0f);
    if (world_get_block(&world, latch_x, latch_y, latch_z) !=
            BLOCK_COMPARATOR_EAST_OFF)
        return check_failed("comparator latch did not reset");
    if (!world_set_block(&world, latch_x, latch_y, latch_z - 1,
                         BLOCK_AIR) ||
        !world_set_block(&world, latch_x, latch_y, latch_z,
                         BLOCK_AIR) ||
        !world_set_block(&world, latch_x - 3, latch_y, latch_z,
                         BLOCK_AIR))
        return check_failed("comparator latch cleanup failed");
    for (size_t i = 0; i < sizeof(latch_feedback) / sizeof(latch_feedback[0]);
         i++) {
        if (!world_set_block(&world,
                             latch_x + latch_feedback[i][0],
                             latch_y,
                             latch_z + latch_feedback[i][1],
                             BLOCK_AIR))
            return check_failed("comparator latch feedback cleanup failed");
    }

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
    const int saved_repeater_x = WORLD_CHUNK_SIZE + 2;
    const int saved_repeater_y = 2;
    const int saved_repeater_z = 2;
    uint8_t saved_repeater_delay = 0;
    if (!world_set_block(&world,
                         saved_repeater_x,
                         saved_repeater_y,
                         saved_repeater_z,
                         BLOCK_REPEATER_EAST_OFF) ||
        !world_cycle_repeater_delay(&world,
                                    saved_repeater_x,
                                    saved_repeater_y,
                                    saved_repeater_z,
                                    &saved_repeater_delay) ||
        saved_repeater_delay != 2 ||
        !world_cycle_repeater_delay(&world,
                                    saved_repeater_x,
                                    saved_repeater_y,
                                    saved_repeater_z,
                                    &saved_repeater_delay) ||
        saved_repeater_delay != 3)
        return check_failed("saved repeater delay setup failed");
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
    if (world_get_block(&world,
                        saved_repeater_x,
                        saved_repeater_y,
                        saved_repeater_z) != BLOCK_REPEATER_EAST_OFF ||
        world_repeater_delay_ticks(&world,
                                   saved_repeater_x,
                                   saved_repeater_y,
                                   saved_repeater_z) != 3)
        return check_failed("saved repeater delay did not reload after eviction");

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
                                        true,
                                        TEST_RENDER_DISTANCE,
                                        0.0f,
                                        0.0f,
                                        world_dir))
        return check_failed("reload world_init_infinite_procedural failed");
    if (world_get_block(&reloaded_world, WORLD_CHUNK_SIZE, 1, 0) != BLOCK_WOOD)
        return check_failed("saved block edit did not survive world reload");
    if (world_get_block(&reloaded_world,
                        saved_repeater_x,
                        saved_repeater_y,
                        saved_repeater_z) != BLOCK_REPEATER_EAST_OFF ||
        world_repeater_delay_ticks(&reloaded_world,
                                   saved_repeater_x,
                                   saved_repeater_y,
                                   saved_repeater_z) != 3)
        return check_failed("saved repeater delay did not survive world reload");
    world_free(&reloaded_world);

    if (!world_init_infinite_procedural(&capped_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        false,
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
    static VoxelWorld async_world;
    char async_world_dir_template[] = "/tmp/voxel-world-async-XXXXXX";
    char *async_world_dir = mkdtemp(async_world_dir_template);
    if (!async_world_dir)
        return check_failed("async mkdtemp failed");
    world_init(&async_world);
    if (!world_init_infinite_procedural(&async_world,
                                        TEST_WORLD_SEED,
                                        12,
                                        false,
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

    static ChunkGenResult result;
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
