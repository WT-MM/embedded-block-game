#include "inventory.h"

#include <string.h>

typedef struct {
    bool shapeless;
    uint8_t width;
    uint8_t height;
    ItemID inputs[SURVIVAL_CRAFT_SLOT_COUNT];
    ItemID output;
    uint8_t output_count;
} CraftRecipe;

typedef struct {
    const char *name;
    uint8_t texture_id;
    ItemToolKind tool_kind;
    ItemToolTier tool_tier;
    uint8_t food_units;
    bool returns_bowl;
} ItemDef;

#define ITEM_DEF(label, texture) \
    { .name = label, .texture_id = texture }
#define FOOD_DEF(label, texture, units) \
    { .name = label, .texture_id = texture, .food_units = units }
#define FOOD_BOWL_DEF(label, texture, units) \
    { .name = label, .texture_id = texture, .food_units = units, \
      .returns_bowl = true }
#define TOOL_DEF(label, texture, kind, tier) \
    { .name = label, .texture_id = texture, .tool_kind = kind, \
      .tool_tier = tier }

static const ItemDef ITEM_DEFS[NUM_ITEM_TYPES - NUM_BLOCK_TYPES] = {
    [ITEM_STICK - NUM_BLOCK_TYPES] =
        ITEM_DEF("Stick", TEX_TILE_STICK),
    [ITEM_APPLE - NUM_BLOCK_TYPES] =
        FOOD_DEF("Apple", TEX_TILE_APPLE, 4),
    [ITEM_RED_MUSHROOM - NUM_BLOCK_TYPES] =
        FOOD_DEF("Red Mushroom", TEX_TILE_RED_MUSHROOM, 2),
    [ITEM_BROWN_MUSHROOM - NUM_BLOCK_TYPES] =
        FOOD_DEF("Brown Mushroom", TEX_TILE_BROWN_MUSHROOM, 2),
    [ITEM_BOWL - NUM_BLOCK_TYPES] =
        ITEM_DEF("Bowl", TEX_TILE_BOWL),
    [ITEM_MUSHROOM_STEW - NUM_BLOCK_TYPES] =
        FOOD_BOWL_DEF("Mushroom Stew", TEX_TILE_MUSHROOM_STEW, 8),
    [ITEM_COAL - NUM_BLOCK_TYPES] =
        ITEM_DEF("Coal", TEX_TILE_COAL),
    [ITEM_IRON_INGOT - NUM_BLOCK_TYPES] =
        ITEM_DEF("Iron Ingot", TEX_TILE_IRON_INGOT),
    [ITEM_GOLD_INGOT - NUM_BLOCK_TYPES] =
        ITEM_DEF("Gold Ingot", TEX_TILE_GOLD_INGOT),
    [ITEM_DIAMOND - NUM_BLOCK_TYPES] =
        ITEM_DEF("Diamond", TEX_TILE_DIAMOND),
    [ITEM_BUCKET - NUM_BLOCK_TYPES] =
        ITEM_DEF("Bucket", TEX_TILE_BUCKET),
    [ITEM_WATER_BUCKET - NUM_BLOCK_TYPES] =
        ITEM_DEF("Water Bucket", TEX_TILE_WATER_BUCKET),
    [ITEM_LAVA_BUCKET - NUM_BLOCK_TYPES] =
        ITEM_DEF("Lava Bucket", TEX_TILE_LAVA_BUCKET),
    [ITEM_WOOD_PICKAXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Wooden Pickaxe", TEX_TILE_WOOD_PICKAXE,
                 ITEM_TOOL_PICKAXE, ITEM_TOOL_TIER_WOOD),
    [ITEM_STONE_PICKAXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Stone Pickaxe", TEX_TILE_STONE_PICKAXE,
                 ITEM_TOOL_PICKAXE, ITEM_TOOL_TIER_STONE),
    [ITEM_IRON_PICKAXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Iron Pickaxe", TEX_TILE_IRON_PICKAXE,
                 ITEM_TOOL_PICKAXE, ITEM_TOOL_TIER_IRON),
    [ITEM_GOLD_PICKAXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Gold Pickaxe", TEX_TILE_GOLD_PICKAXE,
                 ITEM_TOOL_PICKAXE, ITEM_TOOL_TIER_GOLD),
    [ITEM_DIAMOND_PICKAXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Diamond Pickaxe", TEX_TILE_DIAMOND_PICKAXE,
                 ITEM_TOOL_PICKAXE, ITEM_TOOL_TIER_DIAMOND),
    [ITEM_WOOD_AXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Wooden Axe", TEX_TILE_WOOD_AXE,
                 ITEM_TOOL_AXE, ITEM_TOOL_TIER_WOOD),
    [ITEM_STONE_AXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Stone Axe", TEX_TILE_STONE_AXE,
                 ITEM_TOOL_AXE, ITEM_TOOL_TIER_STONE),
    [ITEM_IRON_AXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Iron Axe", TEX_TILE_IRON_AXE,
                 ITEM_TOOL_AXE, ITEM_TOOL_TIER_IRON),
    [ITEM_GOLD_AXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Gold Axe", TEX_TILE_GOLD_AXE,
                 ITEM_TOOL_AXE, ITEM_TOOL_TIER_GOLD),
    [ITEM_DIAMOND_AXE - NUM_BLOCK_TYPES] =
        TOOL_DEF("Diamond Axe", TEX_TILE_DIAMOND_AXE,
                 ITEM_TOOL_AXE, ITEM_TOOL_TIER_DIAMOND),
};

