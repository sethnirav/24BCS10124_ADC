/*
 * scheduler.c - Job registry, priority scheduling, retry handling.
 *
 * Locking: s->lock protects every Job field that changes after submission,
 * the job registry, the counters and the shutdown flag. Queue mutexes are
 * only ever taken while s->lock is held (fixed order s->lock -> queue lock),
 * so no deadlock is possible. Logging and metrics are done after s->lock is
 * released whenever possible to keep the critical section short.
 */
#include "scheduler.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "job_queue.h"
#include "task_handlers.h"

struct Scheduler {
    JobQueue       *queues[PRIORITY_COUNT];
    size_t          queue_capacity;
    pthread_mutex_t lock;
    pthread_cond_t  work_cond; /* signalled when a job is queued */
    pthread_cond_t  idle_cond; /* broadcast when unfinished hits 0 */
    size_t          queued_total;
    size_t          unfinished;  /* accepted jobs not yet COMPLETED/FAILED */
    bool            shutdown;

    Job           **jobs;        /* dynamic registry of every job (owner) */
    size_t          job_count;
    size_t          job_capacity;
    unsigned        next_id;

    int             default_max_retries;
    long            ageing_ms;

    Metrics        *metrics;
    Logger         *logger;
};

Scheduler *scheduler_create(const SchedulerConfig *cfg, Logger *logger)
{
    SchedulerConfig defaults;
    if (!cfg) { config_set_defaults(&defaults); cfg = &defaults; }
    if (!config_is_valid(cfg)) return NULL;

    Scheduler *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    s->queue_capacity      = cfg->queue_capacity;
    s->default_max_retries = cfg->max_retries;
    s->ageing_ms           = cfg->ageing_ms;
    s->logger              = logger;
    s->next_id             = 1;

    for (int p = 0; p < PRIORITY_COUNT; p++) {
        s->queues[p] = job_queue_create(cfg->queue_capacity);
        if (!s->queues[p]) goto fail;
    }
    s->metrics = metrics_create();
    if (!s->metrics) goto fail;

    if (pthread_mutex_init(&s->lock, NULL) != 0) goto fail;
    if (pthread_cond_init(&s->work_cond, NULL) != 0) {
        pthread_mutex_destroy(&s->lock);
        goto fail;
    }
    if (pthread_cond_init(&s->idle_cond, NULL) != 0) {
        pthread_cond_destroy(&s->work_cond);
        pthread_mutex_destroy(&s->lock);
        goto fail;
    }
    return s;

fail:
    for (int p = 0; p < PRIORITY_COUNT; p++) job_queue_destroy(s->queues[p]);
    metrics_destroy(s->metrics);
    free(s);
    return NULL;
}

