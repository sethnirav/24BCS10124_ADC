/*
 * test_queue.c - Circular queue: empty/full, FIFO, wrap-around, closing,
 *                and multi-threaded producer/consumer synchronisation.
 */
#include <pthread.h>
#include <stdlib.h>

#include "job_queue.h"
#include "test_common.h"

static Job *make(unsigned id)
{
    TaskInput in = { .text = "x" };
    return job_create(id, "q", TASK_CHECKSUM, PRIORITY_NORMAL, &in, 0);
}

static void test_empty_and_full(void)
{
    JobQueue *q = job_queue_create(3);
    CHECK(q != NULL);
    CHECK(job_queue_create(0) == NULL);
    Job *out = (Job *)1;
    CHECK(job_queue_try_pop(q, &out) == QUEUE_EMPTY);
    CHECK(out == NULL);
    CHECK(job_queue_peek(q, &out) == QUEUE_EMPTY);
    CHECK(job_queue_is_empty(q));

    Job *j[4] = { make(1), make(2), make(3), make(4) };
    for (int i = 0; i < 3; i++) CHECK(job_queue_try_push(q, j[i]) == QUEUE_OK);
    CHECK(job_queue_is_full(q));
    CHECK(job_queue_try_push(q, j[3]) == QUEUE_FULL);
    CHECK(job_queue_size(q) == 3);
    CHECK(job_queue_try_push(q, NULL) == QUEUE_ERROR);
    CHECK(job_queue_try_pop(NULL, &out) == QUEUE_ERROR);

    for (int i = 0; i < 4; i++) job_destroy(j[i]);
    job_queue_destroy(q);
}

static void test_fifo_and_wraparound(void)
{
    JobQueue *q = job_queue_create(3);
    Job *j[6];
    for (unsigned i = 0; i < 6; i++) j[i] = make(i + 1);
    Job *out = NULL;

    job_queue_try_push(q, j[0]);
    job_queue_try_push(q, j[1]);
    job_queue_try_push(q, j[2]);
    CHECK(job_queue_try_pop(q, &out) == QUEUE_OK && out->id == 1);
    CHECK(job_queue_try_pop(q, &out) == QUEUE_OK && out->id == 2);
    /* tail wraps to index 0 and 1 now */
    CHECK(job_queue_try_push(q, j[3]) == QUEUE_OK);
    CHECK(job_queue_try_push(q, j[4]) == QUEUE_OK);
    CHECK(job_queue_try_push(q, j[5]) == QUEUE_FULL);
    CHECK(job_queue_peek(q, &out) == QUEUE_OK && out->id == 3);
    unsigned expect[] = { 3, 4, 5 };
    for (int i = 0; i < 3; i++) CHECK(job_queue_try_pop(q, &out) == QUEUE_OK && out->id == expect[i]);
    CHECK(job_queue_try_pop(q, &out) == QUEUE_EMPTY);

    for (int i = 0; i < 6; i++) job_destroy(j[i]);
    job_queue_destroy(q);
}

static void test_close_semantics(void)
{
    JobQueue *q = job_queue_create(2);
    Job *a = make(1), *b = make(2), *out = NULL;
    job_queue_try_push(q, a);
    job_queue_close(q);
    CHECK(job_queue_try_push(q, b) == QUEUE_CLOSED);
    CHECK(job_queue_push(q, b) == QUEUE_CLOSED);
    CHECK(job_queue_pop(q, &out) == QUEUE_OK && out == a); /* drains */
    CHECK(job_queue_pop(q, &out) == QUEUE_CLOSED);
    CHECK(job_queue_try_pop(q, &out) == QUEUE_CLOSED);
    job_destroy(a);
    job_destroy(b);
    job_queue_destroy(q);
}

/* ----------------------------------------------- concurrent workload */

#define PER_PRODUCER 2000
#define PRODUCERS    3
#define CONSUMERS    3

typedef struct { JobQueue *q; Job **jobs; int base; } ProducerArg;
typedef struct { JobQueue *q; unsigned long long id_sum; int count; } ConsumerArg;

static void *producer(void *p)
{
    ProducerArg *a = p;
    for (int i = 0; i < PER_PRODUCER; i++)
        if (job_queue_push(a->q, a->jobs[a->base + i]) != QUEUE_OK) break;
    return NULL;
}

static void *consumer(void *p)
{
    ConsumerArg *a = p;
    Job *j;
    while (job_queue_pop(a->q, &j) == QUEUE_OK) { a->id_sum += j->id; a->count++; }
    return NULL;
}

static void test_producer_consumer(void)
{
    JobQueue *q = job_queue_create(8); /* small: forces both waits */
    int total = PER_PRODUCER * PRODUCERS;
    Job **jobs = calloc((size_t)total, sizeof *jobs);
    unsigned long long expected = 0;
    for (int i = 0; i < total; i++) { jobs[i] = make((unsigned)i + 1); expected += (unsigned)i + 1; }

    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    ProducerArg pa[PRODUCERS];
    ConsumerArg ca[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        ca[i] = (ConsumerArg){ q, 0, 0 };
        CHECK(pthread_create(&ct[i], NULL, consumer, &ca[i]) == 0);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pa[i] = (ProducerArg){ q, jobs, i * PER_PRODUCER };
        CHECK(pthread_create(&pt[i], NULL, producer, &pa[i]) == 0);
    }
    for (int i = 0; i < PRODUCERS; i++) pthread_join(pt[i], NULL);
    job_queue_close(q); /* wakes consumers blocked on an empty queue */
    unsigned long long sum = 0;
    int count = 0;
    for (int i = 0; i < CONSUMERS; i++) { pthread_join(ct[i], NULL); sum += ca[i].id_sum; count += ca[i].count; }

    CHECK(count == total);   /* nothing lost or duplicated */
    CHECK(sum == expected);
    CHECK(job_queue_size(q) == 0);

    for (int i = 0; i < total; i++) job_destroy(jobs[i]);
    free(jobs);
    job_queue_destroy(q);
}

int main(void)
{
    printf("test_queue\n");
    RUN(test_empty_and_full);
    RUN(test_fifo_and_wraparound);
    RUN(test_close_semantics);
    RUN(test_producer_consumer);
    return TEST_SUMMARY();
}
