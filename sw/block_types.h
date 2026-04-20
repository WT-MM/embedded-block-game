#ifndef BLOCK_TYPES_H
#define BLOCK_TYPES_H

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
    NUM_BLOCK_TYPES
} BlockID;

typedef enum {
    TEX_TILE_GRASS_TOP = 0,
    TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT,
    TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP,
    TEX_TILE_GRASS_TOP_MIP1 = 16,
    TEX_TILE_GRASS_SIDE_MIP1,
    TEX_TILE_DIRT_MIP1,
    TEX_TILE_STONE_MIP1,
    TEX_TILE_WOOD_SIDE_MIP1,
    TEX_TILE_WOOD_TOP_MIP1,
    TEX_TILE_GRASS_TOP_MIP2 = 24,
    TEX_TILE_GRASS_SIDE_MIP2,
    TEX_TILE_DIRT_MIP2,
    TEX_TILE_STONE_MIP2,
    TEX_TILE_WOOD_SIDE_MIP2,
    TEX_TILE_WOOD_TOP_MIP2,
    TEX_TILE_CROSSHAIR = 63,
    NUM_TEXTURE_TILES = 64
} TextureTileID;

typedef struct {
    BlockID id;
    const char *name;
    uint8_t face_texture_ids[NUM_FACES];
} BlockDescriptor;

extern BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

void init_block_types(void);
uint8_t block_face_texture_id(BlockID id, BlockFace face);
uint8_t texture_lod_tile_id(uint8_t tile_id, int lod);

#endif
