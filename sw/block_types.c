#include "block_types.h"

#include <string.h>

BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

static void set_all_faces(BlockDescriptor *block, uint8_t tile_id)
{
    for (int i = 0; i < NUM_FACES; i++)
        block->face_texture_ids[i] = tile_id;
}

static void init_door_descriptor(BlockID id, bool upper)
{
    BlockRegistry[id].id = id;
    BlockRegistry[id].name = "Door";
    set_all_faces(&BlockRegistry[id],
                  upper ? TEX_TILE_DOOR_UPPER : TEX_TILE_DOOR_LOWER);
    BlockRegistry[id].hardness_seconds = 0.9f;
    BlockRegistry[id].render_model = BLOCK_RENDER_DOOR;
}

static void init_redstone_flat_descriptor(BlockID id,
                                          const char *name,
                                          uint8_t texture,
                                          bool powered)
{
    BlockRegistry[id].id = id;
    BlockRegistry[id].name = name;
    set_all_faces(&BlockRegistry[id], texture);
    BlockRegistry[id].hardness_seconds = 0.2f;
    BlockRegistry[id].render_model = BLOCK_RENDER_FLAT;
    if (powered) {
        BlockRegistry[id].emission_level = 5;
        BlockRegistry[id].self_lit = true;
    }
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
    BlockRegistry[BLOCK_AIR].hardness_seconds = 0.0f;

    BlockRegistry[BLOCK_DIRT].id = BLOCK_DIRT;
    BlockRegistry[BLOCK_DIRT].name = "Dirt";
    set_all_faces(&BlockRegistry[BLOCK_DIRT], TEX_TILE_DIRT);
    BlockRegistry[BLOCK_DIRT].hardness_seconds = 0.55f;

    BlockRegistry[BLOCK_WOOD].id = BLOCK_WOOD;
    BlockRegistry[BLOCK_WOOD].name = "Wood";
    set_all_faces(&BlockRegistry[BLOCK_WOOD], TEX_TILE_WOOD_SIDE);
    BlockRegistry[BLOCK_WOOD].face_texture_ids[FACE_TOP] = TEX_TILE_WOOD_TOP;
    BlockRegistry[BLOCK_WOOD].face_texture_ids[FACE_BOTTOM] = TEX_TILE_WOOD_TOP;
    BlockRegistry[BLOCK_WOOD].hardness_seconds = 1.35f;

    BlockRegistry[BLOCK_STONE].id = BLOCK_STONE;
    BlockRegistry[BLOCK_STONE].name = "Stone";
    set_all_faces(&BlockRegistry[BLOCK_STONE], TEX_TILE_STONE);
    BlockRegistry[BLOCK_STONE].hardness_seconds = 2.25f;

    BlockRegistry[BLOCK_GRASS].id = BLOCK_GRASS;
    BlockRegistry[BLOCK_GRASS].name = "Grass";
    set_all_faces(&BlockRegistry[BLOCK_GRASS], TEX_TILE_GRASS_SIDE);
    BlockRegistry[BLOCK_GRASS].face_texture_ids[FACE_TOP] = TEX_TILE_GRASS_TOP;
    BlockRegistry[BLOCK_GRASS].face_texture_ids[FACE_BOTTOM] = TEX_TILE_DIRT;
    BlockRegistry[BLOCK_GRASS].hardness_seconds = 0.6f;

    BlockRegistry[BLOCK_GLASS].id = BLOCK_GLASS;
    BlockRegistry[BLOCK_GLASS].name = "Glass";
    set_all_faces(&BlockRegistry[BLOCK_GLASS], TEX_TILE_GLASS);
    BlockRegistry[BLOCK_GLASS].hardness_seconds = 0.35f;
    BlockRegistry[BLOCK_GLASS].self_lit = false;

    BlockRegistry[BLOCK_LAMP].id = BLOCK_LAMP;
    BlockRegistry[BLOCK_LAMP].name = "Lamp On";
    set_all_faces(&BlockRegistry[BLOCK_LAMP], TEX_TILE_LAMP);
    BlockRegistry[BLOCK_LAMP].emission_level = 15;
    BlockRegistry[BLOCK_LAMP].hardness_seconds = 0.8f;
    BlockRegistry[BLOCK_LAMP].self_lit = true;

    BlockRegistry[BLOCK_PLANKS].id = BLOCK_PLANKS;
    BlockRegistry[BLOCK_PLANKS].name = "Planks";
    set_all_faces(&BlockRegistry[BLOCK_PLANKS], TEX_TILE_WOOD_PLANK);
    BlockRegistry[BLOCK_PLANKS].hardness_seconds = 1.05f;

    BlockRegistry[BLOCK_LEAVES].id = BLOCK_LEAVES;
    BlockRegistry[BLOCK_LEAVES].name = "Leaves";
    set_all_faces(&BlockRegistry[BLOCK_LEAVES], TEX_TILE_LEAVES);
    BlockRegistry[BLOCK_LEAVES].hardness_seconds = 0.25f;
    BlockRegistry[BLOCK_LEAVES].self_lit = false;

    BlockRegistry[BLOCK_WATER].id = BLOCK_WATER;
    BlockRegistry[BLOCK_WATER].name = "Water";
    set_all_faces(&BlockRegistry[BLOCK_WATER], TEX_TILE_WATER);
    BlockRegistry[BLOCK_WATER].hardness_seconds = 0.0f;
    BlockRegistry[BLOCK_WATER].self_lit = false;

    BlockRegistry[BLOCK_WATER_FLOW].id = BLOCK_WATER_FLOW;
    BlockRegistry[BLOCK_WATER_FLOW].name = "Water (flowing)";
    set_all_faces(&BlockRegistry[BLOCK_WATER_FLOW], TEX_TILE_WATER);
    BlockRegistry[BLOCK_WATER_FLOW].hardness_seconds = 0.0f;
    BlockRegistry[BLOCK_WATER_FLOW].self_lit = false;

    BlockRegistry[BLOCK_SAND].id = BLOCK_SAND;
    BlockRegistry[BLOCK_SAND].name = "Sand";
    set_all_faces(&BlockRegistry[BLOCK_SAND], TEX_TILE_SAND);
    BlockRegistry[BLOCK_SAND].hardness_seconds = 0.5f;

    BlockRegistry[BLOCK_GRAVEL].id = BLOCK_GRAVEL;
    BlockRegistry[BLOCK_GRAVEL].name = "Gravel";
    set_all_faces(&BlockRegistry[BLOCK_GRAVEL], TEX_TILE_GRAVEL);
    BlockRegistry[BLOCK_GRAVEL].hardness_seconds = 0.6f;

    BlockRegistry[BLOCK_COBBLESTONE].id = BLOCK_COBBLESTONE;
    BlockRegistry[BLOCK_COBBLESTONE].name = "Cobblestone";
    set_all_faces(&BlockRegistry[BLOCK_COBBLESTONE], TEX_TILE_COBBLESTONE);
    BlockRegistry[BLOCK_COBBLESTONE].hardness_seconds = 2.0f;

    BlockRegistry[BLOCK_BRICKS].id = BLOCK_BRICKS;
    BlockRegistry[BLOCK_BRICKS].name = "Bricks";
    set_all_faces(&BlockRegistry[BLOCK_BRICKS], TEX_TILE_BRICKS);
    BlockRegistry[BLOCK_BRICKS].hardness_seconds = 2.0f;

    BlockRegistry[BLOCK_OBSIDIAN].id = BLOCK_OBSIDIAN;
    BlockRegistry[BLOCK_OBSIDIAN].name = "Obsidian";
    set_all_faces(&BlockRegistry[BLOCK_OBSIDIAN], TEX_TILE_OBSIDIAN);
    BlockRegistry[BLOCK_OBSIDIAN].hardness_seconds = 8.0f;

    BlockRegistry[BLOCK_SANDSTONE].id = BLOCK_SANDSTONE;
    BlockRegistry[BLOCK_SANDSTONE].name = "Sandstone";
    set_all_faces(&BlockRegistry[BLOCK_SANDSTONE], TEX_TILE_SANDSTONE);
    BlockRegistry[BLOCK_SANDSTONE].hardness_seconds = 0.8f;

    BlockRegistry[BLOCK_CLAY].id = BLOCK_CLAY;
    BlockRegistry[BLOCK_CLAY].name = "Clay";
    set_all_faces(&BlockRegistry[BLOCK_CLAY], TEX_TILE_CLAY);
    BlockRegistry[BLOCK_CLAY].hardness_seconds = 0.7f;

    BlockRegistry[BLOCK_REDSTONE_BLOCK].id = BLOCK_REDSTONE_BLOCK;
    BlockRegistry[BLOCK_REDSTONE_BLOCK].name = "Redstone Block";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_BLOCK], TEX_TILE_REDSTONE_BLOCK);
    BlockRegistry[BLOCK_REDSTONE_BLOCK].hardness_seconds = 2.0f;

    BlockRegistry[BLOCK_LAVA].id = BLOCK_LAVA;
    BlockRegistry[BLOCK_LAVA].name = "Lava";
    set_all_faces(&BlockRegistry[BLOCK_LAVA], TEX_TILE_LAVA);
    BlockRegistry[BLOCK_LAVA].emission_level = 15;
    BlockRegistry[BLOCK_LAVA].hardness_seconds = 0.0f;
    BlockRegistry[BLOCK_LAVA].self_lit = true;

    BlockRegistry[BLOCK_LAVA_FLOW].id = BLOCK_LAVA_FLOW;
    BlockRegistry[BLOCK_LAVA_FLOW].name = "Lava (flowing)";
    set_all_faces(&BlockRegistry[BLOCK_LAVA_FLOW], TEX_TILE_LAVA);
    BlockRegistry[BLOCK_LAVA_FLOW].emission_level = 15;
    BlockRegistry[BLOCK_LAVA_FLOW].hardness_seconds = 0.0f;
    BlockRegistry[BLOCK_LAVA_FLOW].self_lit = true;

    BlockRegistry[BLOCK_COAL_ORE].id = BLOCK_COAL_ORE;
    BlockRegistry[BLOCK_COAL_ORE].name = "Coal Ore";
    set_all_faces(&BlockRegistry[BLOCK_COAL_ORE], TEX_TILE_COAL_ORE);
    BlockRegistry[BLOCK_COAL_ORE].hardness_seconds = 2.4f;

    BlockRegistry[BLOCK_IRON_ORE].id = BLOCK_IRON_ORE;
    BlockRegistry[BLOCK_IRON_ORE].name = "Iron Ore";
    set_all_faces(&BlockRegistry[BLOCK_IRON_ORE], TEX_TILE_IRON_ORE);
    BlockRegistry[BLOCK_IRON_ORE].hardness_seconds = 2.4f;

    BlockRegistry[BLOCK_GOLD_ORE].id = BLOCK_GOLD_ORE;
    BlockRegistry[BLOCK_GOLD_ORE].name = "Gold Ore";
    set_all_faces(&BlockRegistry[BLOCK_GOLD_ORE], TEX_TILE_GOLD_ORE);
    BlockRegistry[BLOCK_GOLD_ORE].hardness_seconds = 2.4f;

    BlockRegistry[BLOCK_DIAMOND_ORE].id = BLOCK_DIAMOND_ORE;
    BlockRegistry[BLOCK_DIAMOND_ORE].name = "Diamond Ore";
    set_all_faces(&BlockRegistry[BLOCK_DIAMOND_ORE], TEX_TILE_DIAMOND_ORE);
    BlockRegistry[BLOCK_DIAMOND_ORE].hardness_seconds = 2.6f;

    BlockRegistry[BLOCK_REDSTONE_ORE].id = BLOCK_REDSTONE_ORE;
    BlockRegistry[BLOCK_REDSTONE_ORE].name = "Redstone Ore";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_ORE], TEX_TILE_REDSTONE_ORE);
    BlockRegistry[BLOCK_REDSTONE_ORE].hardness_seconds = 2.4f;

    BlockRegistry[BLOCK_GOLD_BLOCK].id = BLOCK_GOLD_BLOCK;
    BlockRegistry[BLOCK_GOLD_BLOCK].name = "Gold Block";
    set_all_faces(&BlockRegistry[BLOCK_GOLD_BLOCK], TEX_TILE_GOLD_BLOCK);
    BlockRegistry[BLOCK_GOLD_BLOCK].hardness_seconds = 2.8f;

    BlockRegistry[BLOCK_DIAMOND_BLOCK].id = BLOCK_DIAMOND_BLOCK;
    BlockRegistry[BLOCK_DIAMOND_BLOCK].name = "Diamond Block";
    set_all_faces(&BlockRegistry[BLOCK_DIAMOND_BLOCK], TEX_TILE_DIAMOND_BLOCK);
    BlockRegistry[BLOCK_DIAMOND_BLOCK].hardness_seconds = 2.8f;

    BlockRegistry[BLOCK_RED_FLOWER].id = BLOCK_RED_FLOWER;
    BlockRegistry[BLOCK_RED_FLOWER].name = "Red Flower";
    set_all_faces(&BlockRegistry[BLOCK_RED_FLOWER], TEX_TILE_RED_FLOWER);
    BlockRegistry[BLOCK_RED_FLOWER].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_RED_FLOWER].render_model = BLOCK_RENDER_CROSS;

    BlockRegistry[BLOCK_YELLOW_FLOWER].id = BLOCK_YELLOW_FLOWER;
    BlockRegistry[BLOCK_YELLOW_FLOWER].name = "Yellow Flower";
    set_all_faces(&BlockRegistry[BLOCK_YELLOW_FLOWER], TEX_TILE_YELLOW_FLOWER);
    BlockRegistry[BLOCK_YELLOW_FLOWER].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_YELLOW_FLOWER].render_model = BLOCK_RENDER_CROSS;

    BlockRegistry[BLOCK_CRAFTING_TABLE].id = BLOCK_CRAFTING_TABLE;
    BlockRegistry[BLOCK_CRAFTING_TABLE].name = "Crafting Table";
    set_all_faces(&BlockRegistry[BLOCK_CRAFTING_TABLE],
                  TEX_TILE_CRAFTING_TABLE_SIDE);
    BlockRegistry[BLOCK_CRAFTING_TABLE].face_texture_ids[FACE_TOP] =
        TEX_TILE_CRAFTING_TABLE_TOP;
    BlockRegistry[BLOCK_CRAFTING_TABLE].face_texture_ids[FACE_BOTTOM] =
        TEX_TILE_WOOD_PLANK;
    BlockRegistry[BLOCK_CRAFTING_TABLE].face_texture_ids[FACE_FRONT] =
        TEX_TILE_CRAFTING_TABLE_FRONT;
    BlockRegistry[BLOCK_CRAFTING_TABLE].hardness_seconds = 1.05f;

    BlockRegistry[BLOCK_DOOR].id = BLOCK_DOOR;
    BlockRegistry[BLOCK_DOOR].name = "Door";
    init_door_descriptor(BLOCK_DOOR, false);

    BlockRegistry[BLOCK_CACTUS].id = BLOCK_CACTUS;
    BlockRegistry[BLOCK_CACTUS].name = "Cactus";
    set_all_faces(&BlockRegistry[BLOCK_CACTUS], TEX_TILE_CACTUS_SIDE);
    BlockRegistry[BLOCK_CACTUS].face_texture_ids[FACE_TOP] = TEX_TILE_CACTUS_TOP;
    BlockRegistry[BLOCK_CACTUS].face_texture_ids[FACE_BOTTOM] = TEX_TILE_CACTUS_BOTTOM;
    BlockRegistry[BLOCK_CACTUS].hardness_seconds = 0.45f;

    BlockRegistry[BLOCK_RED_MUSHROOM].id = BLOCK_RED_MUSHROOM;
    BlockRegistry[BLOCK_RED_MUSHROOM].name = "Red Mushroom";
    set_all_faces(&BlockRegistry[BLOCK_RED_MUSHROOM], TEX_TILE_RED_MUSHROOM);
    BlockRegistry[BLOCK_RED_MUSHROOM].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_RED_MUSHROOM].render_model = BLOCK_RENDER_CROSS;

    BlockRegistry[BLOCK_BROWN_MUSHROOM].id = BLOCK_BROWN_MUSHROOM;
    BlockRegistry[BLOCK_BROWN_MUSHROOM].name = "Brown Mushroom";
    set_all_faces(&BlockRegistry[BLOCK_BROWN_MUSHROOM], TEX_TILE_BROWN_MUSHROOM);
    BlockRegistry[BLOCK_BROWN_MUSHROOM].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_BROWN_MUSHROOM].render_model = BLOCK_RENDER_CROSS;

    BlockRegistry[BLOCK_FURNACE].id = BLOCK_FURNACE;
    BlockRegistry[BLOCK_FURNACE].name = "Furnace";
    set_all_faces(&BlockRegistry[BLOCK_FURNACE], TEX_TILE_FURNACE_SIDE);
    BlockRegistry[BLOCK_FURNACE].face_texture_ids[FACE_TOP] = TEX_TILE_FURNACE_TOP;
    BlockRegistry[BLOCK_FURNACE].face_texture_ids[FACE_BOTTOM] = TEX_TILE_FURNACE_TOP;
    BlockRegistry[BLOCK_FURNACE].face_texture_ids[FACE_FRONT] = TEX_TILE_FURNACE_FRONT;
    BlockRegistry[BLOCK_FURNACE].hardness_seconds = 2.0f;

    BlockRegistry[BLOCK_TORCH].id = BLOCK_TORCH;
    BlockRegistry[BLOCK_TORCH].name = "Torch";
    set_all_faces(&BlockRegistry[BLOCK_TORCH], TEX_TILE_TORCH);
    BlockRegistry[BLOCK_TORCH].emission_level = 14;
    BlockRegistry[BLOCK_TORCH].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_TORCH].self_lit = true;
    BlockRegistry[BLOCK_TORCH].render_model = BLOCK_RENDER_TORCH;

    init_door_descriptor(BLOCK_DOOR_EAST, false);
    init_door_descriptor(BLOCK_DOOR_SOUTH, false);
    init_door_descriptor(BLOCK_DOOR_WEST, false);
    init_door_descriptor(BLOCK_DOOR_NORTH_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_EAST_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_SOUTH_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_WEST_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_NORTH_OPEN, false);
    init_door_descriptor(BLOCK_DOOR_EAST_OPEN, false);
    init_door_descriptor(BLOCK_DOOR_SOUTH_OPEN, false);
    init_door_descriptor(BLOCK_DOOR_WEST_OPEN, false);
    init_door_descriptor(BLOCK_DOOR_NORTH_OPEN_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_EAST_OPEN_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_SOUTH_OPEN_UPPER, true);
    init_door_descriptor(BLOCK_DOOR_WEST_OPEN_UPPER, true);

    BlockRegistry[BLOCK_REDSTONE_WIRE_UNCONNECTED].id =
        BLOCK_REDSTONE_WIRE_UNCONNECTED;
    BlockRegistry[BLOCK_REDSTONE_WIRE_UNCONNECTED].name =
        "Redstone Dust";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_WIRE_UNCONNECTED],
                  TEX_TILE_REDSTONE_WIRE_UNCONNECTED);
    BlockRegistry[BLOCK_REDSTONE_WIRE_UNCONNECTED].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_REDSTONE_WIRE_UNCONNECTED].render_model =
        BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_REDSTONE_WIRE_OFF].id = BLOCK_REDSTONE_WIRE_OFF;
    BlockRegistry[BLOCK_REDSTONE_WIRE_OFF].name = "Redstone Wire Off";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_WIRE_OFF],
                  TEX_TILE_REDSTONE_WIRE_OFF);
    BlockRegistry[BLOCK_REDSTONE_WIRE_OFF].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_REDSTONE_WIRE_OFF].render_model = BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].id = BLOCK_REDSTONE_WIRE_ON;
    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].name = "Redstone Wire On";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_WIRE_ON],
                  TEX_TILE_REDSTONE_WIRE_ON);
    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].render_model = BLOCK_RENDER_FLAT;
    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].emission_level = 5;
    BlockRegistry[BLOCK_REDSTONE_WIRE_ON].self_lit = true;

    BlockRegistry[BLOCK_REDSTONE_TORCH_OFF].id = BLOCK_REDSTONE_TORCH_OFF;
    BlockRegistry[BLOCK_REDSTONE_TORCH_OFF].name = "Redstone Torch Off";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_TORCH_OFF],
                  TEX_TILE_REDSTONE_TORCH_OFF);
    BlockRegistry[BLOCK_REDSTONE_TORCH_OFF].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_REDSTONE_TORCH_OFF].render_model = BLOCK_RENDER_TORCH;

    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].id = BLOCK_REDSTONE_TORCH_ON;
    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].name = "Redstone Torch On";
    set_all_faces(&BlockRegistry[BLOCK_REDSTONE_TORCH_ON],
                  TEX_TILE_REDSTONE_TORCH_ON);
    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].emission_level = 7;
    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].self_lit = true;
    BlockRegistry[BLOCK_REDSTONE_TORCH_ON].render_model = BLOCK_RENDER_TORCH;

    init_redstone_flat_descriptor(BLOCK_REPEATER_OFF, "Repeater Off",
                                  TEX_TILE_REPEATER_OFF, false);
    init_redstone_flat_descriptor(BLOCK_REPEATER_ON, "Repeater On",
                                  TEX_TILE_REPEATER_ON, true);

    BlockRegistry[BLOCK_LAMP_OFF].id = BLOCK_LAMP_OFF;
    BlockRegistry[BLOCK_LAMP_OFF].name = "Lamp Off";
    set_all_faces(&BlockRegistry[BLOCK_LAMP_OFF], TEX_TILE_LAMP_OFF);
    BlockRegistry[BLOCK_LAMP_OFF].hardness_seconds = 0.8f;

    BlockRegistry[BLOCK_BUTTON].id = BLOCK_BUTTON;
    BlockRegistry[BLOCK_BUTTON].name = "Button";
    set_all_faces(&BlockRegistry[BLOCK_BUTTON], TEX_TILE_BUTTON);
    BlockRegistry[BLOCK_BUTTON].hardness_seconds = 0.2f;
    BlockRegistry[BLOCK_BUTTON].render_model = BLOCK_RENDER_FLAT;

    init_redstone_flat_descriptor(BLOCK_REPEATER_EAST_OFF,
                                  "Repeater East Off",
                                  TEX_TILE_REPEATER_OFF, false);
    init_redstone_flat_descriptor(BLOCK_REPEATER_SOUTH_OFF,
                                  "Repeater South Off",
                                  TEX_TILE_REPEATER_OFF, false);
    init_redstone_flat_descriptor(BLOCK_REPEATER_WEST_OFF,
                                  "Repeater West Off",
                                  TEX_TILE_REPEATER_OFF, false);
    init_redstone_flat_descriptor(BLOCK_REPEATER_EAST_ON,
                                  "Repeater East On",
                                  TEX_TILE_REPEATER_ON, true);
    init_redstone_flat_descriptor(BLOCK_REPEATER_SOUTH_ON,
                                  "Repeater South On",
                                  TEX_TILE_REPEATER_ON, true);
    init_redstone_flat_descriptor(BLOCK_REPEATER_WEST_ON,
                                  "Repeater West On",
                                  TEX_TILE_REPEATER_ON, true);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_OFF,
                                  "Comparator Off",
                                  TEX_TILE_COMPARATOR_OFF, false);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_EAST_OFF,
                                  "Comparator East Off",
                                  TEX_TILE_COMPARATOR_OFF, false);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_SOUTH_OFF,
                                  "Comparator South Off",
                                  TEX_TILE_COMPARATOR_OFF, false);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_WEST_OFF,
                                  "Comparator West Off",
                                  TEX_TILE_COMPARATOR_OFF, false);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_ON,
                                  "Comparator On",
                                  TEX_TILE_COMPARATOR_ON, true);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_EAST_ON,
                                  "Comparator East On",
                                  TEX_TILE_COMPARATOR_ON, true);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_SOUTH_ON,
                                  "Comparator South On",
                                  TEX_TILE_COMPARATOR_ON, true);
    init_redstone_flat_descriptor(BLOCK_COMPARATOR_WEST_ON,
                                  "Comparator West On",
                                  TEX_TILE_COMPARATOR_ON, true);

    init_redstone_flat_descriptor(BLOCK_LEVER_OFF, "Lever Off",
                                  TEX_TILE_LEVER_OFF, false);
    init_redstone_flat_descriptor(BLOCK_LEVER_ON, "Lever On",
                                  TEX_TILE_LEVER_ON, true);

    BlockRegistry[BLOCK_BUTTON_PRESSED].id = BLOCK_BUTTON_PRESSED;
    BlockRegistry[BLOCK_BUTTON_PRESSED].name = "Button Pressed";
    set_all_faces(&BlockRegistry[BLOCK_BUTTON_PRESSED], TEX_TILE_BUTTON);
    BlockRegistry[BLOCK_BUTTON_PRESSED].hardness_seconds = 0.2f;
    BlockRegistry[BLOCK_BUTTON_PRESSED].render_model = BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_SUGAR_CANE].id = BLOCK_SUGAR_CANE;
    BlockRegistry[BLOCK_SUGAR_CANE].name = "Sugar Cane";
    set_all_faces(&BlockRegistry[BLOCK_SUGAR_CANE], TEX_TILE_SUGAR_CANE);
    BlockRegistry[BLOCK_SUGAR_CANE].hardness_seconds = 0.1f;
    BlockRegistry[BLOCK_SUGAR_CANE].render_model = BLOCK_RENDER_CROSS;

    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE].id = BLOCK_WOOD_PRESSURE_PLATE;
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE].name = "Wood Pressure Plate";
    set_all_faces(&BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE],
                  TEX_TILE_WOOD_PLANK);
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE].hardness_seconds = 0.25f;
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE].render_model = BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE_PRESSED].id =
        BLOCK_WOOD_PRESSURE_PLATE_PRESSED;
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE_PRESSED].name =
        "Wood Pressure Plate Pressed";
    set_all_faces(&BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE_PRESSED],
                  TEX_TILE_WOOD_PLANK);
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE_PRESSED].hardness_seconds = 0.25f;
    BlockRegistry[BLOCK_WOOD_PRESSURE_PLATE_PRESSED].render_model =
        BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE].id = BLOCK_STONE_PRESSURE_PLATE;
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE].name = "Stone Pressure Plate";
    set_all_faces(&BlockRegistry[BLOCK_STONE_PRESSURE_PLATE], TEX_TILE_STONE);
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE].hardness_seconds = 0.25f;
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE].render_model = BLOCK_RENDER_FLAT;

    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE_PRESSED].id =
        BLOCK_STONE_PRESSURE_PLATE_PRESSED;
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE_PRESSED].name =
        "Stone Pressure Plate Pressed";
    set_all_faces(&BlockRegistry[BLOCK_STONE_PRESSURE_PLATE_PRESSED],
                  TEX_TILE_STONE);
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE_PRESSED].hardness_seconds = 0.25f;
    BlockRegistry[BLOCK_STONE_PRESSURE_PLATE_PRESSED].render_model =
        BLOCK_RENDER_FLAT;

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

