#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "block_types.h"
#include "world.h"

static void tick_redstone(VoxelWorld *world, int ticks)
{
    for (int i = 0; i < ticks; i++)
        world_update_redstone(world, 0.1f);
}

int main(int argc, char **argv)
{
    VoxelWorld world;

    if (argc < 2 || ((argc - 2) % 3) != 0) {
        fprintf(stderr, "usage: %s WORLD_DIR [RESET_X RESET_Y RESET_Z]...\n",
                argv[0]);
        return 2;
    }

    init_block_types();
    world_init(&world);
    if (!world_init_infinite_procedural(&world,
                                        SETTLE_WORLD_SEED,
                                        SETTLE_STONE_TRIES,
                                        false,
                                        SETTLE_RENDER_DISTANCE,
                                        SETTLE_CENTER_X,
                                        SETTLE_CENTER_Z,
                                        argv[1])) {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        return 3;
    }

    tick_redstone(&world, 30);
    for (int arg = 2; arg < argc; arg += 3) {
        int wx = atoi(argv[arg]);
        int wy = atoi(argv[arg + 1]);
        int wz = atoi(argv[arg + 2]);

        if (!world_press_button(&world, wx, wy, wz)) {
            fprintf(stderr, "failed to press reset at %d/%d/%d\n",
                    wx, wy, wz);
            world_free(&world);
            return 4;
        }
        tick_redstone(&world, 50);
    }
    tick_redstone(&world, 100);

    if (!world_flush(&world)) {
        fprintf(stderr, "failed to flush settled chunks\n");
        world_free(&world);
        return 5;
    }

    world_free(&world);
    return 0;
}
