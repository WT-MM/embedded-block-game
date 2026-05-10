#include "inventory.h"
#include "block_types.h"

#include <assert.h>
#include <stdio.h>

static void test_pickup_stacks_into_inventory(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    assert(survival_inventory_add_block(&inv, BLOCK_WOOD, 70) == 0);
    assert(inv.storage[0].item == (ItemID)BLOCK_WOOD);
    assert(inv.storage[0].count == 64);
    assert(inv.storage[1].item == (ItemID)BLOCK_WOOD);
    assert(inv.storage[1].count == 6);
}

static void test_wood_to_planks_recipe(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_WOOD, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_PLANKS);
    assert(inv.craft_output.count == 4);

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_OUTPUT, 0, false));
    assert(inv.cursor.item == (ItemID)BLOCK_PLANKS);
    assert(inv.cursor.count == 4);
    assert(item_stack_is_empty(&inv.craft[0]));
    assert(item_stack_is_empty(&inv.craft_output));
}

static void test_planks_to_sticks_recipe(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[3] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_STICK);
    assert(inv.craft_output.count == 4);
}

static void test_pressure_plate_and_bowl_recipes(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[1] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_WOOD_PRESSURE_PLATE);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_STONE, 1 };
    inv.craft[1] = (ItemStack){ (ItemID)BLOCK_STONE, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_STONE_PRESSURE_PLATE);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[2] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[4] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_BOWL);
    assert(inv.craft_output.count == 4);
}

static void test_crafting_table_and_door_recipes(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[1] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[3] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[4] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_CRAFTING_TABLE);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[1] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[3] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[4] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[6] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[7] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_CRAFTING_TABLE);

    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_DOOR);
    assert(inv.craft_output.count == 3);
}

static void test_table_only_block_recipes(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        inv.craft[i] = (ItemStack){ ITEM_DIAMOND, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_DIAMOND_BLOCK);
    assert(inv.craft_output.count == 1);

    assert(survival_craft_recipe_count_for_grid(SURVIVAL_CRAFT_GRID_TABLE) >
           survival_craft_recipe_count_for_grid(SURVIVAL_CRAFT_GRID_PLAYER));
}

static void test_legacy_door_recipe_removed(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[1] = (ItemStack){ ITEM_STICK, 1 };
    inv.craft[3] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[4] = (ItemStack){ ITEM_STICK, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(item_stack_is_empty(&inv.craft_output));
}

static void test_food_items_and_mushroom_stew_recipe(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ ITEM_BROWN_MUSHROOM, 1 };
    inv.craft[1] = (ItemStack){ ITEM_BOWL, 1 };
    inv.craft[3] = (ItemStack){ ITEM_RED_MUSHROOM, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_MUSHROOM_STEW);
    assert(inv.craft_output.count == 1);
    assert(item_food_units(ITEM_APPLE) == 4);
    assert(item_food_units(ITEM_RED_MUSHROOM) == 2);
    assert(item_food_units(ITEM_MUSHROOM_STEW) == 8);
    assert(item_food_returns_bowl(ITEM_MUSHROOM_STEW));
}

