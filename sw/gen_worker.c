#include "gen_worker.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "env_util.h"
#include "thread_affinity.h"

/*
 * SPSC ring buffer of pending chunk-gen jobs. Push from the main thread
 * (gen_worker_drain_pending), pop on the worker. Capacity is power-of-two
 * so head/tail wrap with a mask. A full queue is not an error - we just
 * leave CHUNK_FLAG_LOADING set without GEN_QUEUED, so the next drain
 * pass retries.
 *
 * The job stores (chunk_x, chunk_z, generation) instead of a Chunk*:
 * retain_chunks_in_window can compact the chunks[] array, so a pointer
 * captured at push time would dangle. The worker resolves coords ->
 * Chunk* under world_mu in world_finalize_async_chunk_load and discards
 * stale jobs (chunk evicted and re-streamed before the worker ran).
 */
#define GEN_JOB_QUEUE_CAPACITY 256u

typedef struct {
    int chunk_x;
    int chunk_z;
    uint32_t generation;
} GenJob;

static struct {
    VoxelWorld *world;
    pthread_t thread;
    bool thread_started;

    pthread_mutex_t queue_mu;
    pthread_cond_t  queue_cv;
    GenJob queue[GEN_JOB_QUEUE_CAPACITY];
    unsigned head;          /* next pop index */
    unsigned tail;          /* next push index */
    _Atomic bool stop;
    bool running;
} g_worker;

static bool worker_enabled_from_env(void)
{
    return env_flag("VOXEL_GEN_WORKER", true);
}

/* Per-frame cap on how many gen jobs we push into the worker queue.
 * Caches the env value on first read.
 *
 * Returns:
 *   < 0: env unset, fall back to world->stream_chunks_per_frame
 *   = 0: explicit unlimited
 *   > 0: hard cap
 *
 * Why this exists: the pre-async path implicitly rate-limited mesh-dirty
 * marking via VOXEL_CHUNKS_PER_FRAME on the main thread. With async gen,
 * the worker can finalize all freshly-streamed chunks back-to-back, which
 * either floods the mesh queue (without the deferral fix) or piles up a
 * single big mesh wave once neighbors complete. Capping pushes per drain
 * call restores the smoothed pace.
 */
static int gen_pushes_cap_from_env(void)
{
    static int cached = -1;
    static bool initialized = false;

    if (!initialized) {
        cached = env_int_or_default("VOXEL_GEN_PUSHES_PER_FRAME",
                                    -1,
                                    0,
                                    1023);
        initialized = true;
    }

    return cached;
}

static unsigned queue_count_unlocked(void)
{
    return g_worker.tail - g_worker.head;
}

static bool queue_push_unlocked(const GenJob *job)
{
    if (queue_count_unlocked() >= GEN_JOB_QUEUE_CAPACITY)
        return false;
    g_worker.queue[g_worker.tail & (GEN_JOB_QUEUE_CAPACITY - 1u)] = *job;
    g_worker.tail++;
    return true;
}

static bool queue_pop_unlocked(GenJob *out)
{
    if (queue_count_unlocked() == 0)
        return false;
    *out = g_worker.queue[g_worker.head & (GEN_JOB_QUEUE_CAPACITY - 1u)];
    g_worker.head++;
    return true;
}

static void *gen_worker_thread(void *arg)
{
    VoxelWorld *world = (VoxelWorld *)arg;

    thread_affinity_pin_current("gen_worker", "VOXEL_GEN_CPU", 1);

    for (;;) {
        GenJob job;
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

        /* Generate without holding world_mu - terrain gen and snapshot
         * load only read fields that are immutable post-init. The
         * finalize call takes world_mu briefly to copy the result in. */
        ChunkGenResult result;
        if (!world_async_chunk_gen_offline(world, job.chunk_x, job.chunk_z,
                                           &result)) {
            /* Snapshot I/O or alloc failure. Slot stays LOADING |
             * GEN_QUEUED until eviction; rare enough that we accept
             * the leaked-chunk-until-eviction behaviour for v1. */
            fprintf(stderr,
                    "gen_worker: offline gen failed for (%d, %d)\n",
                    job.chunk_x, job.chunk_z);
            continue;
        }
        (void)world_finalize_async_chunk_load(world,
                                              job.chunk_x, job.chunk_z,
                                              job.generation, &result);
    }

    return NULL;
}

