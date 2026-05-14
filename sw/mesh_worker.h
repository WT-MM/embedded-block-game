#ifndef MESH_WORKER_H
#define MESH_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "world.h"

/*
 * Background mesh rebuilder. When started, it pulls (chunk_x, chunk_z,
 * generation) jobs off a bounded queue and runs world_rebuild_and_publish_mesh
 * on each, holding world->world_mu for the read-side data of the rebuild.
 * The renderer reads chunk meshes lock-free via chunk->live_mesh.
 *
 * The main thread feeds the worker by calling mesh_worker_drain_dirty once
 * per frame. That walks the chunk array, finds CHUNK_FLAG_MESH_DIRTY chunks,
 * tags the chunk as queued (so the same chunk is not double-queued), and
 * pushes a job. The dirty bit stays set until the worker publishes the mesh.
 *
 * Disabled (env VOXEL_MESH_WORKER=0): start returns false; drain falls
 * back to synchronous world_rebuild_dirty_meshes.
 */

bool mesh_worker_start(VoxelWorld *world);
void mesh_worker_stop(void);
bool mesh_worker_is_running(void);

/* Walk the chunk array under world_mu, push every dirty non-queued chunk to the
 * worker queue, and set MESH_QUEUED. If the worker is not running,
 * runs world_rebuild_dirty_meshes synchronously. */
void mesh_worker_drain_dirty(VoxelWorld *world);

/* Drain the retired_mesh slot on every chunk. Call once per frame from
 * the main thread, AFTER renderer_draw_world has returned, so any in-use
 * chunk-mesh pointer the renderer cached has gone out of scope. Safe to
 * call when the worker is running - only main thread frees retired meshes
 * so there is no contention with worker publication, which only ever
 * stores into retired_mesh. */
void mesh_worker_reap_retired(VoxelWorld *world);

#endif
