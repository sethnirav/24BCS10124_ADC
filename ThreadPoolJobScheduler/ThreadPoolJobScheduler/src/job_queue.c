/*
 * job_queue.c - Bounded circular buffer protected by a mutex, with two
 *               condition variables (not_empty / not_full) so blocking
 *               callers sleep instead of busy-waiting.
 *
 *   head -> index of the oldest item     tail -> next free slot
 *   both advance with (index + 1) % capacity, count tracks the fill level
 */
#include "job_queue.h"

#include <pthread.h>
#include <stdlib.h>

struct JobQueue {
    Job           **items;
    size_t          capacity;
    size_t          head;
    size_t          tail;
    size_t          count;
    bool            closed;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
};

JobQueue *job_queue_create(size_t capacity)
{
    if (capacity == 0) return NULL;

    JobQueue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->items = calloc(capacity, sizeof *q->items);
    if (!q->items) goto fail_items;
    q->capacity = capacity;

    if (pthread_mutex_init(&q->lock, NULL) != 0) goto fail_mutex;
    if (pthread_cond_init(&q->not_empty, NULL) != 0) goto fail_not_empty;
    if (pthread_cond_init(&q->not_full, NULL) != 0) goto fail_not_full;
    return q;

fail_not_full:
    pthread_cond_destroy(&q->not_empty);
fail_not_empty:
    pthread_mutex_destroy(&q->lock);
fail_mutex:
    free(q->items);
fail_items:
    free(q);
    return NULL;
}

void job_queue_destroy(JobQueue *q)
{
    if (!q) return;
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    free(q);
}

/* Caller holds q->lock and has checked there is room. */
static void push_locked(JobQueue *q, Job *job)
{
    q->items[q->tail] = job;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
}

/* Caller holds q->lock and has checked count > 0. */
static Job *pop_locked(JobQueue *q)
{
    Job *job = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    return job;
}

QueueResult job_queue_try_push(JobQueue *q, Job *job)
{
    if (!q || !job) return QUEUE_ERROR;
    QueueResult r;
    pthread_mutex_lock(&q->lock);
    if (q->closed)                     r = QUEUE_CLOSED;
    else if (q->count == q->capacity)  r = QUEUE_FULL;
    else { push_locked(q, job);        r = QUEUE_OK; }
    pthread_mutex_unlock(&q->lock);
    return r;
}

QueueResult job_queue_push(JobQueue *q, Job *job)
{
    if (!q || !job) return QUEUE_ERROR;
    QueueResult r;
    pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity && !q->closed)
        pthread_cond_wait(&q->not_full, &q->lock); /* loop: spurious wakeups */
    if (q->closed) r = QUEUE_CLOSED;
    else { push_locked(q, job); r = QUEUE_OK; }
    pthread_mutex_unlock(&q->lock);
    return r;
}

QueueResult job_queue_try_pop(JobQueue *q, Job **out)
{
    if (!q || !out) return QUEUE_ERROR;
    QueueResult r;
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) { *out = NULL; r = q->closed ? QUEUE_CLOSED : QUEUE_EMPTY; }
    else { *out = pop_locked(q); r = QUEUE_OK; }
    pthread_mutex_unlock(&q->lock);
    return r;
}

QueueResult job_queue_pop(JobQueue *q, Job **out)
{
    if (!q || !out) return QUEUE_ERROR;
    QueueResult r;
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) { *out = NULL; r = QUEUE_CLOSED; }
    else { *out = pop_locked(q); r = QUEUE_OK; }
    pthread_mutex_unlock(&q->lock);
    return r;
}

QueueResult job_queue_peek(JobQueue *q, Job **out)
{
    if (!q || !out) return QUEUE_ERROR;
    QueueResult r;
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) { *out = NULL; r = QUEUE_EMPTY; }
    else { *out = q->items[q->head]; r = QUEUE_OK; }
    pthread_mutex_unlock(&q->lock);
    return r;
}

void job_queue_close(JobQueue *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

size_t job_queue_size(JobQueue *q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);
    size_t n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

size_t job_queue_capacity(const JobQueue *q) { return q ? q->capacity : 0; }
bool   job_queue_is_empty(JobQueue *q)       { return job_queue_size(q) == 0; }
bool   job_queue_is_full(JobQueue *q)        { return q && job_queue_size(q) == q->capacity; }

const char *queue_result_str(QueueResult r)
{
    switch (r) {
    case QUEUE_OK:     return "OK";
    case QUEUE_FULL:   return "FULL";
    case QUEUE_EMPTY:  return "EMPTY";
    case QUEUE_CLOSED: return "CLOSED";
    case QUEUE_ERROR:
    default:           return "ERROR";
    }
}
