/*
 * test_scheduler.c - Submission validation, queue-full handling, priority
 *                    order + FIFO, ageing, status transitions, concurrency,
 *                    pool resizing and graceful shutdown.
 */
#include <pthread.h>
#include <stdlib.h>

#include "scheduler.h"
#include "test_common.h"
#include "worker_pool.h"

static SchedulerConfig cfg_with(size_t capacity, long ageing_ms)
{
    SchedulerConfig c;
    config_set_defaults(&c);
    c.queue_capacity = capacity;
    c.ageing_ms = ageing_ms;
    return c;
}

static TaskInput calc(CalcOp op, uint64_t n)
{
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.calc.op = op;
    in.calc.n = n;
    return in;
}

static void test_valid_and_invalid_submission(void)
{
    SchedulerConfig c = cfg_with(8, 0);
    Scheduler *s = scheduler_create(&c, NULL);
    TaskInput ok = calc(CALC_SUM, 10);
    unsigned id = 0;
    char err[256];

    CHECK(scheduler_submit(s, "good", TASK_CALCULATION, PRIORITY_HIGH, &ok, -1, &id, err, sizeof err) == SUBMIT_OK);
    CHECK(id == 1);
    Job j;
    CHECK(scheduler_get_job(s, 1, &j) == 0);
    CHECK(j.status == JOB_QUEUED);
    CHECK(j.max_retries == DEFAULT_MAX_RETRIES);
    CHECK(scheduler_get_job(s, 99, &j) != 0);

    CHECK(scheduler_submit(s, "", TASK_CALCULATION, PRIORITY_HIGH, &ok, -1, NULL, err, sizeof err) == SUBMIT_INVALID);
    CHECK(scheduler_submit(s, NULL, TASK_CALCULATION, PRIORITY_HIGH, &ok, -1, NULL, err, sizeof err) == SUBMIT_INVALID);
    CHECK(scheduler_submit(s, "t", (TaskType)42, PRIORITY_HIGH, &ok, -1, NULL, err, sizeof err) == SUBMIT_INVALID);
    CHECK(scheduler_submit(s, "p", TASK_CALCULATION, (JobPriority)7, &ok, -1, NULL, err, sizeof err) == SUBMIT_INVALID);
    CHECK(scheduler_submit(s, "r", TASK_CALCULATION, PRIORITY_LOW, &ok, MAX_RETRY_LIMIT + 1, NULL, err, sizeof err) == SUBMIT_INVALID);
    TaskInput big = calc(CALC_FACTORIAL, 21);
    CHECK(scheduler_submit(s, "f", TASK_CALCULATION, PRIORITY_LOW, &big, -1, NULL, err, sizeof err) == SUBMIT_INVALID);
    CHECK_STR_CONTAINS(err, "factorial");
    TaskInput empty;
    memset(&empty, 0, sizeof empty);
    CHECK(scheduler_submit(s, "c", TASK_CHECKSUM, PRIORITY_LOW, &empty, -1, NULL, err, sizeof err) == SUBMIT_INVALID);

    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(s), &m);
    CHECK(m.submitted == 1);
    CHECK(m.rejected == 7);
    scheduler_destroy(s);
}

static void test_queue_full(void)
{
    SchedulerConfig c = cfg_with(2, 0);
    Scheduler *s = scheduler_create(&c, NULL);
    TaskInput in = calc(CALC_SUM, 1);
    CHECK(scheduler_submit(s, "a", TASK_CALCULATION, PRIORITY_HIGH, &in, -1, NULL, NULL, 0) == SUBMIT_OK);
    CHECK(scheduler_submit(s, "b", TASK_CALCULATION, PRIORITY_HIGH, &in, -1, NULL, NULL, 0) == SUBMIT_OK);
    CHECK(scheduler_submit(s, "c", TASK_CALCULATION, PRIORITY_HIGH, &in, -1, NULL, NULL, 0) == SUBMIT_QUEUE_FULL);
    /* other priorities have their own capacity */
    CHECK(scheduler_submit(s, "d", TASK_CALCULATION, PRIORITY_LOW, &in, -1, NULL, NULL, 0) == SUBMIT_OK);
    CHECK(scheduler_queue_depth(s, PRIORITY_HIGH) == 2);
    scheduler_destroy(s);
}

