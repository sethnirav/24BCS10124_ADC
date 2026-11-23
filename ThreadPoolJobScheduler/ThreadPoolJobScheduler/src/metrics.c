/*
 * metrics.c - Mutex-protected counters, snapshots, binary persistence
 *             and the text performance report.
 */
#include "metrics.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "job.h"

struct Metrics {
    pthread_mutex_t lock;
    uint64_t submitted, rejected, completed, failed, retries;
    uint64_t exec_samples, wait_samples;
    double   total_exec_ms, min_exec_ms, max_exec_ms, total_wait_ms;
    struct timespec started;
};

Metrics *metrics_create(void)
{
    Metrics *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (pthread_mutex_init(&m->lock, NULL) != 0) { free(m); return NULL; }
    timespec_now(&m->started);
    return m;
}

void metrics_destroy(Metrics *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    free(m);
}

#define COUNTER_FN(fn, field)                \
    void fn(Metrics *m)                      \
    {                                        \
        if (!m) return;                      \
        pthread_mutex_lock(&m->lock);        \
        m->field++;                          \
        pthread_mutex_unlock(&m->lock);      \
    }

COUNTER_FN(metrics_record_submitted, submitted)
COUNTER_FN(metrics_record_rejected, rejected)
COUNTER_FN(metrics_record_completed, completed)
COUNTER_FN(metrics_record_failed, failed)
COUNTER_FN(metrics_record_retry, retries)

void metrics_record_execution(Metrics *m, double ms)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    if (m->exec_samples == 0 || ms < m->min_exec_ms) m->min_exec_ms = ms;
    if (m->exec_samples == 0 || ms > m->max_exec_ms) m->max_exec_ms = ms;
    m->total_exec_ms += ms;
    m->exec_samples++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_record_wait(Metrics *m, double ms)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->total_wait_ms += ms;
    m->wait_samples++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_snapshot(Metrics *m, MetricsSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m) return;
    struct timespec now;
    timespec_now(&now);

    pthread_mutex_lock(&m->lock);
    out->submitted     = m->submitted;
    out->rejected      = m->rejected;
    out->completed     = m->completed;
    out->failed        = m->failed;
    out->retries       = m->retries;
    out->exec_samples  = m->exec_samples;
    out->total_exec_ms = m->total_exec_ms;
    out->min_exec_ms   = m->min_exec_ms;
    out->max_exec_ms   = m->max_exec_ms;
    out->wait_samples  = m->wait_samples;
    out->total_wait_ms = m->total_wait_ms;
    out->uptime_s      = timespec_diff_ms(&m->started, &now) / 1000.0;
    pthread_mutex_unlock(&m->lock);

    out->avg_exec_ms = out->exec_samples ? out->total_exec_ms / (double)out->exec_samples : 0.0;
    out->avg_wait_ms = out->wait_samples ? out->total_wait_ms / (double)out->wait_samples : 0.0;
    out->throughput  = out->uptime_s > 0.0 ? (double)(out->completed + out->failed) / out->uptime_s : 0.0;
}

/* ------------------------------------------------- binary persistence */

typedef struct SnapshotFile {
    char            magic[8];
    uint32_t        version;
    int64_t         saved_at;
    MetricsSnapshot snapshot;
} SnapshotFile;

static const char SNAPSHOT_MAGIC[8] = "TPJSMET";
#define SNAPSHOT_VERSION 1u

int metrics_save_binary(const MetricsSnapshot *snap, const char *path)
{
    if (!snap || !path) return -1;
    SnapshotFile f;
    memset(&f, 0, sizeof f);
    memcpy(f.magic, SNAPSHOT_MAGIC, sizeof f.magic);
    f.version  = SNAPSHOT_VERSION;
    f.saved_at = (int64_t)time(NULL);
    f.snapshot = *snap;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t written = fwrite(&f, sizeof f, 1, fp);
    if (fclose(fp) != 0 || written != 1) return -1;
    return 0;
}

int metrics_load_binary(const char *path, MetricsSnapshot *out, time_t *saved_at)
{
    if (!path || !out) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    SnapshotFile f;
    size_t got = fread(&f, sizeof f, 1, fp);
    fclose(fp);
    /* Reject truncated, foreign or old-format files instead of trusting them. */
    if (got != 1 || memcmp(f.magic, SNAPSHOT_MAGIC, sizeof f.magic) != 0 || f.version != SNAPSHOT_VERSION)
        return -1;
    *out = f.snapshot;
    if (saved_at) *saved_at = (time_t)f.saved_at;
    return 0;
}

/* ------------------------------------------------------- text report */

int metrics_write_report(const MetricsSnapshot *s, const RuntimeStats *rt, const char *path)
{
    if (!s || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    char when[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(fp, "==============================================================\n");
    fprintf(fp, " %s v%s - Performance Report\n", APP_NAME, APP_VERSION);
    fprintf(fp, " Generated: %s\n", when);
    fprintf(fp, "==============================================================\n");
    fprintf(fp, " Session uptime            : %.3f s\n", s->uptime_s);
    fprintf(fp, " Jobs submitted (accepted) : %llu\n", (unsigned long long)s->submitted);
    fprintf(fp, " Submissions rejected      : %llu\n", (unsigned long long)s->rejected);
    fprintf(fp, " Jobs completed            : %llu\n", (unsigned long long)s->completed);
    fprintf(fp, " Jobs failed               : %llu\n", (unsigned long long)s->failed);
    if (rt) {
        fprintf(fp, " Jobs pending / queued     : %zu\n", rt->pending);
        fprintf(fp, " Jobs running              : %zu\n", rt->running);
        fprintf(fp, " Worker threads            : %zu\n", rt->workers);
        if (rt->utilization >= 0.0)
            fprintf(fp, " Worker utilization        : %.2f %%\n", rt->utilization * 100.0);
    }
    fprintf(fp, " Retry attempts            : %llu\n", (unsigned long long)s->retries);
    fprintf(fp, " Attempts executed         : %llu\n", (unsigned long long)s->exec_samples);
    fprintf(fp, " Avg execution time        : %.3f ms\n", s->avg_exec_ms);
    fprintf(fp, " Min execution time        : %.3f ms\n", s->min_exec_ms);
    fprintf(fp, " Max execution time        : %.3f ms\n", s->max_exec_ms);
    fprintf(fp, " Avg queue waiting time    : %.3f ms\n", s->avg_wait_ms);
    fprintf(fp, " Throughput                : %.3f jobs/s (finished jobs / uptime)\n", s->throughput);
    fprintf(fp, "==============================================================\n");
    fprintf(fp, " All values were measured with CLOCK_MONOTONIC during this run.\n");

    return fclose(fp) == 0 ? 0 : -1;
}
