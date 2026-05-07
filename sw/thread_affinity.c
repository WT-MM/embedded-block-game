#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "thread_affinity.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

static bool env_value_is_false(const char *value)
{
    return value &&
           (strcmp(value, "0") == 0 ||
            strcmp(value, "false") == 0 ||
            strcmp(value, "off") == 0 ||
            strcmp(value, "no") == 0);
}

static bool env_value_is_true(const char *value)
{
    return value &&
           value[0] != '\0' &&
           !env_value_is_false(value);
}

#ifdef __linux__
static bool affinity_enabled(void)
{
    const char *value = getenv("VOXEL_PIN_THREADS");

    if (!value || value[0] == '\0')
        return true;
    return !env_value_is_false(value);
}
#endif

static bool affinity_log_enabled(void)
{
    const char *value = getenv("VOXEL_AFFINITY_LOG");

    if (value && value[0] != '\0')
        return env_value_is_true(value);
    return env_value_is_true(getenv("DEBUG"));
}

#ifdef __linux__
static int read_cpu_env(const char *name, int default_cpu)
{
    const char *value = name ? getenv(name) : NULL;
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0')
        return default_cpu;
    if (env_value_is_false(value) || strcmp(value, "-1") == 0)
        return -1;

    parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed < -1 || parsed > 1023)
        return default_cpu;
    return (int)parsed;
}
#endif

void thread_affinity_pin_current(const char *thread_name,
                                 const char *cpu_env_name,
                                 int default_cpu)
{
    bool log_enabled = affinity_log_enabled();

    if (!thread_name)
        thread_name = "thread";

#ifndef __linux__
    (void)cpu_env_name;
    (void)default_cpu;
    if (log_enabled)
        fprintf(stderr, "affinity: %s pinning unavailable on this host\n",
                thread_name);
    return;
#else
    if (!affinity_enabled()) {
        if (log_enabled)
            fprintf(stderr, "affinity: %s pinning disabled\n", thread_name);
        return;
    }

    int cpu = read_cpu_env(cpu_env_name, default_cpu);
    if (cpu < 0) {
        if (log_enabled)
            fprintf(stderr, "affinity: %s pinning off\n", thread_name);
        return;
    }

    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0 && cpu >= cpu_count) {
        if (log_enabled) {
            fprintf(stderr,
                    "affinity: %s requested CPU %d but only %ld online\n",
                    thread_name, cpu, cpu_count);
        }
        return;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        if (log_enabled) {
            fprintf(stderr, "affinity: %s CPU %d failed: %s\n",
                    thread_name, cpu, strerror(errno));
        }
        return;
    }

    if (log_enabled)
        fprintf(stderr, "affinity: %s pinned to CPU %d\n", thread_name, cpu);
#endif
}