#undef ITEM_DEF
#undef FOOD_DEF
#undef FOOD_BOWL_DEF
#undef TOOL_DEF

static const ItemDef *item_def(ItemID item)
{
    if (item < (ItemID)NUM_BLOCK_TYPES || item >= NUM_ITEM_TYPES)
        return NULL;
    if (!ITEM_DEFS[item - NUM_BLOCK_TYPES].name)
        return NULL;
    return &ITEM_DEFS[item - NUM_BLOCK_TYPES];
}

static const CraftRecipe CRAFT_RECIPES[] = {
    {
        .shapeless = false,
        .width = 1,
        .height = 1,
        .inputs = { (ItemID)BLOCK_WOOD, ITEM_NONE, ITEM_NONE, ITEM_NONE },
        .output = (ItemID)BLOCK_PLANKS,
        .output_count = 4,
    },
    {
        .shapeless = false,
        .width = 1,
        .height = 2,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    ITEM_NONE, ITEM_NONE },
        .output = ITEM_STICK,
        .output_count = 4,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, ITEM_NONE, ITEM_STICK, ITEM_NONE,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE },
        .output = ITEM_WOOD_PICKAXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, ITEM_STICK,
                    ITEM_NONE, ITEM_STICK },
        .output = ITEM_WOOD_AXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE, ITEM_NONE, ITEM_STICK,
                    ITEM_NONE, ITEM_NONE, ITEM_STICK, ITEM_NONE },
        .output = ITEM_STONE_PICKAXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE, ITEM_STICK,
                    ITEM_NONE, ITEM_STICK },
        .output = ITEM_STONE_AXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { ITEM_IRON_INGOT, ITEM_IRON_INGOT, ITEM_IRON_INGOT,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE },
        .output = ITEM_IRON_PICKAXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { ITEM_IRON_INGOT, ITEM_IRON_INGOT,
                    ITEM_IRON_INGOT, ITEM_STICK,
                    ITEM_NONE, ITEM_STICK },
        .output = ITEM_IRON_AXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { ITEM_GOLD_INGOT, ITEM_GOLD_INGOT, ITEM_GOLD_INGOT,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE },
        .output = ITEM_GOLD_PICKAXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { ITEM_GOLD_INGOT, ITEM_GOLD_INGOT,
                    ITEM_GOLD_INGOT, ITEM_STICK,
                    ITEM_NONE, ITEM_STICK },
        .output = ITEM_GOLD_AXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { ITEM_DIAMOND, ITEM_DIAMOND, ITEM_DIAMOND,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE,
                    ITEM_NONE, ITEM_STICK, ITEM_NONE },
        .output = ITEM_DIAMOND_PICKAXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { ITEM_DIAMOND, ITEM_DIAMOND,
                    ITEM_DIAMOND, ITEM_STICK,
                    ITEM_NONE, ITEM_STICK },
        .output = ITEM_DIAMOND_AXE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS },
        .output = (ItemID)BLOCK_CRAFTING_TABLE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 3,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    ITEM_NONE, ITEM_NONE, ITEM_NONE },
        .output = (ItemID)BLOCK_DOOR,
        .output_count = 3,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    ITEM_NONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE },
        .output = (ItemID)BLOCK_FURNACE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 1,
        .height = 2,
        .inputs = { ITEM_COAL, ITEM_STICK, ITEM_NONE, ITEM_NONE },
        .output = (ItemID)BLOCK_TORCH,
        .output_count = 4,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 2,
        .inputs = { ITEM_IRON_INGOT, ITEM_NONE, ITEM_IRON_INGOT,
                    ITEM_NONE, ITEM_IRON_INGOT, ITEM_NONE,
                    ITEM_NONE, ITEM_NONE, ITEM_NONE },
        .output = ITEM_BUCKET,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_SAND, (ItemID)BLOCK_SAND,
                    (ItemID)BLOCK_SAND, (ItemID)BLOCK_SAND },
        .output = (ItemID)BLOCK_SANDSTONE,
        .output_count = 1,
    },
    {
        .shapeless = true,
        .width = 2,
        .height = 1,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    ITEM_NONE, ITEM_NONE },
        .output = ITEM_BOWL,
        .output_count = 4,
    },
    {
        .shapeless = true,
        .width = 2,
        .height = 2,
        .inputs = { ITEM_BOWL, ITEM_RED_MUSHROOM, ITEM_BROWN_MUSHROOM,
                    ITEM_NONE },
        .output = ITEM_MUSHROOM_STEW,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_CLAY, (ItemID)BLOCK_CLAY,
                    (ItemID)BLOCK_CLAY, (ItemID)BLOCK_CLAY },
        .output = (ItemID)BLOCK_BRICKS,
        .output_count = 1,
    },
    {
        .shapeless = true,
        .width = 2,
        .height = 1,
        .inputs = { (ItemID)BLOCK_GLASS, (ItemID)BLOCK_REDSTONE_BLOCK,
                    ITEM_NONE, ITEM_NONE },
        .output = (ItemID)BLOCK_LAMP,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE,
                    (ItemID)BLOCK_COBBLESTONE, (ItemID)BLOCK_COBBLESTONE },
        .output = (ItemID)BLOCK_STONE,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { ITEM_GOLD_INGOT, ITEM_GOLD_INGOT, ITEM_GOLD_INGOT,
                    ITEM_GOLD_INGOT, ITEM_GOLD_INGOT, ITEM_GOLD_INGOT,
                    ITEM_GOLD_INGOT, ITEM_GOLD_INGOT, ITEM_GOLD_INGOT },
        .output = (ItemID)BLOCK_GOLD_BLOCK,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { ITEM_DIAMOND, ITEM_DIAMOND, ITEM_DIAMOND,
                    ITEM_DIAMOND, ITEM_DIAMOND, ITEM_DIAMOND,
                    ITEM_DIAMOND, ITEM_DIAMOND, ITEM_DIAMOND },
        .output = (ItemID)BLOCK_DIAMOND_BLOCK,
        .output_count = 1,
    },
    {
        .shapeless = false,
        .width = 3,
        .height = 3,
        .inputs = { (ItemID)BLOCK_REDSTONE_ORE, (ItemID)BLOCK_REDSTONE_ORE,
                    (ItemID)BLOCK_REDSTONE_ORE, (ItemID)BLOCK_REDSTONE_ORE,
                    (ItemID)BLOCK_REDSTONE_ORE, (ItemID)BLOCK_REDSTONE_ORE,
                    (ItemID)BLOCK_REDSTONE_ORE, (ItemID)BLOCK_REDSTONE_ORE,
                    (ItemID)BLOCK_REDSTONE_ORE },
        .output = (ItemID)BLOCK_REDSTONE_BLOCK,
        .output_count = 1,
    },
};

