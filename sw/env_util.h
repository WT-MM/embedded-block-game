#ifndef ENV_UTIL_H
#define ENV_UTIL_H

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

static inline bool env_value_is_set(const char *value)
{
    return value && value[0] != '\0';
}

static inline bool env_value_is_false(const char *value)
{
    return value && value[0] == '0' && value[1] == '\0';
}

static inline bool env_value_is_true(const char *value)
{
    return env_value_is_set(value) && !env_value_is_false(value);
}

static inline const char *env_get_nonempty(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

    const char *value = getenv(name);

    return env_value_is_set(value) ? value : NULL;
}

static inline const char *env_get_nonempty_fallback(const char *primary,
                                                    const char *fallback)
{
    const char *value = env_get_nonempty(primary);

    return value ? value : env_get_nonempty(fallback);
}

static inline bool env_flag(const char *name, bool default_value)
{
    const char *value = env_get_nonempty(name);

    return value ? env_value_is_true(value) : default_value;
}

static inline bool env_parse_long_value(const char *value, long *out)
{
    char *end = NULL;
    long parsed;

    if (!env_value_is_set(value))
        return false;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0')
        return false;

    *out = parsed;
    return true;
}

static inline int env_int_or_default_value(const char *value,
                                           int default_value,
                                           int min_value,
                                           int max_value)
{
    long parsed;

    if (!env_parse_long_value(value, &parsed) ||
        parsed < min_value ||
        parsed > max_value)
        return default_value;

    return (int)parsed;
}

static inline int env_int_or_default(const char *name,
                                     int default_value,
                                     int min_value,
                                     int max_value)
{
    return env_int_or_default_value(env_get_nonempty(name),
                                    default_value,
                                    min_value,
                                    max_value);
}

static inline int env_int_or_default_fallback(const char *primary,
                                              const char *fallback,
                                              int default_value,
                                              int min_value,
                                              int max_value)
{
    return env_int_or_default_value(env_get_nonempty_fallback(primary, fallback),
                                    default_value,
                                    min_value,
                                    max_value);
}

static inline int env_int_capped_or_default(const char *name,
                                            int default_value,
                                            int min_value,
                                            int max_value)
{
    long parsed;

    if (!env_parse_long_value(env_get_nonempty(name), &parsed) ||
        parsed < min_value)
        return default_value;
    if (parsed > max_value)
        return max_value;

    return (int)parsed;
}

static inline int env_int_clamped(const char *primary,
                                  const char *fallback,
                                  int default_value,
                                  int min_value,
                                  int max_value)
{
    long parsed;

    if (!env_parse_long_value(env_get_nonempty_fallback(primary, fallback),
                              &parsed))
        return default_value;
    if (parsed < min_value)
        return min_value;
    if (parsed > max_value)
        return max_value;

    return (int)parsed;
}

static inline bool env_parse_float_value(const char *value, float *out)
{
    char *end = NULL;
    float parsed;

    if (!env_value_is_set(value))
        return false;

    errno = 0;
    parsed = strtof(value, &end);
    if (errno == ERANGE || end == value || *end != '\0' || parsed != parsed)
        return false;

    *out = parsed;
    return true;
}

static inline float env_float_or_default(const char *name,
                                         float default_value,
                                         float min_value,
                                         float max_value)
{
    float parsed;

    if (!env_parse_float_value(env_get_nonempty(name), &parsed) ||
        parsed < min_value ||
        parsed > max_value)
        return default_value;

    return parsed;
}

#endif /* ENV_UTIL_H */
