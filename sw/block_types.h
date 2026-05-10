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
    BLOCK_CACTUS,
    BLOCK_RED_MUSHROOM,
    BLOCK_BROWN_MUSHROOM,
    BLOCK_FURNACE,
    BLOCK_TORCH,
    BLOCK_DOOR_EAST,
    BLOCK_DOOR_SOUTH,
    BLOCK_DOOR_WEST,
    BLOCK_DOOR_NORTH_UPPER,
    BLOCK_DOOR_EAST_UPPER,
    BLOCK_DOOR_SOUTH_UPPER,
    BLOCK_DOOR_WEST_UPPER,
    BLOCK_DOOR_NORTH_OPEN,
    BLOCK_DOOR_EAST_OPEN,
    BLOCK_DOOR_SOUTH_OPEN,
    BLOCK_DOOR_WEST_OPEN,
    BLOCK_DOOR_NORTH_OPEN_UPPER,
    BLOCK_DOOR_EAST_OPEN_UPPER,
    BLOCK_DOOR_SOUTH_OPEN_UPPER,
    BLOCK_DOOR_WEST_OPEN_UPPER,
    BLOCK_REDSTONE_WIRE_UNCONNECTED,
    BLOCK_REDSTONE_WIRE_OFF,
    BLOCK_REDSTONE_WIRE_ON,
    BLOCK_REDSTONE_TORCH_OFF,
    BLOCK_REDSTONE_TORCH_ON,
    BLOCK_REPEATER_OFF,
    BLOCK_REPEATER_ON,
    BLOCK_LAMP_OFF,
    BLOCK_BUTTON,
    BLOCK_REPEATER_EAST_OFF,
    BLOCK_REPEATER_SOUTH_OFF,
    BLOCK_REPEATER_WEST_OFF,
    BLOCK_REPEATER_EAST_ON,
    BLOCK_REPEATER_SOUTH_ON,
    BLOCK_REPEATER_WEST_ON,
    BLOCK_COMPARATOR_OFF,
    BLOCK_COMPARATOR_EAST_OFF,
    BLOCK_COMPARATOR_SOUTH_OFF,
    BLOCK_COMPARATOR_WEST_OFF,
    BLOCK_COMPARATOR_ON,
    BLOCK_COMPARATOR_EAST_ON,
    BLOCK_COMPARATOR_SOUTH_ON,
    BLOCK_COMPARATOR_WEST_ON,
    BLOCK_LEVER_OFF,
    BLOCK_LEVER_ON,
    BLOCK_BUTTON_PRESSED,
    BLOCK_SUGAR_CANE,
    BLOCK_WOOD_PRESSURE_PLATE,
    BLOCK_WOOD_PRESSURE_PLATE_PRESSED,
    BLOCK_STONE_PRESSURE_PLATE,
    BLOCK_STONE_PRESSURE_PLATE_PRESSED,
    NUM_BLOCK_TYPES
} BlockID;

typedef enum {
    BLOCK_RENDER_CUBE = 0,
    BLOCK_RENDER_CROSS,
    BLOCK_RENDER_TORCH,
    BLOCK_RENDER_DOOR,
    BLOCK_RENDER_FLAT,
} BlockRenderModel;

typedef enum {
    BLOCK_DOOR_FACING_NORTH = 0,
    BLOCK_DOOR_FACING_EAST,
    BLOCK_DOOR_FACING_SOUTH,
    BLOCK_DOOR_FACING_WEST,
} BlockDoorFacing;

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
/* Shared cube-face occlusion rule for renderer and chunk meshing. */
bool block_face_should_render(BlockID current, BlockID neighbor);
BlockRenderModel block_render_model(BlockID id);
bool block_is_door(BlockID id);
bool block_is_door_upper(BlockID id);
bool block_is_door_open(BlockID id);
BlockDoorFacing block_door_facing(BlockID id);
BlockID block_door_make(BlockDoorFacing facing, bool open, bool upper);
BlockID block_door_toggle(BlockID id);
bool block_is_repeater(BlockID id);
bool block_is_comparator(BlockID id);
bool block_is_lever(BlockID id);
bool block_is_pressure_plate(BlockID id);
bool block_is_wood_pressure_plate(BlockID id);
bool block_is_stone_pressure_plate(BlockID id);
bool block_is_redstone_directional(BlockID id);
bool block_redstone_directional_powered(BlockID id);
bool block_lever_powered(BlockID id);
bool block_pressure_plate_powered(BlockID id);
BlockID block_pressure_plate_unpressed(BlockID id);
BlockDoorFacing block_redstone_facing(BlockID id);
BlockID block_repeater_make(BlockDoorFacing facing, bool powered);
BlockID block_comparator_make(BlockDoorFacing facing, bool powered);
BlockID block_lever_make(bool powered);
BlockID block_pressure_plate_make(BlockID id, bool powered);

#endif