static bool item_id_valid(ItemID item)
{
    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return true;
    return item_def(item) != NULL;
}

void item_stack_clear(ItemStack *stack)
{
    if (!stack)
        return;

    stack->item = ITEM_NONE;
    stack->count = 0;
}

bool item_stack_is_empty(const ItemStack *stack)
{
    return !stack || stack->count == 0 || !item_id_valid(stack->item);
}

bool item_stack_can_merge(const ItemStack *stack, ItemID item)
{
    if (!item_id_valid(item))
        return false;
    if (item_stack_is_empty(stack))
        return true;

    return stack->item == item && stack->count < ITEM_STACK_MAX;
}

int item_stack_add(ItemStack *stack, ItemID item, int count)
{
    int room;
    int moved;

    if (!stack || !item_id_valid(item) || count <= 0)
        return count;
    if (!item_stack_can_merge(stack, item))
        return count;

    if (item_stack_is_empty(stack)) {
        stack->item = item;
        stack->count = 0;
    }

    room = ITEM_STACK_MAX - (int)stack->count;
    if (room <= 0)
        return count;
    moved = count < room ? count : room;
    stack->count = (uint8_t)(stack->count + moved);
    return count - moved;
}

int item_stack_remove(ItemStack *stack, int count)
{
    int removed;

    if (item_stack_is_empty(stack) || count <= 0)
        return 0;

    removed = count < (int)stack->count ? count : (int)stack->count;
    stack->count = (uint8_t)(stack->count - removed);
    if (stack->count == 0)
        item_stack_clear(stack);
    return removed;
}

