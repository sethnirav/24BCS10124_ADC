/*
 * config.c - Load/save the runtime configuration as "key=value" text.
 */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_set_defaults(SchedulerConfig *cfg)
{
    if (!cfg) return;
    cfg->worker_count   = DEFAULT_WORKER_COUNT;
    cfg->max_retries    = DEFAULT_MAX_RETRIES;
    cfg->ageing_ms      = DEFAULT_AGEING_MS;
    cfg->queue_capacity = DEFAULT_QUEUE_CAPACITY;
}

int config_is_valid(const SchedulerConfig *cfg)
{
    return cfg &&
           cfg->worker_count >= MIN_WORKER_COUNT && cfg->worker_count <= MAX_WORKER_COUNT &&
           cfg->max_retries >= 0 && cfg->max_retries <= MAX_RETRY_LIMIT &&
           cfg->ageing_ms >= 0 && cfg->ageing_ms <= MAX_AGEING_MS &&
           cfg->queue_capacity >= 1 && cfg->queue_capacity <= MAX_QUEUE_CAPACITY;
}

/* Strict integer parse: whole string must be a number within [lo, hi]. */
static int parse_long(const char *text, long lo, long hi, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (errno != 0 || end == text) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int config_load(SchedulerConfig *cfg, const char *path, int *warnings)
{
    if (warnings) *warnings = 0;
    if (!cfg || !path) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) return errno == ENOENT ? 1 : -1;

    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) { if (warnings) (*warnings)++; continue; }
        *eq = '\0';
        char *key = p, *value = eq + 1;
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) key[--klen] = '\0';

        long v;
        int ok = -1;
        if (strcmp(key, "worker_count") == 0) {
            ok = parse_long(value, MIN_WORKER_COUNT, MAX_WORKER_COUNT, &v);
            if (ok == 0) cfg->worker_count = (int)v;
        } else if (strcmp(key, "max_retries") == 0) {
            ok = parse_long(value, 0, MAX_RETRY_LIMIT, &v);
            if (ok == 0) cfg->max_retries = (int)v;
        } else if (strcmp(key, "ageing_ms") == 0) {
            ok = parse_long(value, 0, MAX_AGEING_MS, &v);
            if (ok == 0) cfg->ageing_ms = v;
        } else if (strcmp(key, "queue_capacity") == 0) {
            ok = parse_long(value, 1, MAX_QUEUE_CAPACITY, &v);
            if (ok == 0) cfg->queue_capacity = (size_t)v;
        }
        if (ok != 0 && warnings) (*warnings)++;
    }
    int read_error = ferror(fp);
    fclose(fp);
    return read_error ? -1 : 0;
}

int config_save(const SchedulerConfig *cfg, const char *path)
{
    if (!config_is_valid(cfg) || !path) return -1;

    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;

    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    int rc = fprintf(fp,
                     "# %s configuration (edited by the CLI)\n"
                     "worker_count=%d\nmax_retries=%d\nageing_ms=%ld\nqueue_capacity=%zu\n",
                     APP_NAME, cfg->worker_count, cfg->max_retries,
                     cfg->ageing_ms, cfg->queue_capacity);
    if (fclose(fp) != 0 || rc < 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
