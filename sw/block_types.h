#ifndef BLOCK_TYPES_H
#define BLOCK_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FACE_TOP = 0,
    FACE_BOTTOM,
    FACE_LEFT,
    FACE_RIGHT,
    FACE_FRONT,
    FACE_BACK,
    NUM_FACES
} BlockFace;

typedef enum {
    BLOCK_AIR = 0,
    BLOCK_GRASS,
    BLOCK_WOOD,
    BLOCK_DIRT,
    BLOCK_STONE,
    BLOCK_GLASS,
    BLOCK_LAMP,
    BLOCK_PLANKS,
    BLOCK_LEAVES,
    BLOCK_WATER,
    BLOCK_WATER_FLOW,  /* auto-spread flowing water; evaporates when source removed */
    BLOCK_SAND,
    BLOCK_GRAVEL,
    BLOCK_COBBLESTONE,
    BLOCK_BRICKS,
    BLOCK_OBSIDIAN,
    BLOCK_SANDSTONE,
    BLOCK_CLAY,
    BLOCK_REDSTONE_BLOCK,
    BLOCK_LAVA,
    BLOCK_COAL_ORE,
    BLOCK_IRON_ORE,
    BLOCK_GOLD_ORE,
    BLOCK_DIAMOND_ORE,
    BLOCK_REDSTONE_ORE,
    BLOCK_GOLD_BLOCK,
    BLOCK_DIAMOND_BLOCK,
    BLOCK_RED_FLOWER,
    BLOCK_YELLOW_FLOWER,
    BLOCK_LAVA_FLOW,   /* auto-spread flowing lava; hidden from the hotbar */
    BLOCK_CRAFTING_TABLE,
    BLOCK_DOOR,
    NUM_BLOCK_TYPES
} BlockID;

typedef enum {
    BLOCK_RENDER_CUBE = 0,
    BLOCK_RENDER_CROSS,
} BlockRenderModel;

typedef enum {
#define TEXTURE_TILE(name, base) TEX_TILE_##name = base,
#define TEXTURE_TILE_MIPPED(name, base, mip1, mip2) \
    TEX_TILE_##name = base,                         \
    TEX_TILE_##name##_MIP1 = mip1,                  \
    TEX_TILE_##name##_MIP2 = mip2,
#include "texture_tiles.def"
#undef TEXTURE_TILE
#undef TEXTURE_TILE_MIPPED
    NUM_TEXTURE_TILES = 128
} TextureTileID;

typedef struct {
    BlockID id;
    const char *name;
    uint8_t face_texture_ids[NUM_FACES];
    uint8_t emission_level;
    float hardness_seconds;
    bool self_lit;
    BlockRenderModel render_model;
} BlockDescriptor;

extern BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

void init_block_types(void);
uint8_t block_face_texture_id(BlockID id, BlockFace face);
uint8_t texture_lod_tile_id(uint8_t tile_id, int lod);
uint8_t block_emission_level(BlockID id);
float block_break_seconds(BlockID id);
bool block_is_self_lit(BlockID id);
bool block_blocks_light(BlockID id);

/* True for blocks that don't fully occlude their neighbors (air, glass). */
bool block_is_transparent(BlockID id);
/* True for alpha-blended blocks that need special draw treatment (glass). */
bool block_is_translucent(BlockID id);
/* True for cutout blocks rendered with palette-0 alpha-key (leaves). */
bool block_is_alpha_keyed(BlockID id);
/* True for blocks the player walks through without colliding (water). */
bool block_is_passable(BlockID id);
BlockRenderModel block_render_model(BlockID id);

#endif
