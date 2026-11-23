/*
 * worker_pool.c - Reusable POSIX worker threads.
 *
 * Each worker loops:  job = scheduler_next_job()  (sleeps on a condition
 * variable while there is nothing to do) -> task_execute() through the
 * function-pointer registry -> scheduler_complete_job().
 *
 * Retiring a worker: its atomic 'retire' flag is set, the scheduler is
 * woken, the worker's exit predicate returns true and the thread returns;
 * then it is joined. A running job is always finished first.
 */
#include "worker_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "task_handlers.h"

typedef struct Worker {
    int                id;
    pthread_t          thread;
    struct WorkerPool *pool;
    atomic_bool        retire;
    /* fields below are protected by pool->lock */
    WorkerState        state;
    unsigned           current_job;
    uint64_t           jobs_processed;
    double             busy_ms;
    struct timespec    created_at;
} Worker;

struct WorkerPool {
    Scheduler      *sched;
    Worker         *workers[MAX_WORKER_COUNT];
    int             count;
    int             next_worker_id;
    pthread_mutex_t lock;
    /* totals carried over from retired workers (for utilization) */
    double          retired_busy_ms;
    double          retired_alive_ms;
};

static bool worker_should_exit(void *ctx)
{
    Worker *w = ctx;
    return atomic_load(&w->retire);
}

static void *worker_main(void *arg)
{
    Worker     *w    = arg;
    WorkerPool *pool = w->pool;
    char result[JOB_MESSAGE_LEN];
    char error[JOB_MESSAGE_LEN];
    Job *job;

    while ((job = scheduler_next_job(pool->sched, worker_should_exit, w)) != NULL) {
        pthread_mutex_lock(&pool->lock);
        w->state = WORKER_BUSY;
        w->current_job = job->id;
        pthread_mutex_unlock(&pool->lock);

        result[0] = error[0] = '\0';
        struct timespec t0, t1;
        timespec_now(&t0);
        int rc = task_execute(job, result, sizeof result, error, sizeof error);
        timespec_now(&t1);
        double ms = timespec_diff_ms(&t0, &t1);

        scheduler_complete_job(pool->sched, job, rc, result, error, ms);

        pthread_mutex_lock(&pool->lock);
        w->state = WORKER_IDLE;
        w->current_job = 0;
        w->jobs_processed++;
        w->busy_ms += ms;
        pthread_mutex_unlock(&pool->lock);
    }

    pthread_mutex_lock(&pool->lock);
    w->state = WORKER_STOPPED;
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

/* Caller holds pool->lock. */
static Worker *spawn_worker(WorkerPool *pool)
{
    Worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->id   = ++pool->next_worker_id;
    w->pool = pool;
    w->state = WORKER_IDLE;
    atomic_init(&w->retire, false);
    timespec_now(&w->created_at);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* Joins and frees an already-retired worker, accumulating its stats. */
static void reap_worker(WorkerPool *pool, Worker *w)
{
    pthread_join(w->thread, NULL);
    struct timespec now;
    timespec_now(&now);
    pthread_mutex_lock(&pool->lock);
    pool->retired_busy_ms  += w->busy_ms;
    pool->retired_alive_ms += timespec_diff_ms(&w->created_at, &now);
    pthread_mutex_unlock(&pool->lock);
    free(w);
}

/* Retires workers [keep, count): flag them, wake them, join them. */
static void retire_from(WorkerPool *pool, int keep)
{
    Worker *retiring[MAX_WORKER_COUNT];
    int n = 0;

    pthread_mutex_lock(&pool->lock);
    for (int i = keep; i < pool->count; i++) {
        retiring[n++] = pool->workers[i];
        atomic_store(&pool->workers[i]->retire, true);
        pool->workers[i] = NULL;
    }
    pool->count = keep;
    pthread_mutex_unlock(&pool->lock);

    /* Must not hold pool->lock here: workers take it before exiting. */
    scheduler_wake_all(pool->sched);
    for (int i = 0; i < n; i++) reap_worker(pool, retiring[i]);
}

WorkerPool *worker_pool_create(Scheduler *sched, int worker_count)
{
    if (!sched || worker_count < MIN_WORKER_COUNT || worker_count > MAX_WORKER_COUNT)
        return NULL;
    WorkerPool *pool = calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->sched = sched;
    if (pthread_mutex_init(&pool->lock, NULL) != 0) { free(pool); return NULL; }

    if (worker_pool_resize(pool, worker_count) != 0) {
        worker_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

int worker_pool_resize(WorkerPool *pool, int new_count)
{
    if (!pool || new_count < MIN_WORKER_COUNT || new_count > MAX_WORKER_COUNT) return -1;

    pthread_mutex_lock(&pool->lock);
    int current = pool->count;
    if (new_count > current) {
        for (int i = current; i < new_count; i++) {
            Worker *w = spawn_worker(pool);
            if (!w) break;
            pool->workers[i] = w;
            pool->count++;
        }
        int ok = pool->count == new_count;
        pthread_mutex_unlock(&pool->lock);
        return ok ? 0 : -1;
    }
    pthread_mutex_unlock(&pool->lock);

    if (new_count < current) retire_from(pool, new_count);
    return 0;
}

int worker_pool_size(WorkerPool *pool)
{
    if (!pool) return 0;
    pthread_mutex_lock(&pool->lock);
    int n = pool->count;
    pthread_mutex_unlock(&pool->lock);
    return n;
}

size_t worker_pool_info(WorkerPool *pool, WorkerInfo *out, size_t max)
{
    if (!pool || !out) return 0;
    struct timespec now;
    timespec_now(&now);
    pthread_mutex_lock(&pool->lock);
    size_t n = 0;
    for (int i = 0; i < pool->count && n < max; i++, n++) {
        const Worker *w = pool->workers[i];
        out[n].id             = w->id;
        out[n].state          = w->state;
        out[n].current_job    = w->current_job;
        out[n].jobs_processed = w->jobs_processed;
        out[n].busy_ms        = w->busy_ms;
        out[n].alive_ms       = timespec_diff_ms(&w->created_at, &now);
    }
    pthread_mutex_unlock(&pool->lock);
    return n;
}

double worker_pool_utilization(WorkerPool *pool)
{
    if (!pool) return -1.0;
    struct timespec now;
    timespec_now(&now);
    pthread_mutex_lock(&pool->lock);
    double busy = pool->retired_busy_ms, alive = pool->retired_alive_ms;
    for (int i = 0; i < pool->count; i++) {
        busy  += pool->workers[i]->busy_ms;
        alive += timespec_diff_ms(&pool->workers[i]->created_at, &now);
    }
    pthread_mutex_unlock(&pool->lock);
    if (alive <= 0.0) return 0.0;
    double u = busy / alive;
    return u > 1.0 ? 1.0 : u;
}

void worker_pool_destroy(WorkerPool *pool)
{
    if (!pool) return;
    retire_from(pool, 0);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

const char *worker_state_str(WorkerState s)
{
    switch (s) {
    case WORKER_IDLE:    return "IDLE";
    case WORKER_BUSY:    return "BUSY";
    case WORKER_STOPPED:
    default:             return "STOPPED";
    }
}
