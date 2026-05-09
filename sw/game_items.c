#include "game_items.h"

#include <math.h>
#include <string.h>

#include "block_types.h"

#define ITEM_ENTITY_SIZE_WORLD 0.42f
#define ITEM_ENTITY_PICKUP_DELAY_SECONDS 0.35f
#define ITEM_ENTITY_LIFETIME_SECONDS 300.0f
#define ITEM_ENTITY_PICKUP_RADIUS 1.35f
#define ITEM_ENTITY_GRAVITY 18.0f

void item_entities_init(ItemEntityPool *pool)
{
    if (!pool)
        return;

    memset(pool, 0, sizeof(*pool));
}

static ItemEntity *item_entity_find_spawn_slot(ItemEntityPool *pool)
{
    int oldest_index = 0;
    float oldest_age = -1.0f;

    if (!pool)
        return NULL;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        if (!pool->items[i].active)
            return &pool->items[i];
        if (pool->items[i].age_seconds > oldest_age) {
            oldest_age = pool->items[i].age_seconds;
            oldest_index = i;
        }
    }

    return &pool->items[oldest_index];
}

static void item_entity_spawn_stack(ItemEntityPool *pool,
                                    ItemID item,
                                    int count,
                                    Vec3 position,
                                    Vec3 velocity,
                                    float pickup_delay)
{
    while (pool && count > 0 && item != ITEM_NONE && item < NUM_ITEM_TYPES) {
        ItemEntity *entity = item_entity_find_spawn_slot(pool);
        int stack_count = count > ITEM_STACK_MAX ? ITEM_STACK_MAX : count;

        if (!entity)
            return;

        memset(entity, 0, sizeof(*entity));
        entity->active = true;
        entity->stack.item = item;
        entity->stack.count = (uint8_t)stack_count;
        entity->position = position;
        entity->velocity = velocity;
        entity->age_seconds = 0.0f;
        entity->pickup_delay_seconds = pickup_delay;
        entity->bob_seed = (float)(pool->spawn_counter++ & 31u) * 0.37f;
        count -= stack_count;
    }
}

void item_entity_spawn_block_drop(ItemEntityPool *pool,
                                  BlockID broken_block,
                                  int wx,
                                  int wy,
                                  int wz,
                                  Vec3 push_dir)
{
    ItemID drop = survival_drop_for_block(broken_block);
    Vec3 pos = {
        (float)wx + 0.5f,
        (float)wy + 0.55f,
        (float)wz + 0.5f,
    };
    Vec3 vel = {
        push_dir.x * 1.2f,
        2.2f,
        push_dir.z * 1.2f,
    };

    if (drop == ITEM_NONE)
        return;

    item_entity_spawn_stack(pool, drop, 1, pos, vel,
                            ITEM_ENTITY_PICKUP_DELAY_SECONDS);
}

static void item_entity_spawn_near_player(ItemEntityPool *pool,
                                          const Player *player,
                                          const ItemStack *stack)
{
    if (!pool || !player || item_stack_is_empty(stack))
        return;

    Vec3 pos = {
        player->x,
        player->y + PLAYER_HEIGHT * 0.55f,
        player->z,
    };
    Vec3 vel = { 0.0f, 2.0f, 0.0f };

    item_entity_spawn_stack(pool, stack->item, stack->count, pos, vel, 0.0f);
}

static void return_stack_to_inventory_or_drop(SurvivalInventory *inv,
                                              ItemEntityPool *drops,
                                              const Player *player,
                                              ItemStack *stack)
{
    int leftover;

    if (!inv || item_stack_is_empty(stack))
        return;

    leftover = survival_inventory_add_item(inv, stack->item, stack->count);
    if (leftover > 0) {
        ItemStack drop = { stack->item, (uint8_t)leftover };
        item_entity_spawn_near_player(drops, player, &drop);
    }
    item_stack_clear(stack);
}

