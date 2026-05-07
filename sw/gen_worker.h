#ifndef GEN_WORKER_H
#define GEN_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "world.h"

/*
 * Background chunk generator. When started, it pulls (chunk_x, chunk_z,
 * generation) jobs off a bounded queue. For each, it runs terrain gen,
 * snapshot load, and per-chunk sky lighting into a local buffer with no
 * locks held, then takes world->world_mu to install the result via
 * world_finalize_async_chunk_load.
 *
 * The main thread feeds the worker by calling gen_worker_drain_pending
 * once per frame. That walks the chunk array, finds CHUNK_FLAG_LOADING
 * chunks without CHUNK_FLAG_GEN_QUEUED, tags them queued, and pushes a
 * job. Stale jobs (chunk evicted or re-streamed before the worker got
 * to it) are dropped on the finalize path via the generation check.
 *
 * Disabled (env VOXEL_GEN_WORKER=0): start returns false; the world's
 * async_chunk_gen_enabled flag stays clear so stream_generate_chunk
 * runs synchronously on the main thread.
 */

bool gen_worker_start(VoxelWorld *world);
void gen_worker_stop(void);
bool gen_worker_is_running(void);

/* Walk chunks[] under world_mu, push every LOADING non-queued chunk to
 * the worker queue, set CHUNK_FLAG_GEN_QUEUED. No-op if the worker is
 * not running (LOADING chunks are not produced in that mode). */
void gen_worker_drain_pending(VoxelWorld *world);

#endif
