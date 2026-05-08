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
    NUM_BLOCK_TYPES
} BlockID;

typedef enum {
    TEX_TILE_GRASS_TOP = 0,
    TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT,
    TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP,
    TEX_TILE_GLASS = 6,
    TEX_TILE_LAMP = 7,
    TEX_TILE_LEAVES = 8,
    TEX_TILE_WOOD_PLANK = 9,
    TEX_TILE_GRASS_TOP_MIP1 = 16,
    TEX_TILE_GRASS_SIDE_MIP1,
    TEX_TILE_DIRT_MIP1,
    TEX_TILE_STONE_MIP1,
    TEX_TILE_WOOD_SIDE_MIP1,
    TEX_TILE_WOOD_TOP_MIP1,
    TEX_TILE_GLASS_MIP1 = 22,
    TEX_TILE_LAMP_MIP1 = 23,
    TEX_TILE_GRASS_TOP_MIP2 = 24,
    TEX_TILE_GRASS_SIDE_MIP2,
    TEX_TILE_DIRT_MIP2,
    TEX_TILE_STONE_MIP2,
    TEX_TILE_WOOD_SIDE_MIP2,
    TEX_TILE_WOOD_TOP_MIP2,
    TEX_TILE_GLASS_MIP2 = 30,
    TEX_TILE_LAMP_MIP2 = 31,
    TEX_TILE_LEAVES_MIP1 = 32,
    TEX_TILE_LEAVES_MIP2 = 33,
    TEX_TILE_WOOD_PLANK_MIP1 = 34,
    TEX_TILE_WOOD_PLANK_MIP2 = 35,
    TEX_TILE_SKY = 48,
    TEX_TILE_CLOUD = 49,
    TEX_TILE_SUN = 50,
    TEX_TILE_MOON = 51,
    TEX_TILE_STARS = 52,
    TEX_TILE_CROSSHAIR = 63,
    NUM_TEXTURE_TILES = 64
} TextureTileID;

typedef struct {
    BlockID id;
    const char *name;
    uint8_t face_texture_ids[NUM_FACES];
    uint8_t emission_level;
    bool self_lit;
} BlockDescriptor;

extern BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

void init_block_types(void);
uint8_t block_face_texture_id(BlockID id, BlockFace face);
uint8_t texture_lod_tile_id(uint8_t tile_id, int lod);
uint8_t block_emission_level(BlockID id);
bool block_is_self_lit(BlockID id);
bool block_blocks_light(BlockID id);

/* True for blocks that don't fully occlude their neighbors (air, glass). */
bool block_is_transparent(BlockID id);
/* True for alpha-blended blocks that need special draw treatment (glass). */
bool block_is_translucent(BlockID id);

#endif
