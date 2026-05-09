#ifndef WORLD_GEN_H
#define WORLD_GEN_H

#include <stdint.h>

#include "world.h"

void world_generate_chunk_terrain(Chunk *chunk,
                                  uint32_t base_seed,
                                  int stone_tries_per_chunk);

#endif
