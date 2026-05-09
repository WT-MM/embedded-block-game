#include "world_gen.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define WORLDGEN_SEA_LEVEL 14
#define WORLDGEN_TREE_MIN_Y (WORLDGEN_SEA_LEVEL + 1)
#define WORLDGEN_TREE_RADIUS 2
#define WORLDGEN_LAVA_POOL_RADIUS 3
#define WORLDGEN_LAVA_POOL_CENTER_RADIUS 6

typedef struct {
    int surface_y;
    WorldBiome biome;
    bool underwater;
    bool beach;
    bool clay_patch;
} WorldgenColumn;

static uint32_t hash_world_coord(int wx, int wz, uint32_t base_seed,
                                 uint32_t salt)
{
    uint32_t h = base_seed ^ salt;

    h ^= (uint32_t)wx * 0x9e3779b9u;
    h ^= (uint32_t)wz * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static float smoothstep01(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float smoothstep_range(float edge0, float edge1, float x)
{
    if (edge0 == edge1)
        return x >= edge1 ? 1.0f : 0.0f;
    return smoothstep01((x - edge0) / (edge1 - edge0));
}

static float value_noise_octave(int wx, int wz, int period, uint32_t base_seed,
                                uint32_t salt)
{
    int gx = (wx >= 0) ? (wx / period) : ((wx - period + 1) / period);
    int gz = (wz >= 0) ? (wz / period) : ((wz - period + 1) / period);
    int rx = wx - gx * period;
    int rz = wz - gz * period;
    float fx = smoothstep01((float)rx / (float)period);
    float fz = smoothstep01((float)rz / (float)period);

    float c00 = (float)(hash_world_coord(gx,     gz,     base_seed, salt) & 0xFFFFu);
    float c10 = (float)(hash_world_coord(gx + 1, gz,     base_seed, salt) & 0xFFFFu);
    float c01 = (float)(hash_world_coord(gx,     gz + 1, base_seed, salt) & 0xFFFFu);
    float c11 = (float)(hash_world_coord(gx + 1, gz + 1, base_seed, salt) & 0xFFFFu);

    float a = c00 + (c10 - c00) * fx;
    float b = c01 + (c11 - c01) * fx;
    return (a + (b - a) * fz) / 65535.0f;
}

static float worldgen_biome_value(int wx, int wz, uint32_t base_seed)
{
    float broad = value_noise_octave(wx, wz, 64, base_seed, 0xb10bu);
    float local = value_noise_octave(wx, wz, 24, base_seed, 0xb22bu);

    return broad * 0.78f + local * 0.22f;
}

static WorldBiome worldgen_biome_from_value(float b)
{
    if (b < 0.22f)
        return WORLD_BIOME_OCEAN;
    if (b < 0.38f)
        return WORLD_BIOME_PLAINS;
    if (b < 0.54f)
        return WORLD_BIOME_DESERT;
    if (b < 0.70f)
        return WORLD_BIOME_HILLS;
    return WORLD_BIOME_MOUNTAINS;
}

const char *world_biome_name(WorldBiome biome)
{
    switch (biome) {
    case WORLD_BIOME_PLAINS:
        return "plains";
    case WORLD_BIOME_OCEAN:
        return "ocean";
    case WORLD_BIOME_DESERT:
        return "desert";
    case WORLD_BIOME_HILLS:
        return "hills";
    case WORLD_BIOME_MOUNTAINS:
        return "mountains";
    default:
        return "unknown";
    }
}

WorldBiome world_biome_at(const VoxelWorld *world, int wx, int wz)
{
    if (!world)
        return WORLD_BIOME_PLAINS;

    return worldgen_biome_from_value(
        worldgen_biome_value(wx, wz, world->procedural_seed));
}

static bool worldgen_clay_patch(int wx, int wz, uint32_t base_seed)
{
    float patch = value_noise_octave(wx, wz, 18, base_seed, 0xc1a4u) * 0.65f +
                  value_noise_octave(wx, wz, 6,  base_seed, 0xc1a5u) * 0.35f;

    return patch > 0.72f;
}

static int worldgen_surface_y_at(int wx, int wz, uint32_t base_seed,
                                 float *biome_value_out)
{
    float low  = value_noise_octave(wx, wz, 32, base_seed, 0xa1u);
    float mid  = value_noise_octave(wx, wz, 12, base_seed, 0xb2u);
    float fine = value_noise_octave(wx, wz, 4,  base_seed, 0xc3u);
    float biome_value = worldgen_biome_value(wx, wz, base_seed);
    float ocean_w = 1.0f - smoothstep_range(0.18f, 0.30f, biome_value);
    float plains_w = smoothstep_range(0.18f, 0.30f, biome_value) *
                     (1.0f - smoothstep_range(0.34f, 0.46f, biome_value));
    float desert_w = smoothstep_range(0.34f, 0.46f, biome_value) *
                     (1.0f - smoothstep_range(0.50f, 0.62f, biome_value));
    float hills_w = smoothstep_range(0.50f, 0.62f, biome_value) *
                    (1.0f - smoothstep_range(0.66f, 0.82f, biome_value));
    float mountain_w = smoothstep_range(0.66f, 0.82f, biome_value);
    float weight_sum = ocean_w + plains_w + desert_w + hills_w + mountain_w;
    float ridge = fabsf(value_noise_octave(wx, wz, 24, base_seed, 0xa44u) - 0.5f) * 2.0f;
    float dune = fabsf(value_noise_octave(wx, wz, 20, base_seed, 0xde57u) - 0.5f) * 2.0f;
    float ocean_h = (float)WORLDGEN_SEA_LEVEL - 5.0f +
                    (low - 0.5f) * 2.0f +
                    (mid - 0.5f) * 2.0f +
                    (fine - 0.5f) * 0.8f;
    float plains_h = (float)WORLDGEN_SEA_LEVEL + 1.0f +
                     (low - 0.5f) * 3.0f +
                     (fine - 0.5f) * 1.2f;
    float desert_h = (float)WORLDGEN_SEA_LEVEL + 1.0f +
                     (low - 0.5f) * 2.0f +
                     dune * 2.6f +
                     (fine - 0.5f) * 0.8f;
    float hills_h = (float)WORLDGEN_SEA_LEVEL + 3.0f +
                    (low - 0.5f) * 5.5f +
                    (mid - 0.5f) * 8.0f +
                    (fine - 0.5f) * 1.8f;
    float mountain_h = (float)WORLDGEN_SEA_LEVEL + 7.0f +
                       ridge * 11.5f +
                       (low - 0.5f) * 4.0f +
                       (fine - 0.5f) * 2.5f;
    float h_f;
    int h;

    if (weight_sum <= 0.0001f)
        weight_sum = 1.0f;
    h_f = (ocean_h * ocean_w +
           plains_h * plains_w +
           desert_h * desert_w +
           hills_h * hills_w +
           mountain_h * mountain_w) / weight_sum;
    h = (int)floorf(h_f + 0.5f);

    if (h < 2) h = 2;
    if (h >= WORLD_CHUNK_HEIGHT - 1) h = WORLD_CHUNK_HEIGHT - 2;

    if (biome_value_out)
        *biome_value_out = biome_value;
    return h;
}

static bool worldgen_column_touches_water(int wx, int wz, uint32_t base_seed)
{
    for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (dx == 0 && dz == 0)
                continue;
            if (abs(dx) + abs(dz) > 2)
                continue;
            if (worldgen_surface_y_at(wx + dx, wz + dz, base_seed, NULL) <
                WORLDGEN_SEA_LEVEL)
                return true;
        }
    }
    return false;
}