float block_break_seconds(BlockID id)
{
    if (id <= BLOCK_AIR || id >= NUM_BLOCK_TYPES)
        return 0.0f;

    return BlockRegistry[id].hardness_seconds;
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
           id == BLOCK_WATER || id == BLOCK_WATER_FLOW ||
           id == BLOCK_LAVA || id == BLOCK_LAVA_FLOW ||
           block_is_door(id) ||
           block_render_model(id) == BLOCK_RENDER_CROSS ||
           block_render_model(id) == BLOCK_RENDER_TORCH ||
           block_render_model(id) == BLOCK_RENDER_FLAT;
}

bool block_is_translucent(BlockID id)
{
    return id == BLOCK_GLASS || id == BLOCK_WATER || id == BLOCK_WATER_FLOW ||
           id == BLOCK_LAVA || id == BLOCK_LAVA_FLOW;
}

bool block_is_alpha_keyed(BlockID id)
{
    return id == BLOCK_LEAVES || block_is_door(id) ||
           block_render_model(id) == BLOCK_RENDER_CROSS ||
           block_render_model(id) == BLOCK_RENDER_TORCH ||
           block_render_model(id) == BLOCK_RENDER_FLAT;
}

bool block_is_passable(BlockID id)
{
    return id == BLOCK_AIR || id == BLOCK_WATER || id == BLOCK_WATER_FLOW ||
           id == BLOCK_LAVA || id == BLOCK_LAVA_FLOW ||
           block_is_door_open(id) ||
           block_render_model(id) == BLOCK_RENDER_CROSS ||
           block_render_model(id) == BLOCK_RENDER_TORCH ||
           block_render_model(id) == BLOCK_RENDER_FLAT;
}