static void test_priority_order_and_fifo(void)
{
    SchedulerConfig c = cfg_with(16, 0); /* ageing off: strict priority */
    Scheduler *s = scheduler_create(&c, NULL);
    TaskInput in = calc(CALC_SUM, 1);
    const char *names[] = { "low1", "norm1", "high1", "low2", "high2", "norm2" };
    JobPriority prios[] = { PRIORITY_LOW, PRIORITY_NORMAL, PRIORITY_HIGH, PRIORITY_LOW, PRIORITY_HIGH, PRIORITY_NORMAL };
    for (int i = 0; i < 6; i++)
        scheduler_submit(s, names[i], TASK_CALCULATION, prios[i], &in, -1, NULL, NULL, 0);

    const char *expected[] = { "high1", "high2", "norm1", "norm2", "low1", "low2" };
    for (int i = 0; i < 6; i++) {
        Job *j = scheduler_next_job(s, NULL, NULL);
        CHECK(j != NULL && strcmp(j->name, expected[i]) == 0);
        CHECK(j && j->status == JOB_RUNNING);
        if (j) scheduler_complete_job(s, j, 0, "ok", "", 0.1);
    }
    size_t counts[JOB_STATUS_COUNT];
    scheduler_status_counts(s, counts);
    CHECK(counts[JOB_COMPLETED] == 6);
    scheduler_destroy(s);
}

static void test_ageing_policy(void)
{
    bool all[PRIORITY_COUNT] = { true, true, true };
    double fresh[PRIORITY_COUNT] = { 0, 0, 0 };
    CHECK(scheduler_select_priority(all, fresh, 1000) == PRIORITY_HIGH);

    /* LOW waited 2.5 thresholds -> level 2 (HIGH) and waited longest */
    double aged[PRIORITY_COUNT] = { 2500, 100, 50 };
    CHECK(scheduler_select_priority(all, aged, 1000) == PRIORITY_LOW);
    /* ageing disabled -> strict priority again */
    CHECK(scheduler_select_priority(all, aged, 0) == PRIORITY_HIGH);

    /* NORMAL aged by one level ties HIGH, but waited longer */
    double norm_aged[PRIORITY_COUNT] = { 0, 1200, 10 };
    CHECK(scheduler_select_priority(all, norm_aged, 1000) == PRIORITY_NORMAL);

    bool only_low[PRIORITY_COUNT] = { true, false, false };
    CHECK(scheduler_select_priority(only_low, fresh, 1000) == PRIORITY_LOW);
    bool none[PRIORITY_COUNT] = { false, false, false };
    CHECK(scheduler_select_priority(none, fresh, 1000) == -1);
}

static void test_status_transitions(void)
{
    CHECK(job_transition_valid(JOB_PENDING, JOB_QUEUED));
    CHECK(job_transition_valid(JOB_QUEUED, JOB_RUNNING));
    CHECK(job_transition_valid(JOB_RUNNING, JOB_COMPLETED));
    CHECK(job_transition_valid(JOB_RUNNING, JOB_RETRYING));
    CHECK(job_transition_valid(JOB_RETRYING, JOB_RUNNING));
    CHECK(job_transition_valid(JOB_RUNNING, JOB_FAILED));
    CHECK(!job_transition_valid(JOB_PENDING, JOB_RUNNING));
    CHECK(!job_transition_valid(JOB_COMPLETED, JOB_RUNNING));
    CHECK(!job_transition_valid(JOB_FAILED, JOB_RETRYING));
    CHECK(!job_transition_valid(JOB_QUEUED, JOB_COMPLETED));

    TaskInput in = calc(CALC_SUM, 1);
    Job *j = job_create(1, "t", TASK_CALCULATION, PRIORITY_LOW, &in, 1);
    CHECK(j->status == JOB_PENDING);
    CHECK(job_set_status(j, JOB_COMPLETED) == -1);
    CHECK(j->status == JOB_PENDING);
    CHECK(job_set_status(j, JOB_QUEUED) == 0);
    job_destroy(j);
}

/* Four threads each take a job and wait at a barrier; while they hold
 * their jobs the main thread must observe exactly 4 RUNNING jobs. */
typedef struct { Scheduler *s; pthread_barrier_t *b; } HoldArg;

static void *hold_job(void *p)
{
    HoldArg *a = p;
    Job *j = scheduler_next_job(a->s, NULL, NULL);
    pthread_barrier_wait(a->b); /* 1: everyone holds a job */
    pthread_barrier_wait(a->b); /* 2: main has inspected   */
    if (j) scheduler_complete_job(a->s, j, 0, "done", "", 1.0);
    return NULL;
}