static WorldgenColumn worldgen_column_at(int wx, int wz, uint32_t base_seed)
{
    float biome_value;
    int h = worldgen_surface_y_at(wx, wz, base_seed, &biome_value);
    WorldgenColumn column;

    column.surface_y = h;
    column.biome = worldgen_biome_from_value(biome_value);
    column.underwater = h < WORLDGEN_SEA_LEVEL;
    column.beach = column.biome != WORLD_BIOME_MOUNTAINS &&
                   h == WORLDGEN_SEA_LEVEL &&
                   worldgen_column_touches_water(wx, wz, base_seed);
    column.clay_patch = worldgen_clay_patch(wx, wz, base_seed);
    return column;
}

static BlockID worldgen_column_block(const WorldgenColumn *column, int y)
{
    if (y == column->surface_y) {
        if (column->clay_patch && column->underwater)
            return BLOCK_CLAY;
        if (column->underwater || column->beach ||
            column->biome == WORLD_BIOME_DESERT)
            return BLOCK_SAND;
        if (column->surface_y >= WORLDGEN_SEA_LEVEL + 8)
            return BLOCK_STONE;
        return BLOCK_GRASS;
    }

    if (y >= column->surface_y - 3) {
        if (column->clay_patch && column->underwater &&
            y >= column->surface_y - 1)
            return BLOCK_CLAY;
        if (column->underwater || column->beach ||
            column->biome == WORLD_BIOME_DESERT)
            return (y <= column->surface_y - 2) ? BLOCK_SANDSTONE : BLOCK_SAND;
        if (column->surface_y >= WORLDGEN_SEA_LEVEL + 8)
            return BLOCK_STONE;
        return BLOCK_DIRT;
    }

    if ((column->underwater || column->beach ||
         column->biome == WORLD_BIOME_DESERT) &&
        y >= column->surface_y - 5)
        return BLOCK_SANDSTONE;
    return BLOCK_STONE;
}

