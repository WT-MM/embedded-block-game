#ifndef THREAD_AFFINITY_H
#define THREAD_AFFINITY_H

/*
 * Runtime CPU affinity helpers. On Linux, VOXEL_PIN_THREADS defaults on and
 * each caller may choose a default CPU plus an env override. On non-Linux
 * hosts these functions are no-ops so laptop builds keep working.
 *
 * Env:
 *   VOXEL_PIN_THREADS=0       disables all pinning
 *   VOXEL_MAIN_CPU=N          main/render thread CPU, default 0
 *   VOXEL_MESH_CPU=N          mesh worker CPU, default 1
 *   VOXEL_GEN_CPU=N           chunk-gen worker CPU, default 1
 *   VOXEL_AFFINITY_LOG=1      logs pinning decisions
 */
void thread_affinity_pin_current(const char *thread_name,
                                 const char *cpu_env_name,
                                 int default_cpu);

#endif