void scheduler_destroy(Scheduler *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->job_count; i++) job_destroy(s->jobs[i]);
    free(s->jobs);
    for (int p = 0; p < PRIORITY_COUNT; p++) job_queue_destroy(s->queues[p]);
    metrics_destroy(s->metrics);
    pthread_cond_destroy(&s->idle_cond);
    pthread_cond_destroy(&s->work_cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static void set_error(char *buf, size_t len, const char *msg)
{
    if (buf && len > 0) snprintf(buf, len, "%s", msg);
}

/* Grows the registry with realloc (geometric growth). Caller holds lock. */
static int registry_reserve(Scheduler *s)
{
    if (s->job_count < s->job_capacity) return 0;
    size_t new_cap = s->job_capacity ? s->job_capacity * 2 : 32;
    Job **grown = realloc(s->jobs, new_cap * sizeof *grown);
    if (!grown) return -1; /* old block still valid */
    s->jobs = grown;
    s->job_capacity = new_cap;
    return 0;
}

SubmitResult scheduler_submit(Scheduler *s, const char *name, TaskType type,
                              JobPriority priority, const TaskInput *input,
                              int max_retries, unsigned *out_id,
                              char *error_buf, size_t error_len)
{
    if (!s) return SUBMIT_INVALID;
    char reason[JOB_MESSAGE_LEN] = "";

    if (!name || name[0] == '\0')                     snprintf(reason, sizeof reason, "job name is empty");
    else if (strlen(name) >= JOB_NAME_LEN)            snprintf(reason, sizeof reason, "job name longer than %d characters", JOB_NAME_LEN - 1);
    else if (!task_type_valid((int)type))             snprintf(reason, sizeof reason, "unknown task type %d", (int)type);
    else if (!job_priority_valid((int)priority))      snprintf(reason, sizeof reason, "unknown priority %d", (int)priority);
    else if (max_retries < -1 || max_retries > MAX_RETRY_LIMIT)
        snprintf(reason, sizeof reason, "retry limit must be 0..%d", MAX_RETRY_LIMIT);
    else if (!input)                                  snprintf(reason, sizeof reason, "missing task input");
    else task_validate_input(type, input, reason, sizeof reason);

    if (reason[0] != '\0') {
        metrics_record_rejected(s->metrics);
        logger_log(s->logger, LOG_WARN, "Rejected submission '%s': %s", name ? name : "(null)", reason);
        set_error(error_buf, error_len, reason);
        return SUBMIT_INVALID;
    }

    pthread_mutex_lock(&s->lock);
    if (s->shutdown) {
        pthread_mutex_unlock(&s->lock);
        set_error(error_buf, error_len, "scheduler is shutting down");
        return SUBMIT_SHUTDOWN;
    }
    if (registry_reserve(s) != 0) {
        pthread_mutex_unlock(&s->lock);
        set_error(error_buf, error_len, "out of memory");
        return SUBMIT_NOMEM;
    }
    int retries = max_retries < 0 ? s->default_max_retries : max_retries;
    Job *job = job_create(s->next_id, name, type, priority, input, retries);
    if (!job) {
        pthread_mutex_unlock(&s->lock);
        set_error(error_buf, error_len, "out of memory");
        return SUBMIT_NOMEM;
    }
    if (job_queue_try_push(s->queues[priority], job) != QUEUE_OK) {
        pthread_mutex_unlock(&s->lock);
        job_destroy(job);
        metrics_record_rejected(s->metrics);
        logger_log(s->logger, LOG_WARN, "Rejected '%s': %s queue full (capacity %zu)",
                   name, job_priority_str(priority), s->queue_capacity);
        set_error(error_buf, error_len, "queue for this priority is full");
        return SUBMIT_QUEUE_FULL;
    }
    job_set_status(job, JOB_QUEUED);
    s->jobs[s->job_count++] = job;
    s->next_id++;
    s->queued_total++;
    s->unfinished++;
    unsigned id = job->id;
    pthread_cond_signal(&s->work_cond);
    pthread_mutex_unlock(&s->lock);

    metrics_record_submitted(s->metrics);
    logger_log(s->logger, LOG_INFO, "Job #%u '%s' submitted (type=%s priority=%s max_retries=%d)",
               id, name, task_type_str(type), job_priority_str(priority), retries);
    if (out_id) *out_id = id;
    return SUBMIT_OK;
}

int scheduler_select_priority(const bool nonempty[PRIORITY_COUNT],
                              const double head_wait_ms[PRIORITY_COUNT],
                              long ageing_ms)
{
    int    best = -1;
    long   best_level = -1;
    double best_wait = -1.0;

    for (int p = PRIORITY_HIGH; p >= PRIORITY_LOW; p--) {
        if (!nonempty[p]) continue;
        long level = p;
        if (ageing_ms > 0 && head_wait_ms[p] > 0.0)
            level += (long)(head_wait_ms[p] / (double)ageing_ms);
        if (level > PRIORITY_HIGH) level = PRIORITY_HIGH;

        if (best < 0 || level > best_level ||
            (level == best_level && head_wait_ms[p] > best_wait)) {
            best = p;
            best_level = level;
            best_wait = head_wait_ms[p];
        }
    }
    return best;
}

Job *scheduler_next_job(Scheduler *s, ExitPredicate should_exit, void *ctx)
{
    if (!s) return NULL;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        if (s->shutdown || (should_exit && should_exit(ctx))) {
            pthread_mutex_unlock(&s->lock);
            return NULL;
        }
        if (s->queued_total > 0) break;
        pthread_cond_wait(&s->work_cond, &s->lock); /* no busy waiting */
    }

    struct timespec now;
    timespec_now(&now);
    bool   nonempty[PRIORITY_COUNT];
    double wait_ms[PRIORITY_COUNT];
    for (int p = 0; p < PRIORITY_COUNT; p++) {
        Job *head = NULL;
        nonempty[p] = job_queue_peek(s->queues[p], &head) == QUEUE_OK;
        wait_ms[p]  = nonempty[p] ? timespec_diff_ms(&head->queued_at, &now) : 0.0;
    }

    int  p   = scheduler_select_priority(nonempty, wait_ms, s->ageing_ms);
    Job *job = NULL;
    if (p < 0 || job_queue_try_pop(s->queues[p], &job) != QUEUE_OK || !job) {
        /* Would mean queued_total is out of sync - never expected. */
        pthread_mutex_unlock(&s->lock);
        logger_log(s->logger, LOG_ERROR, "Scheduler inconsistency: queued_total=%zu but queues empty",
                   s->queued_total);
        return NULL;
    }
    s->queued_total--;
    job_set_status(job, JOB_RUNNING);
    job->started_at   = now;
    job->started_wall = time(NULL);
    double waited = wait_ms[p];
    pthread_mutex_unlock(&s->lock);

    metrics_record_wait(s->metrics, waited);
    return job;
}