static bool worldgen_tree_candidate(int wx, int wz, uint32_t base_seed)
{
    WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xd00du);

    if (column.underwater || column.beach ||
        column.biome == WORLD_BIOME_OCEAN ||
        column.biome == WORLD_BIOME_DESERT ||
        column.biome == WORLD_BIOME_MOUNTAINS ||
        column.surface_y >= WORLDGEN_SEA_LEVEL + 8 ||
        column.surface_y < WORLDGEN_TREE_MIN_Y)
        return false;

    uint32_t spacing = (column.biome == WORLD_BIOME_HILLS) ? 48u : 72u;
    if ((roll % spacing) != 0u)
        return false;
    return true;
}

static bool worldgen_wants_tree(int wx, int wz, uint32_t base_seed)
{
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xd00du);

    if (!worldgen_tree_candidate(wx, wz, base_seed))
        return false;

    for (int dz = -WORLDGEN_TREE_RADIUS; dz <= WORLDGEN_TREE_RADIUS; dz++) {
        for (int dx = -WORLDGEN_TREE_RADIUS; dx <= WORLDGEN_TREE_RADIUS; dx++) {
            if (dx == 0 && dz == 0)
                continue;
            uint32_t neighbor = hash_world_coord(wx + dx, wz + dz,
                                                 base_seed, 0xd00du);
            if (!worldgen_tree_candidate(wx + dx, wz + dz, base_seed))
                continue;
            if (neighbor < roll)
                return false;
        }
    }
    return true;
}

static bool worldgen_wants_flower(int wx, int wz, uint32_t base_seed,
                                  WorldBiome biome)
{
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xf10fu);
    uint32_t threshold;

    if (biome == WORLD_BIOME_OCEAN ||
        biome == WORLD_BIOME_DESERT ||
        biome == WORLD_BIOME_MOUNTAINS)
        return false;

    threshold = (biome == WORLD_BIOME_PLAINS) ? 5u : 2u;
    return (roll & 0xffu) < threshold;
}

static bool worldgen_wants_mushroom(int wx, int wz, uint32_t base_seed,
                                    WorldBiome biome)
{
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0x6d45u);
    float shade = value_noise_octave(wx, wz, 10, base_seed, 0x5adeu);

    if (biome == WORLD_BIOME_OCEAN ||
        biome == WORLD_BIOME_DESERT ||
        biome == WORLD_BIOME_MOUNTAINS)
        return false;

    return (roll & 0xffu) < 6u && shade > 0.48f;
}

