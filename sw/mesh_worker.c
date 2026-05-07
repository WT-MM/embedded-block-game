#include "mesh_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * SPSC ring buffer of pending mesh-rebuild jobs. Push happens on the
 * main thread (mesh_worker_drain_dirty); pop happens on the worker.
 * Capacity is power-of-two so head/tail wrap is a mask. A full queue is
 * not an error - we just leave MESH_DIRTY set on the chunk and it will
 * be picked up next drain pass.
 *
 * The job stores (chunk_x, chunk_z, generation) instead of a Chunk*
 * because retain_chunks_in_window can compact the chunks[] array, so a
 * pointer captured at push time would dangle. Coordinates round-trip
 * through chunk_lookup_find_index in the worker, and we re-check
 * generation under world_mu to discard stale jobs (chunk evicted and
 * re-streamed before the worker got to it).
 */
#define MESH_JOB_QUEUE_CAPACITY 1024u

typedef struct {
    int chunk_x;
    int chunk_z;
    uint32_t generation;
} MeshJob;

static struct {
    VoxelWorld *world;
    pthread_t thread;
    bool thread_started;

    pthread_mutex_t queue_mu;
    pthread_cond_t  queue_cv;
    MeshJob queue[MESH_JOB_QUEUE_CAPACITY];
    unsigned head;          /* next pop index */
    unsigned tail;          /* next push index */
    _Atomic bool stop;
    bool running;
} g_worker;

static bool worker_enabled_from_env(void)
{
    const char *value = getenv("VOXEL_MESH_WORKER");
    if (!value || value[0] == '\0')
        return true;
    if (value[0] == '0' && value[1] == '\0')
        return false;
    return true;
}

static unsigned queue_count_unlocked(void)
{
    return g_worker.tail - g_worker.head;
}

static bool queue_push_unlocked(const MeshJob *job)
{
    if (queue_count_unlocked() >= MESH_JOB_QUEUE_CAPACITY)
        return false;
    g_worker.queue[g_worker.tail & (MESH_JOB_QUEUE_CAPACITY - 1u)] = *job;
    g_worker.tail++;
    return true;
}

static bool queue_pop_unlocked(MeshJob *out)
{
    if (queue_count_unlocked() == 0)
        return false;
    *out = g_worker.queue[g_worker.head & (MESH_JOB_QUEUE_CAPACITY - 1u)];
    g_worker.head++;
    return true;
}

static void *mesh_worker_thread(void *arg)
{
    VoxelWorld *world = (VoxelWorld *)arg;

    for (;;) {
        MeshJob job;
        bool got_job = false;

        pthread_mutex_lock(&g_worker.queue_mu);
        while (!atomic_load_explicit(&g_worker.stop, memory_order_acquire) &&
               queue_count_unlocked() == 0) {
            pthread_cond_wait(&g_worker.queue_cv, &g_worker.queue_mu);
        }
        if (atomic_load_explicit(&g_worker.stop, memory_order_acquire) &&
            queue_count_unlocked() == 0) {
            pthread_mutex_unlock(&g_worker.queue_mu);
            break;
        }
        got_job = queue_pop_unlocked(&job);
        pthread_mutex_unlock(&g_worker.queue_mu);

        if (!got_job)
            continue;

        /*
         * Resolve coords -> Chunk* under world_mu so the chunk array
         * cannot be compacted underneath us. Discard the job if the
         * chunk has been evicted or re-generated since enqueue.
         */
        world_lock(world);
        Chunk *chunk = world_get_chunk_mut_locked(world, job.chunk_x, job.chunk_z);
        if (chunk && chunk->generation == job.generation) {
            if (world_rebuild_and_publish_mesh(world, chunk))
                world->meshes_rebuilt_last_stream++;
            chunk->flags &= ~CHUNK_FLAG_MESH_QUEUED;
        } else if (chunk) {
            chunk->flags &= ~CHUNK_FLAG_MESH_QUEUED;
        }
        world_unlock(world);
    }

    return NULL;
}