void scheduler_complete_job(Scheduler *s, Job *job, int rc,
                            const char *result, const char *error,
                            double exec_ms)
{
    if (!s || !job) return;
    enum { OUTCOME_COMPLETED, OUTCOME_RETRY, OUTCOME_FAILED } outcome;

    metrics_record_execution(s->metrics, exec_ms);

    pthread_mutex_lock(&s->lock);
    timespec_now(&job->completed_at);
    job->last_exec_ms   = exec_ms;
    job->total_exec_ms += exec_ms;

    if (rc == 0) {
        job_set_status(job, JOB_COMPLETED);
        snprintf(job->result, sizeof job->result, "%s", result ? result : "");
        job->error[0] = '\0';
        outcome = OUTCOME_COMPLETED;
    } else {
        snprintf(job->error, sizeof job->error, "%s",
                 (error && error[0]) ? error : "task failed without a message");
        if (!s->shutdown && job->retry_count < job->max_retries) {
            job->retry_count++;
            job_set_status(job, JOB_RETRYING);
            timespec_now(&job->queued_at);
            if (job_queue_try_push(s->queues[job->priority], job) == QUEUE_OK) {
                s->queued_total++;
                pthread_cond_signal(&s->work_cond);
                outcome = OUTCOME_RETRY;
            } else {
                job_set_status(job, JOB_FAILED);
                strncat(job->error, " [retry dropped: queue full]",
                        sizeof job->error - strlen(job->error) - 1);
                outcome = OUTCOME_FAILED;
            }
        } else {
            if (s->shutdown && job->retry_count < job->max_retries)
                strncat(job->error, " [retry skipped: shutdown]",
                        sizeof job->error - strlen(job->error) - 1);
            job_set_status(job, JOB_FAILED);
            outcome = OUTCOME_FAILED;
        }
    }
    if (outcome != OUTCOME_RETRY) {
        job->completed_wall = time(NULL);
        s->unfinished--;
        if (s->unfinished == 0) pthread_cond_broadcast(&s->idle_cond);
    }
    Job copy = *job; /* log from a private copy after unlocking */
    pthread_mutex_unlock(&s->lock);

    switch (outcome) {
    case OUTCOME_COMPLETED:
        metrics_record_completed(s->metrics);
        logger_log(s->logger, LOG_INFO, "Job #%u '%s' COMPLETED in %.3f ms: %s",
                   copy.id, copy.name, exec_ms, copy.result);
        logger_journal(s->logger, &copy);
        break;
    case OUTCOME_RETRY:
        metrics_record_retry(s->metrics);
        logger_log(s->logger, LOG_WARN, "Job #%u '%s' failed (%s); retry %d/%d scheduled",
                   copy.id, copy.name, copy.error, copy.retry_count, copy.max_retries);
        break;
    case OUTCOME_FAILED:
        metrics_record_failed(s->metrics);
        logger_log(s->logger, LOG_ERROR, "Job #%u '%s' FAILED after %d retr%s: %s",
                   copy.id, copy.name, copy.retry_count,
                   copy.retry_count == 1 ? "y" : "ies", copy.error);
        logger_journal(s->logger, &copy);
        break;
    }
}

void scheduler_wake_all(Scheduler *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->work_cond);
    pthread_mutex_unlock(&s->lock);
}

void scheduler_wait_idle(Scheduler *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    while (s->unfinished > 0 && !s->shutdown)
        pthread_cond_wait(&s->idle_cond, &s->lock);
    pthread_mutex_unlock(&s->lock);
}

void scheduler_shutdown(Scheduler *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    bool was = s->shutdown;
    s->shutdown = true;
    pthread_cond_broadcast(&s->work_cond);
    pthread_cond_broadcast(&s->idle_cond);
    pthread_mutex_unlock(&s->lock);
    if (!was) logger_log(s->logger, LOG_INFO, "Scheduler shutdown requested");
}