static bool worldgen_wants_cactus(int wx, int wz, uint32_t base_seed,
                                  const WorldgenColumn *column)
{
    uint32_t roll;

    if (!column || column->biome != WORLD_BIOME_DESERT ||
        column->underwater || column->beach ||
        column->surface_y < WORLDGEN_SEA_LEVEL)
        return false;

    roll = hash_world_coord(wx, wz, base_seed, 0xcac7u);
    return (roll & 0xffu) < 5u;
}

static bool worldgen_lava_pool_center_candidate(int wx, int wz,
                                                uint32_t base_seed)
{
    WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
    uint32_t roll;

    if (column.biome != WORLD_BIOME_DESERT ||
        column.underwater || column.beach ||
        column.surface_y < WORLDGEN_SEA_LEVEL + 1 ||
        column.surface_y >= WORLD_CHUNK_HEIGHT - 3)
        return false;

    roll = hash_world_coord(wx, wz, base_seed, 0x1a7au);
    return (roll & 0x3ffu) < 3u;
}

static bool worldgen_wants_lava_pool(int wx, int wz, uint32_t base_seed)
{
    uint32_t roll = hash_world_coord(wx, wz, base_seed, 0x1a7au);

    if (!worldgen_lava_pool_center_candidate(wx, wz, base_seed))
        return false;

    for (int dz = -WORLDGEN_LAVA_POOL_CENTER_RADIUS;
         dz <= WORLDGEN_LAVA_POOL_CENTER_RADIUS; dz++) {
        for (int dx = -WORLDGEN_LAVA_POOL_CENTER_RADIUS;
             dx <= WORLDGEN_LAVA_POOL_CENTER_RADIUS; dx++) {
            uint32_t neighbor;

            if (dx == 0 && dz == 0)
                continue;
            if (!worldgen_lava_pool_center_candidate(wx + dx, wz + dz,
                                                     base_seed))
                continue;
            neighbor = hash_world_coord(wx + dx, wz + dz,
                                        base_seed, 0x1a7au);
            if (neighbor < roll)
                return false;
        }
    }
    return true;
}

static void worldgen_set_block_local(Chunk *chunk, int lx, int ly, int lz,
                                     BlockID block, bool overwrite)
{
    if (lx < 0 || lx >= WORLD_CHUNK_SIZE) return;
    if (lz < 0 || lz >= WORLD_CHUNK_SIZE) return;
    if (ly < 0 || ly >= WORLD_CHUNK_HEIGHT) return;
    if (!overwrite && chunk->blocks[ly][lz][lx] != BLOCK_AIR)
        return;
    chunk->blocks[ly][lz][lx] = block;
}

static void worldgen_place_tree(Chunk *chunk, int local_x, int trunk_top_y,
                                int local_z, uint32_t roll)
{
    int trunk_height = 4 + (int)(roll & 1u);
    int trunk_base = trunk_top_y - trunk_height + 1;
    int trunk_ground = (trunk_base > 0) ? trunk_base - 1 : trunk_base;

    for (int y = trunk_ground; y <= trunk_top_y; y++)
        worldgen_set_block_local(chunk, local_x, y, local_z, BLOCK_WOOD, true);

    int canopy_y = trunk_top_y;
    for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
            if ((dx == -2 || dx == 2) && (dz == -2 || dz == 2))
                continue;
            worldgen_set_block_local(chunk, local_x + dx, canopy_y, local_z + dz,
                                     BLOCK_LEAVES, false);
        }
    }
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0)
                continue;
            worldgen_set_block_local(chunk, local_x + dx, canopy_y + 1,
                                     local_z + dz, BLOCK_LEAVES, false);
        }
    }
    worldgen_set_block_local(chunk, local_x, canopy_y + 2, local_z,
                             BLOCK_LEAVES, false);
}

static void worldgen_place_cactus(Chunk *chunk, int local_x, int base_y,
                                  int local_z, uint32_t roll)
{
    int height = 2 + (int)(roll % 3u);

    for (int i = 0; i < height; i++)
        worldgen_set_block_local(chunk, local_x, base_y + i, local_z,
                                 BLOCK_CACTUS, false);
}