bool block_face_should_render(BlockID current, BlockID neighbor)
{
    if (neighbor == BLOCK_AIR)
        return true;
    if (block_is_translucent(current))
        return false;
    if (current == neighbor && block_is_alpha_keyed(current))
        return false;
    return block_is_transparent(neighbor);
}

BlockRenderModel block_render_model(BlockID id)
{
    if (id < BLOCK_AIR || id >= NUM_BLOCK_TYPES)
        return BLOCK_RENDER_CUBE;

    return BlockRegistry[id].render_model;
}

bool block_is_door(BlockID id)
{
    return id == BLOCK_DOOR ||
           (id >= BLOCK_DOOR_EAST && id <= BLOCK_DOOR_WEST_OPEN_UPPER);
}

bool block_is_door_upper(BlockID id)
{
    return id == BLOCK_DOOR_NORTH_UPPER ||
           id == BLOCK_DOOR_EAST_UPPER ||
           id == BLOCK_DOOR_SOUTH_UPPER ||
           id == BLOCK_DOOR_WEST_UPPER ||
           id == BLOCK_DOOR_NORTH_OPEN_UPPER ||
           id == BLOCK_DOOR_EAST_OPEN_UPPER ||
           id == BLOCK_DOOR_SOUTH_OPEN_UPPER ||
           id == BLOCK_DOOR_WEST_OPEN_UPPER;
}