const char *item_name(ItemID item)
{
    const ItemDef *def;

    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return BlockRegistry[(BlockID)item].name;

    def = item_def(item);
    if (def)
        return def->name;

    return "Unknown";
}

uint8_t item_texture_id(ItemID item)
{
    const ItemDef *def;

    if (block_is_door((BlockID)item))
        return TEX_TILE_DOOR_ITEM;
    if (item == (ItemID)BLOCK_CRAFTING_TABLE)
        return TEX_TILE_CRAFTING_TABLE_FRONT;
    if (item == (ItemID)BLOCK_FURNACE)
        return TEX_TILE_FURNACE_FRONT;
    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return block_face_texture_id((BlockID)item, FACE_FRONT);

    def = item_def(item);
    if (def)
        return def->texture_id;

    return 0;
}

bool item_is_placeable_block(ItemID item)
{
    return item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES;
}

BlockID item_place_block(ItemID item)
{
    if (!item_is_placeable_block(item))
        return BLOCK_AIR;

    return (BlockID)item;
}

bool item_is_furnace_fuel(ItemID item)
{
    return item == ITEM_COAL ||
           item == (ItemID)BLOCK_WOOD ||
           item == (ItemID)BLOCK_PLANKS ||
           item == ITEM_STICK;
}

ItemID item_furnace_smelt_output(ItemID input)
{
    if (input == (ItemID)BLOCK_SAND)
        return (ItemID)BLOCK_GLASS;
    if (input == (ItemID)BLOCK_IRON_ORE)
        return ITEM_IRON_INGOT;
    if (input == (ItemID)BLOCK_GOLD_ORE)
        return ITEM_GOLD_INGOT;
    return ITEM_NONE;
}

ItemToolKind item_tool_kind(ItemID item)
{
    const ItemDef *def = item_def(item);

    return def ? def->tool_kind : ITEM_TOOL_NONE;
}

ItemToolTier item_tool_tier(ItemID item)
{
    const ItemDef *def = item_def(item);

    return def ? def->tool_tier : ITEM_TOOL_TIER_NONE;
}

bool item_is_tool(ItemID item)
{
    return item_tool_kind(item) != ITEM_TOOL_NONE;
}

static bool block_prefers_pickaxe(BlockID block)
{
    return block == BLOCK_STONE ||
           block == BLOCK_COBBLESTONE ||
           block == BLOCK_BRICKS ||
           block == BLOCK_OBSIDIAN ||
           block == BLOCK_SANDSTONE ||
           block == BLOCK_COAL_ORE ||
           block == BLOCK_IRON_ORE ||
           block == BLOCK_GOLD_ORE ||
           block == BLOCK_DIAMOND_ORE ||
           block == BLOCK_REDSTONE_ORE ||
           block == BLOCK_GOLD_BLOCK ||
           block == BLOCK_DIAMOND_BLOCK ||
           block == BLOCK_REDSTONE_BLOCK ||
           block == BLOCK_FURNACE ||
           block == BLOCK_LAMP ||
           block == BLOCK_LAMP_OFF;
}

