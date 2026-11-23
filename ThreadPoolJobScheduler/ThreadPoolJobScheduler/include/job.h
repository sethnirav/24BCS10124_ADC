/*
 * job.h - Job data model: states, priorities, task types and lifecycle.
 *
 * Lifecycle (validated by job_transition_valid):
 *
 *   PENDING --> QUEUED --> RUNNING --> COMPLETED
 *                 ^           |
 *                 |           +------> FAILED   (retries exhausted)
 *              RETRYING <-----+                 (failure, retries left)
 *
 *   PENDING / QUEUED / RETRYING may also go to FAILED when the scheduler
 *   is shut down before the job could run (cancellation).
 */
#ifndef JOB_H
#define JOB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "config.h"

typedef enum {
    JOB_PENDING = 0,
    JOB_QUEUED,
    JOB_RUNNING,
    JOB_COMPLETED,
    JOB_FAILED,
    JOB_RETRYING,
    JOB_STATUS_COUNT
} JobStatus;

/* Numeric order matters: a higher value means a more urgent job. */
typedef enum {
    PRIORITY_LOW = 0,
    PRIORITY_NORMAL,
    PRIORITY_HIGH,
    PRIORITY_COUNT
} JobPriority;

typedef enum {
    TASK_CHECKSUM = 0,
    TASK_VALIDATION,
    TASK_FILE_PROCESSING,
    TASK_CALCULATION,
    TASK_FAILURE_SIM,
    TASK_TYPE_COUNT
} TaskType;

typedef enum {
    CALC_SUM = 0,       /* 1 + 2 + ... + n            */
    CALC_FACTORIAL,     /* n!          (n <= 20)      */
    CALC_FIBONACCI,     /* F(n)        (n <= 93)      */
    CALC_PRIME_COUNT,   /* primes <= n (sieve)        */
    CALC_OP_COUNT
} CalcOp;

/*
 * Task input. Only one interpretation is meaningful for a given task
 * type, so a union keeps the Job structure compact:
 *   CHECKSUM / VALIDATION / FILE_PROCESSING -> text
 *   CALCULATION                             -> calc
 *   FAILURE_SIM                             -> failure
 */
typedef union TaskInput {
    char text[JOB_INPUT_LEN];
    struct {
        CalcOp   op;
        uint64_t n;
    } calc;
    struct {
        int fail_attempts; /* number of attempts that fail; -1 = always */
    } failure;
} TaskInput;

typedef struct Job {
    unsigned    id;
    char        name[JOB_NAME_LEN];
    TaskType    type;
    JobPriority priority;
    JobStatus   status;
    TaskInput   input;

    /* Wall-clock times for display (0 = not yet happened) */
    time_t submitted_wall;
    time_t started_wall;
    time_t completed_wall;

    /* Monotonic times for measurement */
    struct timespec submitted_at;
    struct timespec queued_at;    /* last time it entered a queue */
    struct timespec started_at;   /* start of the latest attempt  */
    struct timespec completed_at; /* end of the latest attempt    */

    double last_exec_ms;  /* duration of the latest attempt   */
    double total_exec_ms; /* sum over all attempts            */

    int retry_count;
    int max_retries;

    char result[JOB_MESSAGE_LEN];
    char error[JOB_MESSAGE_LEN];
} Job;

Job *job_create(unsigned id, const char *name, TaskType type,
                JobPriority priority, const TaskInput *input, int max_retries);
void job_destroy(Job *job);

bool job_transition_valid(JobStatus from, JobStatus to);
/* Applies the transition if valid; returns 0, or -1 if rejected. */
int  job_set_status(Job *job, JobStatus to);

const char *job_status_str(JobStatus status);
const char *job_priority_str(JobPriority priority);
const char *task_type_str(TaskType type);
const char *calc_op_str(CalcOp op);

bool job_priority_valid(int priority);
bool task_type_valid(int type);

/* Monotonic clock helpers */
void   timespec_now(struct timespec *ts);
double timespec_diff_ms(const struct timespec *start, const struct timespec *end);

#endif /* JOB_H */