bool block_is_door_open(BlockID id)
{
    return id >= BLOCK_DOOR_NORTH_OPEN && id <= BLOCK_DOOR_WEST_OPEN_UPPER;
}

BlockDoorFacing block_door_facing(BlockID id)
{
    switch (id) {
    case BLOCK_DOOR:
    case BLOCK_DOOR_NORTH_UPPER:
    case BLOCK_DOOR_NORTH_OPEN:
    case BLOCK_DOOR_NORTH_OPEN_UPPER:
        return BLOCK_DOOR_FACING_NORTH;
    case BLOCK_DOOR_EAST:
    case BLOCK_DOOR_EAST_UPPER:
    case BLOCK_DOOR_EAST_OPEN:
    case BLOCK_DOOR_EAST_OPEN_UPPER:
        return BLOCK_DOOR_FACING_EAST;
    case BLOCK_DOOR_SOUTH:
    case BLOCK_DOOR_SOUTH_UPPER:
    case BLOCK_DOOR_SOUTH_OPEN:
    case BLOCK_DOOR_SOUTH_OPEN_UPPER:
        return BLOCK_DOOR_FACING_SOUTH;
    case BLOCK_DOOR_WEST:
    case BLOCK_DOOR_WEST_UPPER:
    case BLOCK_DOOR_WEST_OPEN:
    case BLOCK_DOOR_WEST_OPEN_UPPER:
        return BLOCK_DOOR_FACING_WEST;
    default:
        return BLOCK_DOOR_FACING_NORTH;
    }
}

