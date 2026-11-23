/*
 * scheduler.h - Job scheduler: owns all jobs, the three priority queues,
 *               the scheduling policy, retry handling and metrics.
 *
 * SCHEDULING POLICY
 *   - One FIFO circular queue per priority (HIGH, NORMAL, LOW).
 *   - effective_level = base_level + floor(head_wait_ms / ageing_ms),
 *     capped at HIGH (LOW=0, NORMAL=1, HIGH=2). Only the head of each
 *     queue is considered, so FIFO order inside a priority is preserved.
 *   - The queue whose head has the highest effective level is served.
 *     Ties are broken by the longest waiting head; that is what lets an
 *     aged LOW job eventually beat a stream of fresh HIGH jobs
 *     (starvation prevention). ageing_ms == 0 disables ageing.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "job.h"
#include "logger.h"
#include "metrics.h"

typedef struct Scheduler Scheduler; /* opaque */

/* Callback used by workers to ask "should I stop waiting?" */
typedef bool (*ExitPredicate)(void *ctx);

typedef enum {
    SUBMIT_OK = 0,
    SUBMIT_INVALID,
    SUBMIT_QUEUE_FULL,
    SUBMIT_SHUTDOWN,
    SUBMIT_NOMEM
} SubmitResult;

Scheduler *scheduler_create(const SchedulerConfig *cfg, Logger *logger);
void       scheduler_destroy(Scheduler *s);

/* max_retries == -1 selects the scheduler default. error_buf (optional)
 * receives a human-readable reason when the result is not SUBMIT_OK. */
SubmitResult scheduler_submit(Scheduler *s, const char *name, TaskType type,
                              JobPriority priority, const TaskInput *input,
                              int max_retries, unsigned *out_id,
                              char *error_buf, size_t error_len);

/* Blocks until a job is available, shutdown begins, or should_exit(ctx)
 * returns true. Returns the job already marked RUNNING, or NULL. */
Job *scheduler_next_job(Scheduler *s, ExitPredicate should_exit, void *ctx);

/* Reports the outcome of one attempt (rc == 0 means success). Handles
 * COMPLETED / RETRYING (re-queue) / FAILED transitions, metrics, logs. */
void scheduler_complete_job(Scheduler *s, Job *job, int rc,
                            const char *result, const char *error,
                            double exec_ms);

void   scheduler_wake_all(Scheduler *s);   /* re-evaluate exit predicates */
void   scheduler_wait_idle(Scheduler *s);  /* until no unfinished jobs   */
void   scheduler_shutdown(Scheduler *s);   /* stop handing out jobs      */
bool   scheduler_is_shutdown(Scheduler *s);
size_t scheduler_cancel_pending(Scheduler *s); /* queued -> FAILED       */

/* Read access: data is COPIED out under the lock (safe for the CLI). */
int    scheduler_get_job(Scheduler *s, unsigned id, Job *out);
/* status_filter < 0 returns all jobs. *out is malloc'd (free() it). */
size_t scheduler_snapshot_jobs(Scheduler *s, int status_filter, Job **out);
void   scheduler_status_counts(Scheduler *s, size_t counts[JOB_STATUS_COUNT]);
size_t scheduler_queue_depth(Scheduler *s, JobPriority p);
size_t scheduler_queue_capacity(Scheduler *s);

Metrics *scheduler_metrics(Scheduler *s);

int  scheduler_set_default_retries(Scheduler *s, int retries);
int  scheduler_get_default_retries(Scheduler *s);
int  scheduler_set_ageing_ms(Scheduler *s, long ageing_ms);
long scheduler_get_ageing_ms(Scheduler *s);

/* Pure scheduling policy (exposed for unit testing). Returns the
 * priority index to serve, or -1 if every queue is empty. */
int scheduler_select_priority(const bool nonempty[PRIORITY_COUNT],
                              const double head_wait_ms[PRIORITY_COUNT],
                              long ageing_ms);

const char *submit_result_str(SubmitResult r);

#endif /* SCHEDULER_H */
