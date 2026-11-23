/*
 * worker_pool.h - Fixed-size (but reconfigurable) pool of POSIX worker
 *                 threads. Threads are created once and reused: each one
 *                 loops "take job -> run handler -> report result".
 */
#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include <stddef.h>
#include <stdint.h>

#include "scheduler.h"

typedef struct WorkerPool WorkerPool; /* opaque */

typedef enum { WORKER_IDLE = 0, WORKER_BUSY, WORKER_STOPPED } WorkerState;

typedef struct WorkerInfo {
    int         id;
    WorkerState state;
    unsigned    current_job; /* 0 when idle */
    uint64_t    jobs_processed;
    double      busy_ms;
    double      alive_ms;
} WorkerInfo;

WorkerPool *worker_pool_create(Scheduler *sched, int worker_count);

/* Grows by creating threads, shrinks by retiring the newest workers
 * (each finishes its current job first, then is joined). 0 on success. */
int    worker_pool_resize(WorkerPool *pool, int new_count);
int    worker_pool_size(WorkerPool *pool);
size_t worker_pool_info(WorkerPool *pool, WorkerInfo *out, size_t max);

/* busy time / alive time over all workers, in [0,1]. */
double worker_pool_utilization(WorkerPool *pool);

/* Retires and joins every worker. Running jobs finish first. */
void   worker_pool_destroy(WorkerPool *pool);

const char *worker_state_str(WorkerState s);

#endif /* WORKER_POOL_H */