bool scheduler_is_shutdown(Scheduler *s)
{
    if (!s) return true;
    pthread_mutex_lock(&s->lock);
    bool v = s->shutdown;
    pthread_mutex_unlock(&s->lock);
    return v;
}

size_t scheduler_cancel_pending(Scheduler *s)
{
    if (!s) return 0;
    size_t cancelled = 0;
    pthread_mutex_lock(&s->lock);
    for (int p = 0; p < PRIORITY_COUNT; p++) {
        Job *job = NULL;
        while (job_queue_try_pop(s->queues[p], &job) == QUEUE_OK && job) {
            job_set_status(job, JOB_FAILED);
            snprintf(job->error, sizeof job->error, "cancelled: scheduler shut down before execution");
            job->completed_wall = time(NULL);
            s->queued_total--;
            s->unfinished--;
            cancelled++;
            metrics_record_failed(s->metrics);
            logger_journal(s->logger, job); /* logger never takes s->lock */
        }
    }
    if (s->unfinished == 0) pthread_cond_broadcast(&s->idle_cond);
    pthread_mutex_unlock(&s->lock);
    if (cancelled)
        logger_log(s->logger, LOG_WARN, "%zu queued job(s) cancelled at shutdown", cancelled);
    return cancelled;
}

int scheduler_get_job(Scheduler *s, unsigned id, Job *out)
{
    if (!s || !out) return -1;
    int rc = -1;
    pthread_mutex_lock(&s->lock);
    /* IDs are sequential starting at 1, so the job sits at index id-1. */
    if (id >= 1 && id <= s->job_count && s->jobs[id - 1]->id == id) {
        *out = *s->jobs[id - 1];
        rc = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

size_t scheduler_snapshot_jobs(Scheduler *s, int status_filter, Job **out)
{
    if (!out) return 0;
    *out = NULL;
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    size_t n = 0;
    for (size_t i = 0; i < s->job_count; i++)
        if (status_filter < 0 || (int)s->jobs[i]->status == status_filter) n++;
    Job *copies = n ? malloc(n * sizeof *copies) : NULL;
    if (n && !copies) { pthread_mutex_unlock(&s->lock); return 0; }
    size_t k = 0;
    for (size_t i = 0; i < s->job_count && k < n; i++)
        if (status_filter < 0 || (int)s->jobs[i]->status == status_filter) copies[k++] = *s->jobs[i];
    pthread_mutex_unlock(&s->lock);
    *out = copies;
    return k;
}

void scheduler_status_counts(Scheduler *s, size_t counts[JOB_STATUS_COUNT])
{
    for (int i = 0; i < JOB_STATUS_COUNT; i++) counts[i] = 0;
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->job_count; i++) counts[s->jobs[i]->status]++;
    pthread_mutex_unlock(&s->lock);
}

size_t scheduler_queue_depth(Scheduler *s, JobPriority p)
{
    if (!s || !job_priority_valid((int)p)) return 0;
    return job_queue_size(s->queues[p]);
}

size_t scheduler_queue_capacity(Scheduler *s) { return s ? s->queue_capacity : 0; }
Metrics *scheduler_metrics(Scheduler *s)      { return s ? s->metrics : NULL; }

int scheduler_set_default_retries(Scheduler *s, int retries)
{
    if (!s || retries < 0 || retries > MAX_RETRY_LIMIT) return -1;
    pthread_mutex_lock(&s->lock);
    s->default_max_retries = retries;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int scheduler_get_default_retries(Scheduler *s)
{
    if (!s) return -1;
    pthread_mutex_lock(&s->lock);
    int v = s->default_max_retries;
    pthread_mutex_unlock(&s->lock);
    return v;
}

int scheduler_set_ageing_ms(Scheduler *s, long ageing_ms)
{
    if (!s || ageing_ms < 0 || ageing_ms > MAX_AGEING_MS) return -1;
    pthread_mutex_lock(&s->lock);
    s->ageing_ms = ageing_ms;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

long scheduler_get_ageing_ms(Scheduler *s)
{
    if (!s) return -1;
    pthread_mutex_lock(&s->lock);
    long v = s->ageing_ms;
    pthread_mutex_unlock(&s->lock);
    return v;
}

const char *submit_result_str(SubmitResult r)
{
    switch (r) {
    case SUBMIT_OK:         return "OK";
    case SUBMIT_INVALID:    return "INVALID";
    case SUBMIT_QUEUE_FULL: return "QUEUE_FULL";
    case SUBMIT_SHUTDOWN:   return "SHUTDOWN";
    case SUBMIT_NOMEM:
    default:                return "NO_MEMORY";
    }
}
