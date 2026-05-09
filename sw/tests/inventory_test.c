#include "inventory.h"
#include "block_types.h"

#include <assert.h>
#include <stdio.h>

static void test_pickup_stacks_into_inventory(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    assert(survival_inventory_add_block(&inv, BLOCK_WOOD, 70) == 0);
    assert(inv.storage[0].block == BLOCK_WOOD);
    assert(inv.storage[0].count == 64);
    assert(inv.storage[1].block == BLOCK_WOOD);
    assert(inv.storage[1].count == 6);
}

static void test_wood_to_planks_recipe(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.craft[0] = (ItemStack){ BLOCK_WOOD, 1 };
    survival_inventory_refresh_craft_output(&inv);
    assert(inv.craft_output.block == BLOCK_PLANKS);
    assert(inv.craft_output.count == 4);

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_OUTPUT, 0, false));
    assert(inv.cursor.block == BLOCK_PLANKS);
    assert(inv.cursor.count == 4);
    assert(item_stack_is_empty(&inv.craft[0]));
    assert(item_stack_is_empty(&inv.craft_output));
}

static void test_cursor_slot_clicks(void)
{
    SurvivalInventory inv;

    survival_inventory_init(&inv);
    inv.storage[0] = (ItemStack){ BLOCK_DIRT, 8 };

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_STORAGE, 0, true));
    assert(inv.cursor.block == BLOCK_DIRT);
    assert(inv.cursor.count == 4);
    assert(inv.storage[0].count == 4);

    assert(survival_inventory_click(&inv, INVENTORY_SLOT_STORAGE, 1, true));
    assert(inv.storage[1].block == BLOCK_DIRT);
    assert(inv.storage[1].count == 1);
    assert(inv.cursor.count == 3);
}

int main(void)
{
    init_block_types();
    test_pickup_stacks_into_inventory();
    test_wood_to_planks_recipe();
    test_cursor_slot_clicks();
    puts("inventory_test: ok");
    return 0;
}