bool mesh_worker_start(VoxelWorld *world)
{
    int rc;

    if (!world)
        return false;
    if (g_worker.running)
        return true;
    if (!worker_enabled_from_env())
        return false;

    memset(&g_worker, 0, sizeof(g_worker));
    g_worker.world = world;

    if (pthread_mutex_init(&g_worker.queue_mu, NULL) != 0)
        return false;
    if (pthread_cond_init(&g_worker.queue_cv, NULL) != 0) {
        pthread_mutex_destroy(&g_worker.queue_mu);
        return false;
    }

    rc = pthread_create(&g_worker.thread, NULL, mesh_worker_thread, world);
    if (rc != 0) {
        fprintf(stderr, "mesh_worker: pthread_create failed (%d)\n", rc);
        pthread_cond_destroy(&g_worker.queue_cv);
        pthread_mutex_destroy(&g_worker.queue_mu);
        return false;
    }

    g_worker.thread_started = true;
    g_worker.running = true;

    world_lock(world);
    world->async_mesh_rebuilds_enabled = true;
    world_unlock(world);
    return true;
}

void mesh_worker_stop(void)
{
    VoxelWorld *world = g_worker.world;

    if (!g_worker.running)
        return;

    pthread_mutex_lock(&g_worker.queue_mu);
    atomic_store_explicit(&g_worker.stop, true, memory_order_release);
    pthread_cond_broadcast(&g_worker.queue_cv);
    pthread_mutex_unlock(&g_worker.queue_mu);

    if (g_worker.thread_started)
        pthread_join(g_worker.thread, NULL);

    pthread_cond_destroy(&g_worker.queue_cv);
    pthread_mutex_destroy(&g_worker.queue_mu);

    if (world) {
        world_lock(world);
        world->async_mesh_rebuilds_enabled = false;
        world_unlock(world);
    }

    memset(&g_worker, 0, sizeof(g_worker));
}

bool mesh_worker_is_running(void)
{
    return g_worker.running;
}

void mesh_worker_drain_dirty(VoxelWorld *world)
{
    if (!world)
        return;

    if (!g_worker.running) {
        world_rebuild_dirty_meshes(world);
        return;
    }

    /*
     * Hold world_mu across the scan so we get a coherent snapshot of
     * which chunks are dirty and mark MESH_QUEUED atomically with the
     * generation snapshot we enqueue. Worker also takes world_mu at
     * job execution time, so by the time it actually rebuilds, any
     * later main-thread edit will leave MESH_DIRTY set for a follow-up job.
     */
    world_lock(world);

    pthread_mutex_lock(&g_worker.queue_mu);

    bool pushed_any = false;
    bool any_dirty = false;
    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADED) ||
            !(chunk->flags & CHUNK_FLAG_MESH_DIRTY))
            continue;
        any_dirty = true;
        if (chunk->flags & CHUNK_FLAG_MESH_QUEUED)
            continue;

        MeshJob job = {
            .chunk_x = chunk->chunk_x,
            .chunk_z = chunk->chunk_z,
            .generation = chunk->generation,
        };

        if (!queue_push_unlocked(&job)) {
            /* Queue full - leave dirty bit set, retry next frame. */
            break;
        }
        chunk->flags |= CHUNK_FLAG_MESH_QUEUED;
        pushed_any = true;
    }

    world->meshes_dirty = any_dirty;

    if (pushed_any)
        pthread_cond_signal(&g_worker.queue_cv);

    pthread_mutex_unlock(&g_worker.queue_mu);

    world_unlock(world);
}

void mesh_worker_reap_retired(VoxelWorld *world)
{
    if (!world)
        return;

    /*
     * Walks chunks[] and frees retired_mesh on each. Reads world->chunks
     * without world_mu - safe because retain_chunks_in_window only ever
     * runs on the main thread, the same thread that calls reap, and
     * this is called outside the renderer's draw window.
     */
    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];
        if (!(chunk->flags & CHUNK_FLAG_LOADED))
            continue;
        chunk_mesh_free_retired(chunk);
    }
}