BlockID block_door_make(BlockDoorFacing facing, bool open, bool upper)
{
    if (!open) {
        switch (facing) {
        case BLOCK_DOOR_FACING_EAST:
            return upper ? BLOCK_DOOR_EAST_UPPER : BLOCK_DOOR_EAST;
        case BLOCK_DOOR_FACING_SOUTH:
            return upper ? BLOCK_DOOR_SOUTH_UPPER : BLOCK_DOOR_SOUTH;
        case BLOCK_DOOR_FACING_WEST:
            return upper ? BLOCK_DOOR_WEST_UPPER : BLOCK_DOOR_WEST;
        case BLOCK_DOOR_FACING_NORTH:
        default:
            return upper ? BLOCK_DOOR_NORTH_UPPER : BLOCK_DOOR;
        }
    }

    switch (facing) {
    case BLOCK_DOOR_FACING_EAST:
        return upper ? BLOCK_DOOR_EAST_OPEN_UPPER : BLOCK_DOOR_EAST_OPEN;
    case BLOCK_DOOR_FACING_SOUTH:
        return upper ? BLOCK_DOOR_SOUTH_OPEN_UPPER : BLOCK_DOOR_SOUTH_OPEN;
    case BLOCK_DOOR_FACING_WEST:
        return upper ? BLOCK_DOOR_WEST_OPEN_UPPER : BLOCK_DOOR_WEST_OPEN;
    case BLOCK_DOOR_FACING_NORTH:
    default:
        return upper ? BLOCK_DOOR_NORTH_OPEN_UPPER : BLOCK_DOOR_NORTH_OPEN;
    }
}

