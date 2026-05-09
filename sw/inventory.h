#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "block_types.h"

#define ITEM_STACK_MAX 64
#define SURVIVAL_HOTBAR_SLOT_COUNT 9
#define SURVIVAL_MAIN_SLOT_COUNT 27
#define SURVIVAL_STORAGE_SLOT_COUNT \
    (SURVIVAL_HOTBAR_SLOT_COUNT + SURVIVAL_MAIN_SLOT_COUNT)
#define SURVIVAL_CRAFT_SLOT_COUNT 4

typedef enum {
    ITEM_NONE = 0,
    ITEM_STICK = NUM_BLOCK_TYPES,
    NUM_ITEM_TYPES,
} ItemID;

typedef struct {
    ItemID item;
    uint8_t count;
} ItemStack;

typedef enum {
    INVENTORY_SLOT_NONE = 0,
    INVENTORY_SLOT_STORAGE,
    INVENTORY_SLOT_CRAFT,
    INVENTORY_SLOT_OUTPUT,
} InventorySlotArea;

typedef struct {
    ItemStack storage[SURVIVAL_STORAGE_SLOT_COUNT];
    ItemStack craft[SURVIVAL_CRAFT_SLOT_COUNT];
    ItemStack craft_output;
    ItemStack cursor;
} SurvivalInventory;

void item_stack_clear(ItemStack *stack);
bool item_stack_is_empty(const ItemStack *stack);
bool item_stack_can_merge(const ItemStack *stack, ItemID item);
int item_stack_add(ItemStack *stack, ItemID item, int count);
int item_stack_remove(ItemStack *stack, int count);
const char *item_name(ItemID item);
uint8_t item_texture_id(ItemID item);
bool item_is_placeable_block(ItemID item);
BlockID item_place_block(ItemID item);

void survival_inventory_init(SurvivalInventory *inv);
int survival_inventory_add_item(SurvivalInventory *inv, ItemID item, int count);
int survival_inventory_add_block(SurvivalInventory *inv, BlockID block, int count);
bool survival_inventory_remove_storage(SurvivalInventory *inv, int slot, int count);
void survival_inventory_refresh_craft_output(SurvivalInventory *inv);
bool survival_inventory_click(SurvivalInventory *inv,
                              InventorySlotArea area,
                              int index,
                              bool right_click);
const ItemStack *survival_inventory_hotbar_stack(const SurvivalInventory *inv,
                                                 int slot);

ItemID survival_drop_for_block(BlockID block);

#endif