static bool block_prefers_axe(BlockID block)
{
    return block == BLOCK_WOOD ||
           block == BLOCK_PLANKS ||
           block == BLOCK_CRAFTING_TABLE ||
           block_is_door(block);
}

static float tool_tier_multiplier(ItemToolTier tier)
{
    switch (tier) {
    case ITEM_TOOL_TIER_WOOD:
        return 2.0f;
    case ITEM_TOOL_TIER_STONE:
        return 2.7f;
    case ITEM_TOOL_TIER_IRON:
        return 3.6f;
    case ITEM_TOOL_TIER_GOLD:
        return 4.4f;
    case ITEM_TOOL_TIER_DIAMOND:
        return 5.0f;
    default:
        return 1.0f;
    }
}

float item_break_seconds(ItemID item, BlockID block, float base_seconds)
{
    ItemToolKind kind;
    float multiplier;

    if (base_seconds <= 0.0f)
        return base_seconds;

    kind = item_tool_kind(item);
    if ((kind == ITEM_TOOL_PICKAXE && !block_prefers_pickaxe(block)) ||
        (kind == ITEM_TOOL_AXE && !block_prefers_axe(block)) ||
        kind == ITEM_TOOL_NONE)
        return base_seconds;

    multiplier = tool_tier_multiplier(item_tool_tier(item));
    if (multiplier <= 1.0f)
        return base_seconds;

    base_seconds /= multiplier;
    return base_seconds < 0.08f ? 0.08f : base_seconds;
}

int item_food_units(ItemID item)
{
    const ItemDef *def = item_def(item);

    return def ? def->food_units : 0;
}

bool item_food_returns_bowl(ItemID item)
{
    const ItemDef *def = item_def(item);

    return def && def->returns_bowl;
}

static int clamp_craft_grid_dim(int grid_dim)
{
    if (grid_dim >= SURVIVAL_CRAFT_GRID_TABLE)
        return SURVIVAL_CRAFT_GRID_TABLE;
    return SURVIVAL_CRAFT_GRID_PLAYER;
}

void survival_inventory_init(SurvivalInventory *inv)
{
    if (!inv)
        return;

    memset(inv, 0, sizeof(*inv));
    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT; i++)
        item_stack_clear(&inv->storage[i]);
    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        item_stack_clear(&inv->craft[i]);
    item_stack_clear(&inv->craft_output);
    item_stack_clear(&inv->cursor);
    inv->craft_grid_dim = SURVIVAL_CRAFT_GRID_PLAYER;
}

void survival_inventory_set_craft_grid_dim(SurvivalInventory *inv, int grid_dim)
{
    if (!inv)
        return;

    inv->craft_grid_dim = (uint8_t)clamp_craft_grid_dim(grid_dim);
    survival_inventory_refresh_craft_output(inv);
}

int survival_inventory_craft_grid_dim(const SurvivalInventory *inv)
{
    if (!inv)
        return SURVIVAL_CRAFT_GRID_PLAYER;

    return clamp_craft_grid_dim(inv->craft_grid_dim);
}

int survival_inventory_add_item(SurvivalInventory *inv, ItemID item, int count)
{
    if (!inv || !item_id_valid(item) || count <= 0)
        return count;

    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT && count > 0; i++) {
        if (!item_stack_is_empty(&inv->storage[i]) &&
            inv->storage[i].item == item)
            count = item_stack_add(&inv->storage[i], item, count);
    }

    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT && count > 0; i++) {
        if (item_stack_is_empty(&inv->storage[i]))
            count = item_stack_add(&inv->storage[i], item, count);
    }

    return count;
}

int survival_inventory_add_block(SurvivalInventory *inv, BlockID block, int count)
{
    if (block <= BLOCK_AIR || block >= NUM_BLOCK_TYPES)
        return count;

    return survival_inventory_add_item(inv, (ItemID)block, count);
}

bool survival_inventory_remove_storage(SurvivalInventory *inv, int slot, int count)
{
    if (!inv || slot < 0 || slot >= SURVIVAL_STORAGE_SLOT_COUNT || count <= 0)
        return false;
    if (item_stack_is_empty(&inv->storage[slot]) ||
        inv->storage[slot].count < count)
        return false;

    item_stack_remove(&inv->storage[slot], count);
    return true;
}