void close_survival_inventory(SurvivalInventory *inv,
                              ItemEntityPool *drops,
                              const Player *player,
                              bool *inventory_open)
{
    if (inventory_open)
        *inventory_open = false;
    if (!inv)
        return;

    for (int i = 0; i < SURVIVAL_CRAFT_SLOT_COUNT; i++)
        return_stack_to_inventory_or_drop(inv, drops, player, &inv->craft[i]);
    return_stack_to_inventory_or_drop(inv, drops, player, &inv->cursor);
    survival_inventory_refresh_craft_output(inv);
}

static bool item_entity_floor_collision(const VoxelWorld *world,
                                        const ItemEntity *entity,
                                        float *floor_y_out)
{
    int wx;
    int wy;
    int wz;
    BlockID block;

    if (!world || !entity)
        return false;

    wx = (int)floorf(entity->position.x);
    wy = (int)floorf(entity->position.y - 0.24f);
    wz = (int)floorf(entity->position.z);
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return false;

    block = world_get_block(world, wx, wy, wz);
    if (block_is_passable(block))
        return false;

    if (floor_y_out)
        *floor_y_out = (float)wy + 1.18f;
    return true;
}

void item_entities_update(ItemEntityPool *pool,
                          const VoxelWorld *world,
                          SurvivalInventory *inventory,
                          const Player *player,
                          float dt)
{
    if (!pool || !inventory || !player)
        return;

    if (dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        ItemEntity *entity = &pool->items[i];
        float floor_y;

        if (!entity->active)
            continue;

        entity->age_seconds += dt;
        if (entity->age_seconds >= ITEM_ENTITY_LIFETIME_SECONDS ||
            item_stack_is_empty(&entity->stack)) {
            entity->active = false;
            continue;
        }

        entity->velocity.y -= ITEM_ENTITY_GRAVITY * dt;
        entity->position.x += entity->velocity.x * dt;
        entity->position.y += entity->velocity.y * dt;
        entity->position.z += entity->velocity.z * dt;
        entity->velocity.x *= 0.98f;
        entity->velocity.z *= 0.98f;

        if (item_entity_floor_collision(world, entity, &floor_y) &&
            entity->position.y < floor_y) {
            entity->position.y = floor_y;
            if (entity->velocity.y < 0.0f)
                entity->velocity.y = 0.0f;
            entity->velocity.x *= 0.72f;
            entity->velocity.z *= 0.72f;
        }

        if (entity->age_seconds >= entity->pickup_delay_seconds) {
            float dx = entity->position.x - player->x;
            float dy = entity->position.y - (player->y + PLAYER_HEIGHT * 0.5f);
            float dz = entity->position.z - player->z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            float pickup_sq = ITEM_ENTITY_PICKUP_RADIUS * ITEM_ENTITY_PICKUP_RADIUS;

            if (dist_sq <= pickup_sq) {
                int leftover = survival_inventory_add_item(inventory,
                                                           entity->stack.item,
                                                           entity->stack.count);
                if (leftover <= 0) {
                    entity->active = false;
                } else {
                    entity->stack.count = (uint8_t)leftover;
                    entity->pickup_delay_seconds = entity->age_seconds + 0.4f;
                }
            }
        }
    }
}

void item_entities_draw(RenderContext *ctx,
                        const ItemEntityPool *pool,
                        float world_time)
{
    if (!ctx || !pool)
        return;

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        const ItemEntity *entity = &pool->items[i];
        Vec3 pos;

        if (!entity->active || item_stack_is_empty(&entity->stack))
            continue;

        pos = entity->position;
        pos.y += sinf(world_time * 4.0f + entity->bob_seed) * 0.05f;
        renderer_draw_world_billboard_tile(
            ctx, pos, ITEM_ENTITY_SIZE_WORLD,
            item_texture_id(entity->stack.item),
            QUAD_FLAG_ALPHA_KEY | QUAD_FLAG_ZTEST);
    }
}
