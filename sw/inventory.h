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
#define SURVIVAL_CRAFT_GRID_PLAYER 2
#define SURVIVAL_CRAFT_GRID_TABLE 3
#define SURVIVAL_CRAFT_SLOT_COUNT \
    (SURVIVAL_CRAFT_GRID_TABLE * SURVIVAL_CRAFT_GRID_TABLE)

typedef enum {
    ITEM_NONE = 0,
    ITEM_STICK = NUM_BLOCK_TYPES,
    ITEM_APPLE,
    ITEM_RED_MUSHROOM,
    ITEM_BROWN_MUSHROOM,
    ITEM_BOWL,
    ITEM_MUSHROOM_STEW,
    ITEM_COAL,
    ITEM_IRON_INGOT,
    ITEM_GOLD_INGOT,
    ITEM_DIAMOND,
    ITEM_WOOD_PICKAXE,
    ITEM_STONE_PICKAXE,
    ITEM_IRON_PICKAXE,
    ITEM_GOLD_PICKAXE,
    ITEM_DIAMOND_PICKAXE,
    ITEM_WOOD_AXE,
    ITEM_STONE_AXE,
    ITEM_IRON_AXE,
    ITEM_GOLD_AXE,
    ITEM_DIAMOND_AXE,
    NUM_ITEM_TYPES,
} ItemID;

typedef enum {
    ITEM_TOOL_NONE = 0,
    ITEM_TOOL_PICKAXE,
    ITEM_TOOL_AXE,
} ItemToolKind;

typedef enum {
    ITEM_TOOL_TIER_NONE = 0,
    ITEM_TOOL_TIER_WOOD,
    ITEM_TOOL_TIER_STONE,
    ITEM_TOOL_TIER_IRON,
    ITEM_TOOL_TIER_GOLD,
    ITEM_TOOL_TIER_DIAMOND,
} ItemToolTier;

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
    uint8_t craft_grid_dim;
} SurvivalInventory;

typedef struct {
    bool shapeless;
    uint8_t width;
    uint8_t height;
    ItemID inputs[SURVIVAL_CRAFT_SLOT_COUNT];
    ItemID output;
    uint8_t output_count;
} CraftRecipeView;

void item_stack_clear(ItemStack *stack);
bool item_stack_is_empty(const ItemStack *stack);
bool item_stack_can_merge(const ItemStack *stack, ItemID item);
int item_stack_add(ItemStack *stack, ItemID item, int count);
int item_stack_remove(ItemStack *stack, int count);
const char *item_name(ItemID item);
uint8_t item_texture_id(ItemID item);
bool item_is_placeable_block(ItemID item);
BlockID item_place_block(ItemID item);
bool item_is_furnace_fuel(ItemID item);
ItemID item_furnace_smelt_output(ItemID input);
ItemToolKind item_tool_kind(ItemID item);
ItemToolTier item_tool_tier(ItemID item);
bool item_is_tool(ItemID item);
float item_break_seconds(ItemID item, BlockID block, float base_seconds);
int item_food_units(ItemID item);
bool item_food_returns_bowl(ItemID item);

void survival_inventory_init(SurvivalInventory *inv);
void survival_inventory_set_craft_grid_dim(SurvivalInventory *inv, int grid_dim);
int survival_inventory_craft_grid_dim(const SurvivalInventory *inv);
int survival_inventory_add_item(SurvivalInventory *inv, ItemID item, int count);
int survival_inventory_add_block(SurvivalInventory *inv, BlockID block, int count);
bool survival_inventory_remove_storage(SurvivalInventory *inv, int slot, int count);
int survival_inventory_count_item(const SurvivalInventory *inv, ItemID item);
bool survival_inventory_remove_item(SurvivalInventory *inv, ItemID item, int count);
void survival_inventory_refresh_craft_output(SurvivalInventory *inv);
bool survival_inventory_click(SurvivalInventory *inv,
                              InventorySlotArea area,
                              int index,
                              bool right_click);
const ItemStack *survival_inventory_hotbar_stack(const SurvivalInventory *inv,
                                                 int slot);
int survival_craft_recipe_count(void);
bool survival_craft_recipe_view(int index, CraftRecipeView *view);
int survival_craft_recipe_count_for_grid(int grid_dim);
bool survival_craft_recipe_view_for_grid(int index, int grid_dim,
                                         CraftRecipeView *view);

ItemID survival_drop_for_block(BlockID block);

#endif