int survival_inventory_count_item(const SurvivalInventory *inv, ItemID item)
{
    int total = 0;

    if (!inv || !item_id_valid(item))
        return 0;

    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT; i++) {
        if (!item_stack_is_empty(&inv->storage[i]) &&
            inv->storage[i].item == item)
            total += inv->storage[i].count;
    }

    return total;
}

bool survival_inventory_remove_item(SurvivalInventory *inv, ItemID item, int count)
{
    if (!inv || !item_id_valid(item) || count <= 0)
        return false;
    if (survival_inventory_count_item(inv, item) < count)
        return false;

    for (int i = 0; i < SURVIVAL_STORAGE_SLOT_COUNT && count > 0; i++) {
        ItemStack *stack = &inv->storage[i];
        int removed;

        if (item_stack_is_empty(stack) || stack->item != item)
            continue;

        removed = item_stack_remove(stack, count);
        count -= removed;
    }

    return true;
}

static bool craft_recipe_matches_at(const SurvivalInventory *inv,
                                    const CraftRecipe *recipe,
                                    int grid_dim,
                                    int offset_x,
                                    int offset_y)
{
    for (int y = 0; y < grid_dim; y++) {
        for (int x = 0; x < grid_dim; x++) {
            int grid_index = y * SURVIVAL_CRAFT_GRID_TABLE + x;
            bool inside =
                x >= offset_x && x < offset_x + recipe->width &&
                y >= offset_y && y < offset_y + recipe->height;
            ItemID want = ITEM_NONE;
            const ItemStack *have = &inv->craft[grid_index];

            if (inside) {
                int rx = x - offset_x;
                int ry = y - offset_y;
                want = recipe->inputs[ry * recipe->width + rx];
            }

            if (want == ITEM_NONE) {
                if (!item_stack_is_empty(have))
                    return false;
            } else if (item_stack_is_empty(have) || have->item != want) {
                return false;
            }
        }
    }

    return true;
}

static bool craft_recipe_matches_shapeless(const SurvivalInventory *inv,
                                           const CraftRecipe *recipe,
                                           int grid_dim)
{
    bool used[SURVIVAL_CRAFT_SLOT_COUNT] = { false };
    int want_count = 0;
    int have_count = 0;

    if (!inv || !recipe)
        return false;

    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++) {
        if (recipe->inputs[i] != ITEM_NONE)
            want_count++;
    }

    for (int y = 0; y < grid_dim; y++) {
        for (int x = 0; x < grid_dim; x++) {
            int index = y * SURVIVAL_CRAFT_GRID_TABLE + x;

            if (!item_stack_is_empty(&inv->craft[index]))
                have_count++;
        }
    }
    if (want_count != have_count)
        return false;

    for (int want = 0; want < SURVIVAL_CRAFT_SLOT_COUNT; want++) {
        ItemID item = recipe->inputs[want];
        bool found = false;

        if (item == ITEM_NONE)
            continue;

        for (int y = 0; y < grid_dim && !found; y++) {
            for (int x = 0; x < grid_dim; x++) {
                int have = y * SURVIVAL_CRAFT_GRID_TABLE + x;

                if (used[have] ||
                    item_stack_is_empty(&inv->craft[have]) ||
                    inv->craft[have].item != item)
                    continue;

                used[have] = true;
                found = true;
                break;
            }
        }

        if (!found)
            return false;
    }

    return true;
}

static bool craft_recipe_matches(const SurvivalInventory *inv,
                                 const CraftRecipe *recipe)
{
    int grid_dim;

    if (!inv || !recipe || recipe->width == 0 || recipe->height == 0 ||
        recipe->width > SURVIVAL_CRAFT_GRID_TABLE ||
        recipe->height > SURVIVAL_CRAFT_GRID_TABLE)
        return false;

    grid_dim = survival_inventory_craft_grid_dim(inv);
    if (recipe->width > grid_dim || recipe->height > grid_dim)
        return false;

    if (recipe->shapeless)
        return craft_recipe_matches_shapeless(inv, recipe, grid_dim);

    for (int y = 0; y <= grid_dim - (int)recipe->height; y++) {
        for (int x = 0; x <= grid_dim - (int)recipe->width; x++) {
            if (craft_recipe_matches_at(inv, recipe, grid_dim, x, y))
                return true;
        }
    }

    return false;
}