static bool worldgen_lava_pool_cell_allowed(const WorldgenColumn *column,
                                            int center_surface_y)
{
    int dy;

    if (!column || column->biome != WORLD_BIOME_DESERT ||
        column->underwater || column->beach ||
        column->surface_y < WORLDGEN_SEA_LEVEL + 1 ||
        column->surface_y >= WORLD_CHUNK_HEIGHT - 2)
        return false;

    dy = column->surface_y - center_surface_y;
    return dy >= -1 && dy <= 1;
}

static void worldgen_place_desert_lava_pool(Chunk *chunk,
                                            int center_local_x,
                                            int center_local_z,
                                            uint32_t base_seed)
{
    int origin_x;
    int origin_z;
    int center_wx;
    int center_wz;
    WorldgenColumn center_column;

    if (!chunk)
        return;

    origin_x = chunk->chunk_x * WORLD_CHUNK_SIZE;
    origin_z = chunk->chunk_z * WORLD_CHUNK_SIZE;
    center_wx = origin_x + center_local_x;
    center_wz = origin_z + center_local_z;
    center_column = worldgen_column_at(center_wx, center_wz, base_seed);

    for (int dz = -WORLDGEN_LAVA_POOL_RADIUS;
         dz <= WORLDGEN_LAVA_POOL_RADIUS; dz++) {
        for (int dx = -WORLDGEN_LAVA_POOL_RADIUS;
             dx <= WORLDGEN_LAVA_POOL_RADIUS; dx++) {
            int dist_sq = dx * dx + dz * dz;
            int lx = center_local_x + dx;
            int lz = center_local_z + dz;
            int wx = center_wx + dx;
            int wz = center_wz + dz;
            WorldgenColumn column;
            int surface_y;

            if (dist_sq > WORLDGEN_LAVA_POOL_RADIUS *
                          WORLDGEN_LAVA_POOL_RADIUS)
                continue;

            column = worldgen_column_at(wx, wz, base_seed);
            if (!worldgen_lava_pool_cell_allowed(&column,
                                                 center_column.surface_y))
                continue;

            surface_y = column.surface_y;
            if (dist_sq <= 4) {
                worldgen_set_block_local(chunk, lx, surface_y - 1, lz,
                                         BLOCK_SANDSTONE, true);
                worldgen_set_block_local(chunk, lx, surface_y, lz,
                                         BLOCK_LAVA, true);
                worldgen_set_block_local(chunk, lx, surface_y + 1, lz,
                                         BLOCK_AIR, true);
            } else {
                worldgen_set_block_local(chunk, lx, surface_y, lz,
                                         BLOCK_SANDSTONE, true);
                worldgen_set_block_local(chunk, lx, surface_y + 1, lz,
                                         BLOCK_AIR, true);
            }
        }
    }
}

typedef struct {
    BlockID block;
    int attempts_per_24;
    int min_y;
    int max_y;
    int radius;
    uint32_t salt;
} WorldgenOreConfig;

static int worldgen_scaled_ore_attempts(int attempts_per_24,
                                        int stone_tries_per_chunk)
{
    int density = stone_tries_per_chunk;
    int attempts;

    if (density <= 0)
        density = 24;

    attempts = (attempts_per_24 * density + 12) / 24;
    return attempts < 1 ? 1 : attempts;
}

static void worldgen_try_place_ore(Chunk *chunk,
                                   int lx, int y, int lz,
                                   BlockID block,
                                   uint32_t base_seed)
{
    int wx;
    int wz;
    WorldgenColumn column;

    if (!chunk)
        return;
    if (lx < 0 || lx >= WORLD_CHUNK_SIZE ||
        lz < 0 || lz >= WORLD_CHUNK_SIZE ||
        y < 1 || y >= WORLD_CHUNK_HEIGHT)
        return;
    if (chunk->blocks[y][lz][lx] != BLOCK_STONE)
        return;

    wx = chunk->chunk_x * WORLD_CHUNK_SIZE + lx;
    wz = chunk->chunk_z * WORLD_CHUNK_SIZE + lz;
    column = worldgen_column_at(wx, wz, base_seed);
    if (y >= column.surface_y - 2)
        return;

    chunk->blocks[y][lz][lx] = block;
}

