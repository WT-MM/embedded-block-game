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
    inv.craft[2] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == ITEM_STICK);
    assert(inv.craft_output.count == 4);
}

static void test_crafting_table_and_door_recipes(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        inv.craft[i] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_CRAFTING_TABLE);
    assert(inv.craft_output.count == 1);

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[1] = (ItemStack){ ITEM_STICK, 1 };
    inv.craft[2] = (ItemStack){ (ItemID)BLOCK_PLANKS, 1 };
    inv.craft[3] = (ItemStack){ ITEM_STICK, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.item == (ItemID)BLOCK_DOOR);
    assert(inv.craft_output.count == 1);
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
    test_cursor_slot_clicks();
    puts("inventory_test: ok");
    return 0;
}
