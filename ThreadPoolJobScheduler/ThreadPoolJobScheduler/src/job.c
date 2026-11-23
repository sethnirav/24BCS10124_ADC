/*
 * job.c - Job creation, lifecycle transitions and string helpers.
 */
#include "job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const STATUS_NAMES[JOB_STATUS_COUNT] = {
    "PENDING", "QUEUED", "RUNNING", "COMPLETED", "FAILED", "RETRYING"
};
static const char *const PRIORITY_NAMES[PRIORITY_COUNT] = { "LOW", "NORMAL", "HIGH" };
static const char *const TYPE_NAMES[TASK_TYPE_COUNT] = {
    "CHECKSUM", "VALIDATION", "FILE_PROC", "CALCULATION", "FAIL_SIM"
};
static const char *const CALC_NAMES[CALC_OP_COUNT] = { "sum", "factorial", "fibonacci", "prime_count" };

void timespec_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

double timespec_diff_ms(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1.0e6;
}

bool job_priority_valid(int priority) { return priority >= 0 && priority < PRIORITY_COUNT; }
bool task_type_valid(int type)        { return type >= 0 && type < TASK_TYPE_COUNT; }

Job *job_create(unsigned id, const char *name, TaskType type,
                JobPriority priority, const TaskInput *input, int max_retries)
{
    if (!name || name[0] == '\0' || !task_type_valid((int)type) ||
        !job_priority_valid((int)priority) || max_retries < 0 || max_retries > MAX_RETRY_LIMIT)
        return NULL;

    Job *job = calloc(1, sizeof *job); /* zeroed: all strings empty, times 0 */
    if (!job) return NULL;

    job->id = id;
    snprintf(job->name, sizeof job->name, "%s", name);
    job->type        = type;
    job->priority    = priority;
    job->status      = JOB_PENDING;
    job->max_retries = max_retries;
    if (input) job->input = *input;

    job->submitted_wall = time(NULL);
    timespec_now(&job->submitted_at);
    job->queued_at = job->submitted_at;
    return job;
}

void job_destroy(Job *job)
{
    free(job);
}

bool job_transition_valid(JobStatus from, JobStatus to)
{
    switch (from) {
    case JOB_PENDING:  return to == JOB_QUEUED  || to == JOB_FAILED;
    case JOB_QUEUED:   return to == JOB_RUNNING || to == JOB_FAILED;
    case JOB_RETRYING: return to == JOB_RUNNING || to == JOB_FAILED;
    case JOB_RUNNING:  return to == JOB_COMPLETED || to == JOB_FAILED || to == JOB_RETRYING;
    case JOB_COMPLETED:
    case JOB_FAILED:
    default:           return false; /* terminal states */
    }
}

int job_set_status(Job *job, JobStatus to)
{
    if (!job || !job_transition_valid(job->status, to)) return -1;
    job->status = to;
    return 0;
}

const char *job_status_str(JobStatus s)
{
    return ((int)s >= 0 && s < JOB_STATUS_COUNT) ? STATUS_NAMES[s] : "UNKNOWN";
}

const char *job_priority_str(JobPriority p)
{
    return job_priority_valid((int)p) ? PRIORITY_NAMES[p] : "UNKNOWN";
}

const char *task_type_str(TaskType t)
{
    return task_type_valid((int)t) ? TYPE_NAMES[t] : "UNKNOWN";
}

const char *calc_op_str(CalcOp op)
{
    return ((int)op >= 0 && op < CALC_OP_COUNT) ? CALC_NAMES[op] : "unknown";
}
