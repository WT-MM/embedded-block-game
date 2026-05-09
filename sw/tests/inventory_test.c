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
    assert(item_stack_is_empty(&inv.craft_output));

    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_DOOR);
    assert(inv.craft_output.count == 3);
}

static void test_table_only_block_recipes(void)
{
    SurvivalInventory inv;
    CraftRecipeView recipe;
    bool found_diamond_block = false;

    survival_inventory_init(&inv);
    survival_inventory_set_craft_grid_dim(&inv, SURVIVAL_CRAFT_GRID_TABLE);
    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        inv.craft[i] = (ItemStack){ (ItemID)BLOCK_DIAMOND_ORE, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_DIAMOND_BLOCK);
    assert(inv.craft_output.count == 1);

    assert(survival_craft_recipe_count_for_grid(SURVIVAL_CRAFT_GRID_TABLE) >
           survival_craft_recipe_count_for_grid(SURVIVAL_CRAFT_GRID_PLAYER));
    for (int i = 0;
         i < survival_craft_recipe_count_for_grid(SURVIVAL_CRAFT_GRID_TABLE);
         i++) {
        assert(survival_craft_recipe_view_for_grid(
            i, SURVIVAL_CRAFT_GRID_TABLE, &recipe));
        if (recipe.output == (ItemID)BLOCK_DIAMOND_BLOCK)
            found_diamond_block = true;
    }
    assert(found_diamond_block);
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
    CraftRecipeView recipe;
    bool found_stew = false;

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

    for (int i = 0; i < survival_craft_recipe_count(); i++) {
        assert(survival_craft_recipe_view(i, &recipe));
        if (recipe.output == ITEM_MUSHROOM_STEW &&
            recipe.output_count == 1 &&
            recipe.shapeless)
            found_stew = true;
    }
    assert(found_stew);
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
    test_crafting_table_and_door_recipes();
    test_table_only_block_recipes();
    test_legacy_door_recipe_removed();
    test_food_items_and_mushroom_stew_recipe();
    test_cursor_slot_clicks();
    puts("inventory_test: ok");
    return 0;
}