bool gen_worker_start(VoxelWorld *world)
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

    rc = pthread_create(&g_worker.thread, NULL, gen_worker_thread, world);
    if (rc != 0) {
        fprintf(stderr, "gen_worker: pthread_create failed (%d)\n", rc);
        pthread_cond_destroy(&g_worker.queue_cv);
        pthread_mutex_destroy(&g_worker.queue_mu);
        return false;
    }

    g_worker.thread_started = true;
    g_worker.running = true;

    world_lock(world);
    world->async_chunk_gen_enabled = true;
    world_unlock(world);
    return true;
}

void gen_worker_stop(void)
{
    VoxelWorld *world = g_worker.world;

    if (!g_worker.running)
        return;

    /* Flip async_chunk_gen_enabled off first so any stream pass running
     * after this point will fall back to synchronous gen for new chunks.
     * Existing LOADING chunks already in the queue still get drained. */
    if (world) {
        world_lock(world);
        world->async_chunk_gen_enabled = false;
        world_unlock(world);
    }

    pthread_mutex_lock(&g_worker.queue_mu);
    atomic_store_explicit(&g_worker.stop, true, memory_order_release);
    pthread_cond_broadcast(&g_worker.queue_cv);
    pthread_mutex_unlock(&g_worker.queue_mu);

    if (g_worker.thread_started)
        pthread_join(g_worker.thread, NULL);

    pthread_cond_destroy(&g_worker.queue_cv);
    pthread_mutex_destroy(&g_worker.queue_mu);

    memset(&g_worker, 0, sizeof(g_worker));
}

bool gen_worker_is_running(void)
{
    return g_worker.running;
}

void gen_worker_drain_pending(VoxelWorld *world)
{
    if (!world || !g_worker.running)
        return;

    /*
     * Hold world_mu across the scan so the chunks[] array does not get
     * compacted underneath us, and we tag GEN_QUEUED atomically with
     * the (coords, generation) snapshot we enqueue.
     */
    world_lock(world);

    pthread_mutex_lock(&g_worker.queue_mu);

    /* Effective cap: env override if set, else mirror stream_chunks_per_frame.
     * Initial stream (epoch <= 1) bypasses the cap so the world fills in
     * fast on startup; subsequent crossings respect the cap to keep the
     * mesh queue from spiking. 0 means unlimited. */
    int env_cap = gen_pushes_cap_from_env();
    int cap;

    if (env_cap < 0)
        cap = world->stream_chunks_per_frame;
    else
        cap = env_cap;
    if (world->stream_epoch <= 1 || cap <= 0)
        cap = INT_MAX;

    bool pushed_any = false;
    int pushed_count = 0;
    for (int i = 0; i < world->chunk_count; i++) {
        Chunk *chunk = &world->chunks[i];

        if (!(chunk->flags & CHUNK_FLAG_LOADING))
            continue;
        if (chunk->flags & CHUNK_FLAG_GEN_QUEUED)
            continue;
        if (pushed_count >= cap)
            break;

        GenJob job = {
            .chunk_x = chunk->chunk_x,
            .chunk_z = chunk->chunk_z,
            .generation = chunk->generation,
        };

        if (!queue_push_unlocked(&job)) {
            /* Queue full - leave LOADING set without GEN_QUEUED so the
             * next drain pass retries this chunk. */
            break;
        }
        chunk->flags |= CHUNK_FLAG_GEN_QUEUED;
        pushed_any = true;
        pushed_count++;
    }

    if (pushed_any)
        pthread_cond_signal(&g_worker.queue_cv);

    pthread_mutex_unlock(&g_worker.queue_mu);

    world_unlock(world);
}
