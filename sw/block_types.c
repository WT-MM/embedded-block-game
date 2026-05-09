#include "block_types.h"

#include <string.h>

BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

static void set_all_faces(BlockDescriptor *block, uint8_t tile_id)
{
    for (int i = 0; i < NUM_FACES; i++)
        block->face_texture_ids[i] = tile_id;
}

typedef struct {
    uint8_t base;
    uint8_t mip1;
    uint8_t mip2;
} TextureLodEntry;

static const TextureLodEntry TEXTURE_LOD_ENTRIES[] = {
#define TEXTURE_TILE(name, base)
#define TEXTURE_TILE_MIPPED(name, base, mip1, mip2) { base, mip1, mip2 },
#include "texture_tiles.def"
#undef TEXTURE_TILE
#undef TEXTURE_TILE_MIPPED
};

void init_block_types(void)
{
    memset(BlockRegistry, 0, sizeof(BlockRegistry));

    BlockRegistry[BLOCK_AIR].id = BLOCK_AIR;
    BlockRegistry[BLOCK_AIR].name = "Air";

    BlockRegistry[BLOCK_DIRT].id = BLOCK_DIRT;
    BlockRegistry[BLOCK_DIRT].name = "Dirt";
    set_all_faces(&BlockRegistry[BLOCK_DIRT], TEX_TILE_DIRT);

    BlockRegistry[BLOCK_WOOD].id = BLOCK_WOOD;
    BlockRegistry[BLOCK_WOOD].name = "Wood";
    set_all_faces(&BlockRegistry[BLOCK_WOOD], TEX_TILE_WOOD_SIDE);
    BlockRegistry[BLOCK_WOOD].face_texture_ids[FACE_TOP] = TEX_TILE_WOOD_TOP;
    BlockRegistry[BLOCK_WOOD].face_texture_ids[FACE_BOTTOM] = TEX_TILE_WOOD_TOP;

    BlockRegistry[BLOCK_STONE].id = BLOCK_STONE;
    BlockRegistry[BLOCK_STONE].name = "Stone";
    set_all_faces(&BlockRegistry[BLOCK_STONE], TEX_TILE_STONE);

    BlockRegistry[BLOCK_GRASS].id = BLOCK_GRASS;
    BlockRegistry[BLOCK_GRASS].name = "Grass";
    set_all_faces(&BlockRegistry[BLOCK_GRASS], TEX_TILE_GRASS_SIDE);
    BlockRegistry[BLOCK_GRASS].face_texture_ids[FACE_TOP] = TEX_TILE_GRASS_TOP;
    BlockRegistry[BLOCK_GRASS].face_texture_ids[FACE_BOTTOM] = TEX_TILE_DIRT;

    BlockRegistry[BLOCK_GLASS].id = BLOCK_GLASS;
    BlockRegistry[BLOCK_GLASS].name = "Glass";
    set_all_faces(&BlockRegistry[BLOCK_GLASS], TEX_TILE_GLASS);
    BlockRegistry[BLOCK_GLASS].self_lit = false;

    BlockRegistry[BLOCK_LAMP].id = BLOCK_LAMP;
    BlockRegistry[BLOCK_LAMP].name = "Lamp";
    set_all_faces(&BlockRegistry[BLOCK_LAMP], TEX_TILE_LAMP);
    BlockRegistry[BLOCK_LAMP].emission_level = 15;
    BlockRegistry[BLOCK_LAMP].self_lit = true;

    BlockRegistry[BLOCK_PLANKS].id = BLOCK_PLANKS;
    BlockRegistry[BLOCK_PLANKS].name = "Planks";
    set_all_faces(&BlockRegistry[BLOCK_PLANKS], TEX_TILE_WOOD_PLANK);

    BlockRegistry[BLOCK_LEAVES].id = BLOCK_LEAVES;
    BlockRegistry[BLOCK_LEAVES].name = "Leaves";
    set_all_faces(&BlockRegistry[BLOCK_LEAVES], TEX_TILE_LEAVES);
    BlockRegistry[BLOCK_LEAVES].self_lit = false;

    BlockRegistry[BLOCK_WATER].id = BLOCK_WATER;
    BlockRegistry[BLOCK_WATER].name = "Water";
    set_all_faces(&BlockRegistry[BLOCK_WATER], TEX_TILE_WATER);
    BlockRegistry[BLOCK_WATER].self_lit = false;
}

uint8_t block_face_texture_id(BlockID id, BlockFace face)
{
    if (id <= BLOCK_AIR || id >= NUM_BLOCK_TYPES)
        return 0;
    if (face < 0 || face >= NUM_FACES)
        return 0;

    return BlockRegistry[id].face_texture_ids[face];
}

uint8_t texture_lod_tile_id(uint8_t tile_id, int lod)
{
    if (lod <= 0)
        return tile_id;

    for (size_t i = 0; i < sizeof(TEXTURE_LOD_ENTRIES) / sizeof(TEXTURE_LOD_ENTRIES[0]); i++) {
        if (TEXTURE_LOD_ENTRIES[i].base == tile_id)
            return lod >= 2 ? TEXTURE_LOD_ENTRIES[i].mip2 : TEXTURE_LOD_ENTRIES[i].mip1;
    }

    return tile_id;
}

uint8_t block_emission_level(BlockID id)
{
    if (id < BLOCK_AIR || id >= NUM_BLOCK_TYPES)
        return 0;

    return BlockRegistry[id].emission_level;
}

bool block_is_self_lit(BlockID id)
{
    if (id < BLOCK_AIR || id >= NUM_BLOCK_TYPES)
        return false;

    return BlockRegistry[id].self_lit;
}

bool block_blocks_light(BlockID id)
{
    return id != BLOCK_AIR && !block_is_transparent(id);
}

bool block_is_transparent(BlockID id)
{
    return id == BLOCK_AIR || id == BLOCK_GLASS || id == BLOCK_LEAVES ||
           id == BLOCK_WATER;
}

bool block_is_translucent(BlockID id)
{
    return id == BLOCK_GLASS || id == BLOCK_WATER;
}

bool block_is_alpha_keyed(BlockID id)
{
    return id == BLOCK_LEAVES;
}

bool block_is_passable(BlockID id)
{
    return id == BLOCK_AIR || id == BLOCK_WATER;
}
