/*
 * job_queue.h - Bounded, thread-safe circular (ring buffer) queue of Job*.
 *
 * The queue only manages storage and synchronisation; it knows nothing
 * about priorities (see scheduler.c for the scheduling policy).
 * The queue never owns or frees the jobs it stores.
 */
#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "job.h"

typedef struct JobQueue JobQueue; /* opaque */

typedef enum {
    QUEUE_OK = 0,
    QUEUE_FULL,
    QUEUE_EMPTY,
    QUEUE_CLOSED,
    QUEUE_ERROR
} QueueResult;

JobQueue   *job_queue_create(size_t capacity);
void        job_queue_destroy(JobQueue *q);

QueueResult job_queue_try_push(JobQueue *q, Job *job);  /* non-blocking */
QueueResult job_queue_push(JobQueue *q, Job *job);      /* waits while full */
QueueResult job_queue_try_pop(JobQueue *q, Job **out);  /* non-blocking */
QueueResult job_queue_pop(JobQueue *q, Job **out);      /* waits while empty */
QueueResult job_queue_peek(JobQueue *q, Job **out);     /* head, not removed */

/* Wakes every blocked producer/consumer; later pushes return QUEUE_CLOSED,
 * pops keep draining remaining items and then return QUEUE_CLOSED. */
void   job_queue_close(JobQueue *q);

size_t job_queue_size(JobQueue *q);
size_t job_queue_capacity(const JobQueue *q);
bool   job_queue_is_empty(JobQueue *q);
bool   job_queue_is_full(JobQueue *q);

const char *queue_result_str(QueueResult r);

#endif /* JOB_QUEUE_H */