static void test_concurrent_execution(void)
{
    SchedulerConfig c = cfg_with(16, 0);
    Scheduler *s = scheduler_create(&c, NULL);
    TaskInput in = calc(CALC_SUM, 5);
    for (int i = 0; i < 4; i++) scheduler_submit(s, "conc", TASK_CALCULATION, PRIORITY_NORMAL, &in, -1, NULL, NULL, 0);

    pthread_barrier_t b;
    pthread_barrier_init(&b, NULL, 5);
    pthread_t t[4];
    HoldArg arg = { s, &b };
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hold_job, &arg);
    pthread_barrier_wait(&b);
    size_t counts[JOB_STATUS_COUNT];
    scheduler_status_counts(s, counts);
    CHECK(counts[JOB_RUNNING] == 4);
    pthread_barrier_wait(&b);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    scheduler_status_counts(s, counts);
    CHECK(counts[JOB_COMPLETED] == 4);
    pthread_barrier_destroy(&b);
    scheduler_destroy(s);
}

static void test_pool_runs_jobs_and_resizes(void)
{
    SchedulerConfig c = cfg_with(256, 1000);
    Scheduler *s = scheduler_create(&c, NULL);
    WorkerPool *pool = worker_pool_create(s, 2);
    CHECK(pool != NULL);
    CHECK(worker_pool_create(s, 0) == NULL);
    CHECK(worker_pool_size(pool) == 2);
    CHECK(worker_pool_resize(pool, 6) == 0 && worker_pool_size(pool) == 6);

    TaskInput in = calc(CALC_PRIME_COUNT, 20000);
    for (int i = 0; i < 60; i++)
        CHECK(scheduler_submit(s, "prime", TASK_CALCULATION, (JobPriority)(i % 3), &in, 0, NULL, NULL, 0) == SUBMIT_OK);
    CHECK(worker_pool_resize(pool, 1) == 0 && worker_pool_size(pool) == 1); /* shrink while busy */
    scheduler_wait_idle(s);

    Job *jobs = NULL;
    size_t n = scheduler_snapshot_jobs(s, JOB_COMPLETED, &jobs);
    CHECK(n == 60);
    for (size_t i = 0; i < n; i++) CHECK_STR_CONTAINS(jobs[i].result, "= 2262"); /* pi(20000) */
    free(jobs);

    WorkerInfo info[MAX_WORKER_COUNT];
    CHECK(worker_pool_info(pool, info, MAX_WORKER_COUNT) == 1);
    double u = worker_pool_utilization(pool);
    CHECK(u >= 0.0 && u <= 1.0);
    worker_pool_destroy(pool);
    scheduler_destroy(s);
}

static void test_graceful_shutdown(void)
{
    SchedulerConfig c = cfg_with(64, 0);
    Scheduler *s = scheduler_create(&c, NULL);
    WorkerPool *pool = worker_pool_create(s, 3);
    TaskInput in = calc(CALC_SUM, 100000);
    for (int i = 0; i < 20; i++) scheduler_submit(s, "g", TASK_CALCULATION, PRIORITY_NORMAL, &in, -1, NULL, NULL, 0);
    scheduler_wait_idle(s);
    scheduler_shutdown(s);
    worker_pool_destroy(pool); /* must return: all threads joined */
    CHECK(scheduler_is_shutdown(s));
    CHECK(scheduler_submit(s, "late", TASK_CALCULATION, PRIORITY_HIGH, &in, -1, NULL, NULL, 0) == SUBMIT_SHUTDOWN);
    size_t counts[JOB_STATUS_COUNT];
    scheduler_status_counts(s, counts);
    CHECK(counts[JOB_COMPLETED] == 20);
    scheduler_destroy(s);
}

static void test_shutdown_cancels_queued(void)
{
    SchedulerConfig c = cfg_with(16, 0);
    Scheduler *s = scheduler_create(&c, NULL);
    TaskInput in = calc(CALC_SUM, 1);
    for (int i = 0; i < 5; i++) scheduler_submit(s, "q", TASK_CALCULATION, PRIORITY_LOW, &in, -1, NULL, NULL, 0);
    scheduler_shutdown(s);
    CHECK(scheduler_next_job(s, NULL, NULL) == NULL); /* no jobs after shutdown */
    CHECK(scheduler_cancel_pending(s) == 5);
    size_t counts[JOB_STATUS_COUNT];
    scheduler_status_counts(s, counts);
    CHECK(counts[JOB_FAILED] == 5);
    scheduler_wait_idle(s); /* returns immediately */
    scheduler_destroy(s);
}

int main(void)
{
    printf("test_scheduler\n");
    RUN(test_valid_and_invalid_submission);
    RUN(test_queue_full);
    RUN(test_priority_order_and_fifo);
    RUN(test_ageing_policy);
    RUN(test_status_transitions);
    RUN(test_concurrent_execution);
    RUN(test_pool_runs_jobs_and_resizes);
    RUN(test_graceful_shutdown);
    RUN(test_shutdown_cancels_queued);
    return TEST_SUMMARY();
}