BlockID block_door_toggle(BlockID id)
{
    if (!block_is_door(id))
        return id;

    return block_door_make(block_door_facing(id),
                           !block_is_door_open(id),
                           block_is_door_upper(id));
}

bool block_is_repeater(BlockID id)
{
    return id == BLOCK_REPEATER_OFF ||
           id == BLOCK_REPEATER_ON ||
           id == BLOCK_REPEATER_EAST_OFF ||
           id == BLOCK_REPEATER_SOUTH_OFF ||
           id == BLOCK_REPEATER_WEST_OFF ||
           id == BLOCK_REPEATER_EAST_ON ||
           id == BLOCK_REPEATER_SOUTH_ON ||
           id == BLOCK_REPEATER_WEST_ON;
}

bool block_is_comparator(BlockID id)
{
    return id == BLOCK_COMPARATOR_OFF ||
           id == BLOCK_COMPARATOR_EAST_OFF ||
           id == BLOCK_COMPARATOR_SOUTH_OFF ||
           id == BLOCK_COMPARATOR_WEST_OFF ||
           id == BLOCK_COMPARATOR_ON ||
           id == BLOCK_COMPARATOR_EAST_ON ||
           id == BLOCK_COMPARATOR_SOUTH_ON ||
           id == BLOCK_COMPARATOR_WEST_ON;
}