static void worldgen_place_ore_vein(Chunk *chunk,
                                    const WorldgenOreConfig *ore,
                                    int center_x,
                                    int center_y,
                                    int center_z,
                                    uint32_t roll,
                                    uint32_t base_seed)
{
    int radius_sq;

    if (!chunk || !ore)
        return;

    radius_sq = ore->radius * ore->radius;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -ore->radius; dz <= ore->radius; dz++) {
            for (int dx = -ore->radius; dx <= ore->radius; dx++) {
                int dist_sq = dx * dx + dz * dz + dy * dy * 2;
                uint32_t mask = roll >> ((abs(dx) + abs(dz) + abs(dy)) & 15);

                if (dist_sq > radius_sq + (int)(mask & 1u))
                    continue;

                worldgen_try_place_ore(chunk,
                                       center_x + dx,
                                       center_y + dy,
                                       center_z + dz,
                                       ore->block,
                                       base_seed);
            }
        }
    }
}

static void worldgen_place_ores(Chunk *chunk,
                                uint32_t base_seed,
                                int stone_tries_per_chunk)
{
    static const WorldgenOreConfig ores[] = {
        { BLOCK_COAL_ORE,     7,  4, 25, 2, 0xc0a10u },
        { BLOCK_IRON_ORE,     6,  3, 22, 2, 0x1a0f0u },
        { BLOCK_GOLD_ORE,     3,  2, 15, 1, 0x901du },
        { BLOCK_REDSTONE_ORE, 3,  1, 12, 1, 0xed570u },
        { BLOCK_DIAMOND_ORE,  2,  1, 10, 1, 0xd1a0du },
    };
    int origin_x;
    int origin_z;

    if (!chunk)
        return;

    origin_x = chunk->chunk_x * WORLD_CHUNK_SIZE;
    origin_z = chunk->chunk_z * WORLD_CHUNK_SIZE;

    for (size_t ore_index = 0; ore_index < sizeof(ores) / sizeof(ores[0]);
         ore_index++) {
        const WorldgenOreConfig *ore = &ores[ore_index];
        int attempts = worldgen_scaled_ore_attempts(ore->attempts_per_24,
                                                   stone_tries_per_chunk);

        for (int i = 0; i < attempts; i++) {
            uint32_t roll = hash_world_coord(origin_x + i * 17,
                                             origin_z - i * 31,
                                             base_seed,
                                             ore->salt);
            int y_span = ore->max_y - ore->min_y + 1;
            int center_x = (int)(roll & 15u);
            int center_z = (int)((roll >> 4) & 15u);
            int center_y = ore->min_y + (int)((roll >> 8) % (uint32_t)y_span);

            worldgen_place_ore_vein(chunk, ore,
                                    center_x, center_y, center_z,
                                    roll, base_seed);
        }
    }
}