static void test_furnace_torch_and_coal(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[1] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[2] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[3] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[5] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[6] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[7] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    inv.craft[8] = (ItemStack){ (ItemID)BLOCK_COBBLESTONE, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_FURNACE);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ ITEM_COAL, 1 };
    inv.craft[3] = (ItemStack){ ITEM_STICK, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_TORCH);
    assert(inv.craft_output.count == 4);

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    inv.craft[0] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    inv.craft[2] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    inv.craft[4] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_BUCKET);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    assert(survival_inventory_add_item(&inv, ITEM_COAL, 3) == 0);
    assert(survival_inventory_count_item(&inv, ITEM_COAL) == 3);
    assert(survival_inventory_add_item(&inv, ITEM_WATER_BUCKET, 1) == 0);
    assert(survival_inventory_add_item(&inv, ITEM_LAVA_BUCKET, 1) == 0);
    assert(survival_inventory_count_item(&inv, ITEM_WATER_BUCKET) == 1);
    assert(survival_inventory_count_item(&inv, ITEM_LAVA_BUCKET) == 1);
    assert(survival_inventory_remove_item(&inv, ITEM_COAL, 2));
    assert(survival_inventory_count_item(&inv, ITEM_COAL) == 1);
    assert(!survival_inventory_remove_item(&inv, ITEM_COAL, 2));
    assert(survival_drop_for_block(BLOCK_COAL_ORE) == ITEM_COAL);
    assert(survival_drop_for_block(BLOCK_DIAMOND_ORE) == ITEM_DIAMOND);
    assert(survival_drop_for_block(BLOCK_BUTTON_PRESSED) ==
           (ItemID)BLOCK_BUTTON);
    assert(survival_drop_for_block(BLOCK_WOOD_PRESSURE_PLATE_PRESSED) ==
           (ItemID)BLOCK_WOOD_PRESSURE_PLATE);
    assert(survival_drop_for_block(BLOCK_STONE_PRESSURE_PLATE_PRESSED) ==
           (ItemID)BLOCK_STONE_PRESSURE_PLATE);
    assert(item_is_furnace_fuel(ITEM_COAL));
    assert(item_is_furnace_fuel((ItemID)BLOCK_WOOD));
    assert(item_is_furnace_fuel((ItemID)BLOCK_PLANKS));
    assert(item_is_furnace_fuel(ITEM_STICK));
    assert(!item_is_furnace_fuel((ItemID)BLOCK_SAND));
    assert(item_furnace_smelt_output((ItemID)BLOCK_SAND) ==
           (ItemID)BLOCK_GLASS);
    assert(item_furnace_smelt_output((ItemID)BLOCK_IRON_ORE) ==
           ITEM_IRON_INGOT);
    assert(item_furnace_smelt_output((ItemID)BLOCK_GOLD_ORE) ==
           ITEM_GOLD_INGOT);

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    inv.craft[0] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    inv.craft[1] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    inv.craft[2] = (ItemStack){ ITEM_IRON_INGOT, 1 };
    inv.craft[4] = (ItemStack){ ITEM_STICK, 1 };
    inv.craft[7] = (ItemStack){ ITEM_STICK, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_IRON_PICKAXE);

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    inv.craft[0] = (ItemStack){ ITEM_DIAMOND, 1 };
    inv.craft[1] = (ItemStack){ ITEM_DIAMOND, 1 };
    inv.craft[3] = (ItemStack){ ITEM_DIAMOND, 1 };
    inv.craft[4] = (ItemStack){ ITEM_STICK, 1 };
    inv.craft[7] = (ItemStack){ ITEM_STICK, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_DIAMOND_AXE);

    assert(item_tool_kind(ITEM_STONE_PICKAXE) == ITEM_TOOL_PICKAXE);
    assert(item_tool_kind(ITEM_WOOD_AXE) == ITEM_TOOL_AXE);
    assert(item_tool_tier(ITEM_DIAMOND_PICKAXE) == ITEM_TOOL_TIER_DIAMOND);
    assert(item_break_seconds(ITEM_IRON_PICKAXE, BLOCK_STONE, 2.4f) < 2.4f);
    assert(item_break_seconds(ITEM_IRON_AXE, BLOCK_WOOD, 1.2f) < 1.2f);
    assert(item_break_seconds(ITEM_IRON_AXE, BLOCK_STONE, 2.4f) == 2.4f);
}

static void test_item_textures(void)
{
    assert(item_texture_id(ITEM_STICK) == TEX_TILE_STICK);
    assert(item_texture_id((ItemID)BLOCK_DOOR) == TEX_TILE_DOOR_ITEM);
    assert(item_texture_id((ItemID)BLOCK_CRAFTING_TABLE) ==
           TEX_TILE_CRAFTING_TABLE_FRONT);
    assert(item_texture_id((ItemID)BLOCK_FURNACE) == TEX_TILE_FURNACE_FRONT);
    assert(item_texture_id((ItemID)BLOCK_TORCH) == TEX_TILE_TORCH);
    assert(item_texture_id((ItemID)BLOCK_REDSTONE_WIRE_ON) ==
           TEX_TILE_REDSTONE_WIRE_ON);
    assert(item_texture_id((ItemID)BLOCK_REDSTONE_TORCH_OFF) ==
           TEX_TILE_REDSTONE_TORCH_OFF);
    assert(item_texture_id((ItemID)BLOCK_REPEATER_ON) == TEX_TILE_REPEATER_ON);
    assert(item_texture_id((ItemID)BLOCK_COMPARATOR_ON) ==
           TEX_TILE_COMPARATOR_ON);
    assert(item_texture_id((ItemID)BLOCK_LAMP_OFF) == TEX_TILE_LAMP_OFF);
    assert(item_texture_id((ItemID)BLOCK_BUTTON) == TEX_TILE_BUTTON);
    assert(item_texture_id((ItemID)BLOCK_LEVER_OFF) == TEX_TILE_LEVER_OFF);
    assert(item_texture_id((ItemID)BLOCK_LEVER_ON) == TEX_TILE_LEVER_ON);
    assert(item_texture_id(ITEM_DIAMOND_PICKAXE) ==
           TEX_TILE_DIAMOND_PICKAXE);
    assert(item_texture_id(ITEM_LAVA_BUCKET) == TEX_TILE_LAVA_BUCKET);
}

static void test_cursor_slot_clicks(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.storage[0] = (ItemStack){ (ItemID)BLOCK_DIRT, 8 };

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_STORAGE, 0, true));
    assert(inv.cursor.item == (ItemID)BLOCK_DIRT);
    assert(inv.cursor.count == 4);
    assert(inv.storage[0].count == 4);

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_STORAGE, 1, true));
    assert(inv.storage[1].item == (ItemID)BLOCK_DIRT);
    assert(inv.storage[1].count == 1);
    assert(inv.cursor.count == 3);
}

int main(void)
{
    init_block_types();
    test_pickup_stacks_into_inventory();
    test_wood_to_planks_recipe();
    test_planks_to_sticks_recipe();
    test_pressure_plate_and_bowl_recipes();
    test_crafting_table_and_door_recipes();
    test_table_only_block_recipes();
    test_legacy_door_recipe_removed();
    test_food_items_and_mushroom_stew_recipe();
    test_furnace_torch_and_coal();
    test_item_textures();
    test_cursor_slot_clicks();
    puts("inventory_test: ok");
    return 0;
}