bool block_is_lever(BlockID id)
{
    return id == BLOCK_LEVER_OFF ||
           id == BLOCK_LEVER_ON;
}

bool block_is_pressure_plate(BlockID id)
{
    return block_is_wood_pressure_plate(id) ||
           block_is_stone_pressure_plate(id);
}

bool block_is_wood_pressure_plate(BlockID id)
{
    return id == BLOCK_WOOD_PRESSURE_PLATE ||
           id == BLOCK_WOOD_PRESSURE_PLATE_PRESSED;
}

bool block_is_stone_pressure_plate(BlockID id)
{
    return id == BLOCK_STONE_PRESSURE_PLATE ||
           id == BLOCK_STONE_PRESSURE_PLATE_PRESSED;
}

bool block_is_redstone_directional(BlockID id)
{
    return block_is_repeater(id) || block_is_comparator(id);
}

bool block_redstone_directional_powered(BlockID id)
{
    return id == BLOCK_REPEATER_ON ||
           id == BLOCK_REPEATER_EAST_ON ||
           id == BLOCK_REPEATER_SOUTH_ON ||
           id == BLOCK_REPEATER_WEST_ON ||
           id == BLOCK_COMPARATOR_ON ||
           id == BLOCK_COMPARATOR_EAST_ON ||
           id == BLOCK_COMPARATOR_SOUTH_ON ||
           id == BLOCK_COMPARATOR_WEST_ON;
}