void survival_inventory_refresh_craft_output(SurvivalInventory *inv)
{
    if (!inv)
        return;

    item_stack_clear(&inv->craft_output);
    for (size_t i = 0; i < sizeof(CRAFT_RECIPES) / sizeof(CRAFT_RECIPES[0]); i++) {
        const CraftRecipe *recipe = &CRAFT_RECIPES[i];

        if (!craft_recipe_matches(inv, recipe))
            continue;

        inv->craft_output.item = recipe->output;
        inv->craft_output.count = recipe->output_count;
        return;
    }
}

static ItemStack *slot_for_area(SurvivalInventory *inv,
                                InventorySlotArea area,
                                int index)
{
    if (!inv)
        return NULL;

    switch (area) {
    case INVENTORY_SLOT_STORAGE:
        if (index >= 0 && index < SURVIVAL_STORAGE_SLOT_COUNT)
            return &inv->storage[index];
        return NULL;
    case INVENTORY_SLOT_CRAFT:
        if (index >= 0 && index < SURVIVAL_CRAFT_SLOT_COUNT)
            return &inv->craft[index];
        return NULL;
    default:
        return NULL;
    }
}

static bool cursor_accepts_stack(const ItemStack *cursor, const ItemStack *stack)
{
    if (item_stack_is_empty(stack))
        return false;
    if (item_stack_is_empty(cursor))
        return true;
    if (cursor->item != stack->item)
        return false;

    return (int)cursor->count + (int)stack->count <= ITEM_STACK_MAX;
}

static bool take_craft_output(SurvivalInventory *inv)
{
    ItemStack output;
    int grid_dim = survival_inventory_craft_grid_dim(inv);

    survival_inventory_refresh_craft_output(inv);
    output = inv->craft_output;
    if (!cursor_accepts_stack(&inv->cursor, &output))
        return false;

    if (item_stack_is_empty(&inv->cursor))
        inv->cursor = output;
    else
        inv->cursor.count = (uint8_t)(inv->cursor.count + output.count);

    for (int y = 0; y < grid_dim; y++) {
        for (int x = 0; x < grid_dim; x++) {
            int i = y * SURVIVAL_CRAFT_GRID_TABLE + x;

            if (!item_stack_is_empty(&inv->craft[i]))
                item_stack_remove(&inv->craft[i], 1);
        }
    }

    survival_inventory_refresh_craft_output(inv);
    return true;
}

static bool click_stack_left(SurvivalInventory *inv, ItemStack *slot)
{
    if (item_stack_is_empty(&inv->cursor)) {
        if (item_stack_is_empty(slot))
            return false;

        inv->cursor = *slot;
        item_stack_clear(slot);
        return true;
    }

    if (item_stack_is_empty(slot)) {
        *slot = inv->cursor;
        item_stack_clear(&inv->cursor);
        return true;
    }

    if (slot->item == inv->cursor.item && slot->count < ITEM_STACK_MAX) {
        int remaining = item_stack_add(slot, inv->cursor.item, inv->cursor.count);

        inv->cursor.count = (uint8_t)remaining;
        if (remaining == 0)
            item_stack_clear(&inv->cursor);
        return true;
    }

    {
        ItemStack tmp = *slot;

        *slot = inv->cursor;
        inv->cursor = tmp;
    }
    return true;
}

static bool click_stack_right(SurvivalInventory *inv, ItemStack *slot)
{
    if (item_stack_is_empty(&inv->cursor)) {
        int take;

        if (item_stack_is_empty(slot))
            return false;

        take = ((int)slot->count + 1) / 2;
        inv->cursor.item = slot->item;
        inv->cursor.count = 0;
        item_stack_add(&inv->cursor, slot->item, take);
        item_stack_remove(slot, take);
        return true;
    }

    if (item_stack_is_empty(slot)) {
        slot->item = inv->cursor.item;
        slot->count = 1;
        item_stack_remove(&inv->cursor, 1);
        return true;
    }

    if (slot->item == inv->cursor.item && slot->count < ITEM_STACK_MAX) {
        item_stack_add(slot, inv->cursor.item, 1);
        item_stack_remove(&inv->cursor, 1);
        return true;
    }

    {
        ItemStack tmp = *slot;

        *slot = inv->cursor;
        inv->cursor = tmp;
    }
    return true;
}

