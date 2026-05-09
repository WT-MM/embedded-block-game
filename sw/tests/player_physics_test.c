#include "block_types.h"
#include "player_physics.h"
#include "world.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define TEST_SEED 0x51f7e59u
#define TEST_DT (1.0f / 60.0f)

static void carve_ledge(VoxelWorld *world)
{
    for (int y = 0; y <= 4; y++)
        for (int z = -3; z <= 3; z++)
            for (int x = -3; x <= 6; x++)
                assert(world_set_block(world, x, y, z, BLOCK_AIR));

    assert(world_set_block(world, 0, 0, 0, BLOCK_STONE));
}

static void settle_player_on_ledge(Player *player, VoxelWorld *world)
{
    for (int i = 0; i < 4; i++)
        player_update(player, world, 0.0f, 0.0f,
                      false, false, false, false, false, TEST_DT);

    assert(player->is_grounded);
    assert(fabsf(player->y - 1.0f) < 0.01f);
}

static void test_crouch_stops_at_ledge(void)
{
    VoxelWorld world;
    Player player;

    world_init(&world);
    assert(world_init_infinite_procedural(&world, TEST_SEED, 1, false, 1,
                                          0.0f, 0.0f, NULL));
    carve_ledge(&world);
    player_init(&player, 0.5f, 1.0f, 0.5f);
    settle_player_on_ledge(&player, &world);

    for (int i = 0; i < 240; i++)
        player_update(&player, &world, 1.0f, 0.0f,
                      false, false, true, false, false, TEST_DT);

    assert(player.x < 1.251f);
    assert(player.is_grounded);
    assert(fabsf(player.y - 1.0f) < 0.01f);

    world_free(&world);
}

static void test_normal_walk_can_leave_ledge(void)
{
    VoxelWorld world;
    Player player;

    world_init(&world);
    assert(world_init_infinite_procedural(&world, TEST_SEED, 1, false, 1,
                                          0.0f, 0.0f, NULL));
    carve_ledge(&world);
    player_init(&player, 0.5f, 1.0f, 0.5f);
    settle_player_on_ledge(&player, &world);

    for (int i = 0; i < 90; i++)
        player_update(&player, &world, 1.0f, 0.0f,
                      false, false, false, false, false, TEST_DT);

    assert(player.x > 1.30f);
    assert(player.y < 0.95f || !player.is_grounded);

    world_free(&world);
}

int main(void)
{
    init_block_types();

    test_crouch_stops_at_ledge();
    test_normal_walk_can_leave_ledge();

    printf("player_physics_test: ok\n");
    return 0;
}