void world_generate_chunk_terrain(Chunk *chunk,
                                  uint32_t base_seed,
                                  int stone_tries_per_chunk,
                                  bool desert_lava_pools_enabled)
{
    memset(chunk->blocks, 0, sizeof(chunk->blocks));

    int origin_x = chunk->chunk_x * WORLD_CHUNK_SIZE;
    int origin_z = chunk->chunk_z * WORLD_CHUNK_SIZE;

    for (int lz = 0; lz < WORLD_CHUNK_SIZE; lz++) {
        for (int lx = 0; lx < WORLD_CHUNK_SIZE; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;
            WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
            int surface_y = column.surface_y;

            for (int y = 0; y <= surface_y; y++)
                chunk->blocks[y][lz][lx] = worldgen_column_block(&column, y);

            if (column.underwater) {
                for (int y = surface_y + 1; y <= WORLDGEN_SEA_LEVEL; y++)
                    chunk->blocks[y][lz][lx] = BLOCK_WATER;
            }
        }
    }

    worldgen_place_ores(chunk, base_seed, stone_tries_per_chunk);

    if (desert_lava_pools_enabled) {
        for (int lz = -WORLDGEN_LAVA_POOL_RADIUS;
             lz < WORLD_CHUNK_SIZE + WORLDGEN_LAVA_POOL_RADIUS; lz++) {
            for (int lx = -WORLDGEN_LAVA_POOL_RADIUS;
                 lx < WORLD_CHUNK_SIZE + WORLDGEN_LAVA_POOL_RADIUS; lx++) {
                int wx = origin_x + lx;
                int wz = origin_z + lz;

                if (!worldgen_wants_lava_pool(wx, wz, base_seed))
                    continue;
                worldgen_place_desert_lava_pool(chunk, lx, lz, base_seed);
            }
        }
    }

    for (int lz = -WORLDGEN_TREE_RADIUS;
         lz < WORLD_CHUNK_SIZE + WORLDGEN_TREE_RADIUS; lz++) {
        for (int lx = -WORLDGEN_TREE_RADIUS;
             lx < WORLD_CHUNK_SIZE + WORLDGEN_TREE_RADIUS; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;

            if (!worldgen_wants_tree(wx, wz, base_seed))
                continue;

            WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
            int surface_y = column.surface_y;
            if (surface_y < WORLDGEN_TREE_MIN_Y)
                continue;
            int trunk_top_y = surface_y + 4 +
                              (int)(hash_world_coord(wx, wz, base_seed,
                                                     0xfeedu) & 1u);
            if (trunk_top_y + 2 >= WORLD_CHUNK_HEIGHT)
                continue;

            uint32_t roll = hash_world_coord(wx, wz, base_seed, 0xd00du);
            worldgen_place_tree(chunk, lx, trunk_top_y, lz, roll);
        }
    }

    for (int lz = 0; lz < WORLD_CHUNK_SIZE; lz++) {
        for (int lx = 0; lx < WORLD_CHUNK_SIZE; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;
            WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
            int cactus_y = column.surface_y + 1;
            uint32_t roll;

            if (cactus_y + 3 >= WORLD_CHUNK_HEIGHT)
                continue;
            if (chunk->blocks[column.surface_y][lz][lx] != BLOCK_SAND ||
                chunk->blocks[cactus_y][lz][lx] != BLOCK_AIR)
                continue;
            if (!worldgen_wants_cactus(wx, wz, base_seed, &column))
                continue;

            roll = hash_world_coord(wx, wz, base_seed, 0xcac7u);
            worldgen_place_cactus(chunk, lx, cactus_y, lz, roll);
        }
    }

    for (int lz = 0; lz < WORLD_CHUNK_SIZE; lz++) {
        for (int lx = 0; lx < WORLD_CHUNK_SIZE; lx++) {
            int wx = origin_x + lx;
            int wz = origin_z + lz;
            WorldgenColumn column = worldgen_column_at(wx, wz, base_seed);
            int flower_y = column.surface_y + 1;

            if (column.underwater || column.beach ||
                flower_y >= WORLD_CHUNK_HEIGHT)
                continue;
            if (chunk->blocks[column.surface_y][lz][lx] != BLOCK_GRASS ||
                chunk->blocks[flower_y][lz][lx] != BLOCK_AIR)
                continue;
            if (worldgen_wants_mushroom(wx, wz, base_seed, column.biome)) {
                chunk->blocks[flower_y][lz][lx] =
                    (hash_world_coord(wx, wz, base_seed, 0x6d46u) & 1u) ?
                    BLOCK_RED_MUSHROOM : BLOCK_BROWN_MUSHROOM;
                continue;
            }
            if (!worldgen_wants_flower(wx, wz, base_seed, column.biome))
                continue;

            chunk->blocks[flower_y][lz][lx] =
                (hash_world_coord(wx, wz, base_seed, 0xf11au) & 1u) ?
                BLOCK_YELLOW_FLOWER : BLOCK_RED_FLOWER;
        }
    }
}
