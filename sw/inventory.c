#include "inventory.h"

#include <string.h>

typedef struct {
    uint8_t width;
    uint8_t height;
    ItemID inputs[SURVIVAL_CRAFT_SLOT_COUNT];
    ItemID output;
    uint8_t output_count;
} CraftRecipe;

static const CraftRecipe CRAFT_RECIPES[] = {
    {
        .width = 1,
        .height = 1,
        .inputs = { (ItemID)BLOCK_WOOD, ITEM_NONE, ITEM_NONE, ITEM_NONE },
        .output = (ItemID)BLOCK_PLANKS,
        .output_count = 4,
    },
    {
        .width = 1,
        .height = 2,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    ITEM_NONE, ITEM_NONE },
        .output = ITEM_STICK,
        .output_count = 4,
    },
    {
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS,
                    (ItemID)BLOCK_PLANKS, (ItemID)BLOCK_PLANKS },
        .output = (ItemID)BLOCK_CRAFTING_TABLE,
        .output_count = 1,
    },
    {
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_PLANKS, ITEM_STICK,
                    (ItemID)BLOCK_PLANKS, ITEM_STICK },
        .output = (ItemID)BLOCK_DOOR,
        .output_count = 1,
    },
    {
        .width = 2,
        .height = 2,
        .inputs = { (ItemID)BLOCK_SAND, (ItemID)BLOCK_SAND,
                    (ItemID)BLOCK_SAND, (ItemID)BLOCK_SAND },
        .output = (ItemID)BLOCK_SANDSTONE,
        .output_count = 1,
    },
};

static bool item_id_valid(ItemID item)
{
    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return true;

    return item == ITEM_STICK;
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
    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return BlockRegistry[(BlockID)item].name;
    if (item == ITEM_STICK)
        return "Stick";

    return "Unknown";
}

uint8_t item_texture_id(ItemID item)
{
    if (item > ITEM_NONE && item < (ItemID)NUM_BLOCK_TYPES)
        return block_face_texture_id((BlockID)item, FACE_FRONT);
    if (item == ITEM_STICK)
        return TEX_TILE_WOOD_PLANK;

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

static bool craft_recipe_matches_at(const SurvivalInventory *inv,
                                    const CraftRecipe *recipe,
                                    int offset_x,
                                    int offset_y)
{
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            int grid_index = y * 2 + x;
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

static bool craft_recipe_matches(const SurvivalInventory *inv,
                                 const CraftRecipe *recipe)
{
    if (!inv || !recipe || recipe->width == 0 || recipe->height == 0 ||
        recipe->width > 2 || recipe->height > 2)
        return false;

    for (int y = 0; y <= 2 - (int)recipe->height; y++) {
        for (int x = 0; x <= 2 - (int)recipe->width; x++) {
            if (craft_recipe_matches_at(inv, recipe, x, y))
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

    survival_inventory_refresh_craft_output(inv);
    output = inv->craft_output;
    if (!cursor_accepts_stack(&inv->cursor, &output))
        return false;

    if (item_stack_is_empty(&inv->cursor))
        inv->cursor = output;
    else
        inv->cursor.count = (uint8_t)(inv->cursor.count + output.count);

    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++) {
        if (!item_stack_is_empty(&inv->craft[i]))
            item_stack_remove(&inv->craft[i], 1);
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
    default:
        if (block > BLOCK_AIR && block < NUM_BLOCK_TYPES)
            return (ItemID)block;
        return ITEM_NONE;
    }
}