bool survival_inventory_click(SurvivalInventory *inv,
                              InventorySlotArea area,
                              int index,
                              bool right_click)
{
    bool changed = false;
    ItemStack *slot;

    if (!inv)
        return false;

    survival_inventory_refresh_craft_output(inv);
    if (area == INVENTORY_SLOT_OUTPUT)
        return take_craft_output(inv);

    slot = slot_for_area(inv, area, index);
    if (!slot)
        return false;

    changed = right_click ? click_stack_right(inv, slot) :
                            click_stack_left(inv, slot);
    if (area == INVENTORY_SLOT_CRAFT)
        survival_inventory_refresh_craft_output(inv);
    return changed;
}

const ItemStack *survival_inventory_hotbar_stack(const SurvivalInventory *inv,
                                                 int slot)
{
    if (!inv || slot < 0 || slot >= SURVIVAL_HOTBAR_SLOT_COUNT)
        return NULL;

    return &inv->storage[slot];
}

int survival_craft_recipe_count(void)
{
    return (int)(sizeof(CRAFT_RECIPES) / sizeof(CRAFT_RECIPES[0]));
}

static bool recipe_fits_grid(const CraftRecipe *recipe, int grid_dim)
{
    if (!recipe)
        return false;

    grid_dim = clamp_craft_grid_dim(grid_dim);
    return recipe->width <= grid_dim && recipe->height <= grid_dim;
}

bool survival_craft_recipe_view(int index, CraftRecipeView *view)
{
    const CraftRecipe *recipe;

    if (!view || index < 0 || index >= survival_craft_recipe_count())
        return false;

    recipe = &CRAFT_RECIPES[index];
    view->shapeless = recipe->shapeless;
    view->width = recipe->width;
    view->height = recipe->height;
    memcpy(view->inputs, recipe->inputs, sizeof(view->inputs));
    view->output = recipe->output;
    view->output_count = recipe->output_count;
    return true;
}

int survival_craft_recipe_count_for_grid(int grid_dim)
{
    int count = 0;

    for (int i = 0; i < survival_craft_recipe_count(); i++) {
        if (recipe_fits_grid(&CRAFT_RECIPES[i], grid_dim))
            count++;
    }

    return count;
}

bool survival_craft_recipe_view_for_grid(int index, int grid_dim,
                                         CraftRecipeView *view)
{
    if (!view || index < 0)
        return false;

    for (int i = 0; i < survival_craft_recipe_count(); i++) {
        if (!recipe_fits_grid(&CRAFT_RECIPES[i], grid_dim))
            continue;
        if (index == 0)
            return survival_craft_recipe_view(i, view);
        index--;
    }

    return false;
}

ItemID survival_drop_for_block(BlockID block)
{
    switch (block) {
    case BLOCK_AIR:
    case BLOCK_WATER:
    case BLOCK_WATER_FLOW:
    case BLOCK_LAVA:
    case BLOCK_LAVA_FLOW:
        return ITEM_NONE;
    case BLOCK_GRASS:
        return (ItemID)BLOCK_DIRT;
    case BLOCK_STONE:
        return (ItemID)BLOCK_COBBLESTONE;
    case BLOCK_COAL_ORE:
        return ITEM_COAL;
    case BLOCK_DIAMOND_ORE:
        return ITEM_DIAMOND;
    case BLOCK_LEAVES:
        return ITEM_NONE;
    case BLOCK_BUTTON_PRESSED:
        return (ItemID)BLOCK_BUTTON;
    case BLOCK_RED_MUSHROOM:
        return ITEM_RED_MUSHROOM;
    case BLOCK_BROWN_MUSHROOM:
        return ITEM_BROWN_MUSHROOM;
    default:
        if (block_is_door(block))
            return (ItemID)BLOCK_DOOR;
        if (block > BLOCK_AIR && block < NUM_BLOCK_TYPES)
            return (ItemID)block;
        return ITEM_NONE;
    }
}
