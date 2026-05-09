#ifndef GAME_ITEMS_H
#define GAME_ITEMS_H

#include <stdbool.h>
#include <stdint.h>

#include "inventory.h"
#include "player_physics.h"
#include "renderer.h"
#include "world.h"

#define ITEM_ENTITY_MAX 128

typedef struct {
    bool active;
    ItemStack stack;
    Vec3 position;
    Vec3 velocity;
    float age_seconds;
    float pickup_delay_seconds;
    float bob_seed;
} ItemEntity;

typedef struct {
    ItemEntity items[ITEM_ENTITY_MAX];
    uint32_t spawn_counter;
} ItemEntityPool;

void item_entities_init(ItemEntityPool *pool);
void item_entity_spawn_block_drop(ItemEntityPool *pool,
                                  BlockID broken_block,
                                  int wx,
                                  int wy,
                                  int wz,
                                  Vec3 push_dir);
void item_entity_spawn_item_near_player(ItemEntityPool *pool,
                                        const Player *player,
                                        ItemID item,
                                        int count);
void item_entity_toss_item(ItemEntityPool *pool,
                           const Player *player,
                           Vec3 push_dir,
                           ItemID item,
                           int count);
void close_survival_inventory(SurvivalInventory *inv,
                              ItemEntityPool *drops,
                              const Player *player,
                              bool *inventory_open);
void item_entities_update(ItemEntityPool *pool,
                          const VoxelWorld *world,
                          SurvivalInventory *inventory,
                          const Player *player,
                          float dt);
void item_entities_draw(RenderContext *ctx,
                        const ItemEntityPool *pool,
                        float world_time);

#endif