bool block_lever_powered(BlockID id)
{
    return id == BLOCK_LEVER_ON;
}

bool block_pressure_plate_powered(BlockID id)
{
    return id == BLOCK_WOOD_PRESSURE_PLATE_PRESSED ||
           id == BLOCK_STONE_PRESSURE_PLATE_PRESSED;
}

BlockID block_pressure_plate_unpressed(BlockID id)
{
    if (block_is_stone_pressure_plate(id))
        return BLOCK_STONE_PRESSURE_PLATE;
    return BLOCK_WOOD_PRESSURE_PLATE;
}

BlockDoorFacing block_redstone_facing(BlockID id)
{
    switch (id) {
    case BLOCK_REPEATER_EAST_OFF:
    case BLOCK_REPEATER_EAST_ON:
    case BLOCK_COMPARATOR_EAST_OFF:
    case BLOCK_COMPARATOR_EAST_ON:
        return BLOCK_DOOR_FACING_EAST;
    case BLOCK_REPEATER_SOUTH_OFF:
    case BLOCK_REPEATER_SOUTH_ON:
    case BLOCK_COMPARATOR_SOUTH_OFF:
    case BLOCK_COMPARATOR_SOUTH_ON:
        return BLOCK_DOOR_FACING_SOUTH;
    case BLOCK_REPEATER_WEST_OFF:
    case BLOCK_REPEATER_WEST_ON:
    case BLOCK_COMPARATOR_WEST_OFF:
    case BLOCK_COMPARATOR_WEST_ON:
        return BLOCK_DOOR_FACING_WEST;
    case BLOCK_REPEATER_OFF:
    case BLOCK_REPEATER_ON:
    case BLOCK_COMPARATOR_OFF:
    case BLOCK_COMPARATOR_ON:
    default:
        return BLOCK_DOOR_FACING_NORTH;
    }
}

BlockID block_repeater_make(BlockDoorFacing facing, bool powered)
{
    if (powered) {
        switch (facing) {
        case BLOCK_DOOR_FACING_EAST:
            return BLOCK_REPEATER_EAST_ON;
        case BLOCK_DOOR_FACING_SOUTH:
            return BLOCK_REPEATER_SOUTH_ON;
        case BLOCK_DOOR_FACING_WEST:
            return BLOCK_REPEATER_WEST_ON;
        case BLOCK_DOOR_FACING_NORTH:
        default:
            return BLOCK_REPEATER_ON;
        }
    }

    switch (facing) {
    case BLOCK_DOOR_FACING_EAST:
        return BLOCK_REPEATER_EAST_OFF;
    case BLOCK_DOOR_FACING_SOUTH:
        return BLOCK_REPEATER_SOUTH_OFF;
    case BLOCK_DOOR_FACING_WEST:
        return BLOCK_REPEATER_WEST_OFF;
    case BLOCK_DOOR_FACING_NORTH:
    default:
        return BLOCK_REPEATER_OFF;
    }
}

BlockID block_comparator_make(BlockDoorFacing facing, bool powered)
{
    if (powered) {
        switch (facing) {
        case BLOCK_DOOR_FACING_EAST:
            return BLOCK_COMPARATOR_EAST_ON;
        case BLOCK_DOOR_FACING_SOUTH:
            return BLOCK_COMPARATOR_SOUTH_ON;
        case BLOCK_DOOR_FACING_WEST:
            return BLOCK_COMPARATOR_WEST_ON;
        case BLOCK_DOOR_FACING_NORTH:
        default:
            return BLOCK_COMPARATOR_ON;
        }
    }

    switch (facing) {
    case BLOCK_DOOR_FACING_EAST:
        return BLOCK_COMPARATOR_EAST_OFF;
    case BLOCK_DOOR_FACING_SOUTH:
        return BLOCK_COMPARATOR_SOUTH_OFF;
    case BLOCK_DOOR_FACING_WEST:
        return BLOCK_COMPARATOR_WEST_OFF;
    case BLOCK_DOOR_FACING_NORTH:
    default:
        return BLOCK_COMPARATOR_OFF;
    }
}

BlockID block_lever_make(bool powered)
{
    return powered ? BLOCK_LEVER_ON : BLOCK_LEVER_OFF;
}

BlockID block_pressure_plate_make(BlockID id, bool powered)
{
    if (block_is_stone_pressure_plate(id))
        return powered ? BLOCK_STONE_PRESSURE_PLATE_PRESSED :
                         BLOCK_STONE_PRESSURE_PLATE;
    return powered ? BLOCK_WOOD_PRESSURE_PLATE_PRESSED :
                     BLOCK_WOOD_PRESSURE_PLATE;
}
