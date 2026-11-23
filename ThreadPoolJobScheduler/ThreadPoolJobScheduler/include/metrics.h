/*
 * metrics.h - Thread-safe performance counters. All values are measured
 *             from real executions using CLOCK_MONOTONIC.
 */
#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct Metrics Metrics; /* opaque */

typedef struct MetricsSnapshot {
    uint64_t submitted;      /* accepted jobs                     */
    uint64_t rejected;       /* invalid / queue full submissions  */
    uint64_t completed;
    uint64_t failed;
    uint64_t retries;        /* retry attempts scheduled          */
    uint64_t exec_samples;   /* attempts executed                 */
    double   total_exec_ms;
    double   min_exec_ms;
    double   max_exec_ms;
    double   avg_exec_ms;
    uint64_t wait_samples;
    double   total_wait_ms;
    double   avg_wait_ms;
    double   uptime_s;
    double   throughput;     /* finished jobs per second of uptime */
} MetricsSnapshot;

/* Live values that belong to other modules, used for reports. */
typedef struct RuntimeStats {
    size_t pending;   /* QUEUED + RETRYING + PENDING */
    size_t running;
    size_t workers;
    double utilization; /* 0..1, negative if unknown */
} RuntimeStats;

Metrics *metrics_create(void);
void     metrics_destroy(Metrics *m);

void metrics_record_submitted(Metrics *m);
void metrics_record_rejected(Metrics *m);
void metrics_record_completed(Metrics *m);
void metrics_record_failed(Metrics *m);
void metrics_record_retry(Metrics *m);
void metrics_record_execution(Metrics *m, double exec_ms);
void metrics_record_wait(Metrics *m, double wait_ms);

void metrics_snapshot(Metrics *m, MetricsSnapshot *out);

/* Binary snapshot persistence (fwrite/fread with magic + version). */
int metrics_save_binary(const MetricsSnapshot *snap, const char *path);
int metrics_load_binary(const char *path, MetricsSnapshot *out, time_t *saved_at);

/* Human-readable report. 0 on success. */
int metrics_write_report(const MetricsSnapshot *snap, const RuntimeStats *rt,
                         const char *path);

#endif /* METRICS_H */
