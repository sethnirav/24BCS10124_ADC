/*
 * test_fault_handling.c - Retry mechanism, retry limit, failure statistics,
 *                         file logging, corrupted journal / snapshot handling.
 */
#include <stdlib.h>

#include "logger.h"
#include "scheduler.h"
#include "test_common.h"
#include "worker_pool.h"

#define T_EXEC    "logs/test_execution.log"
#define T_ERR     "logs/test_error.log"
#define T_JOURNAL "logs/test_journal.journal"

static TaskInput fail_n(int n)
{
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.failure.fail_attempts = n;
    return in;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[1024];
    int found = 0;
    while (!found && fgets(line, sizeof line, fp)) found = strstr(line, needle) != NULL;
    fclose(fp);
    return found;
}

static void test_retry_and_limits(void)
{
    remove(T_EXEC); remove(T_ERR); remove(T_JOURNAL);
    ensure_directory("logs");
    Logger *lg = logger_open(T_EXEC, T_ERR, T_JOURNAL);
    CHECK(lg != NULL);
    SchedulerConfig c;
    config_set_defaults(&c);
    c.ageing_ms = 0;
    Scheduler *s = scheduler_create(&c, lg);
    WorkerPool *pool = worker_pool_create(s, 3);

    TaskInput transient = fail_n(2), permanent = fail_n(-1), once = fail_n(1);
    unsigned a = 0, b = 0, d = 0;
    scheduler_submit(s, "recovers", TASK_FAILURE_SIM, PRIORITY_HIGH, &transient, 3, &a, NULL, 0);
    scheduler_submit(s, "never", TASK_FAILURE_SIM, PRIORITY_NORMAL, &permanent, 3, &b, NULL, 0);
    scheduler_submit(s, "no-retry", TASK_FAILURE_SIM, PRIORITY_LOW, &once, 0, &d, NULL, 0);
    scheduler_wait_idle(s); /* returns => no infinite retry loop */

    Job j;
    scheduler_get_job(s, a, &j);
    CHECK(j.status == JOB_COMPLETED);
    CHECK(j.retry_count == 2);
    CHECK(j.error[0] == '\0');

    scheduler_get_job(s, b, &j);
    CHECK(j.status == JOB_FAILED);
    CHECK(j.retry_count == 3);            /* exactly the limit */
    CHECK_STR_CONTAINS(j.error, "permanent failure (attempt 4)");

    scheduler_get_job(s, d, &j);
    CHECK(j.status == JOB_FAILED);
    CHECK(j.retry_count == 0);

    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(s), &m);
    CHECK(m.completed == 1);
    CHECK(m.failed == 2);
    CHECK(m.retries == 5);                /* 2 + 3 + 0 */
    CHECK(m.exec_samples == 8);           /* 3 + 4 + 1 attempts */
    CHECK(m.min_exec_ms <= m.avg_exec_ms && m.avg_exec_ms <= m.max_exec_ms);

    scheduler_shutdown(s);
    worker_pool_destroy(pool);
    scheduler_destroy(s);
    logger_close(lg);

    /* Persistent logs */
    CHECK(file_contains(T_EXEC, "retry 1/3 scheduled"));
    CHECK(file_contains(T_EXEC, "'recovers' COMPLETED"));
    CHECK(file_contains(T_ERR, "'never' FAILED after 3 retries"));
    JournalSummary js;
    CHECK(logger_read_journal(T_JOURNAL, &js) == 0);
    CHECK(js.valid_records == 3);
    CHECK(js.completed == 1 && js.failed == 2);
    CHECK(js.malformed_records == 0);
}

static void test_corrupted_journal(void)
{
    const char *path = "logs/test_corrupt.journal";
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL);
    if (!fp) return;
    fputs("J1|1700000000|7|ok-job|CHECKSUM|HIGH|COMPLETED|0|1.000|CRC32=0x1\n", fp);
    fputs("garbage line without separators\n", fp);
    fputs("J1|1700000000|0|bad-id|CHECKSUM|HIGH|COMPLETED|0|1.0|x\n", fp);
    fputs("J1|1700000000|8|too|many|fields|FAILED|0|1.0|x|y\n", fp);
    fputs("J1|1700000000|9|truncated|CALC", fp); /* no newline: interrupted write */
    fclose(fp);

    JournalSummary js;
    CHECK(logger_read_journal(path, &js) == 0);
    CHECK(js.valid_records == 1);
    CHECK(js.malformed_records == 4);
    CHECK(logger_read_journal("logs/definitely_missing.journal", &js) == 0);
    CHECK(js.valid_records == 0);
    remove(path);
}

static void test_metrics_snapshot_file(void)
{
    ensure_directory("reports");
    MetricsSnapshot in, out;
    memset(&in, 0, sizeof in);
    in.completed = 42;
    in.avg_exec_ms = 3.5;
    CHECK(metrics_save_binary(&in, "reports/test_snapshot.bin") == 0);
    time_t when = 0;
    CHECK(metrics_load_binary("reports/test_snapshot.bin", &out, &when) == 0);
    CHECK(out.completed == 42 && out.avg_exec_ms == 3.5 && when > 0);

    FILE *fp = fopen("reports/test_snapshot.bin", "wb"); /* corrupt it */
    if (fp) { fputs("not a snapshot", fp); fclose(fp); }
    CHECK(metrics_load_binary("reports/test_snapshot.bin", &out, NULL) == -1);
    remove("reports/test_snapshot.bin");

    RuntimeStats rt = { 1, 2, 4, 0.5 };
    CHECK(metrics_write_report(&in, &rt, "reports/test_report.txt") == 0);
    CHECK(file_contains("reports/test_report.txt", "Jobs completed            : 42"));
    remove("reports/test_report.txt");
}

static void test_config_roundtrip(void)
{
    SchedulerConfig c, d;
    config_set_defaults(&c);
    c.worker_count = 7;
    c.ageing_ms = 1234;
    CHECK(config_save(&c, "logs/test.conf") == 0);
    config_set_defaults(&d);
    int warnings = -1;
    CHECK(config_load(&d, "logs/test.conf", &warnings) == 0);
    CHECK(d.worker_count == 7 && d.ageing_ms == 1234 && warnings == 0);

    FILE *fp = fopen("logs/test.conf", "w");
    if (fp) { fputs("worker_count=999\nnonsense\nmax_retries=2\n", fp); fclose(fp); }
    config_set_defaults(&d);
    CHECK(config_load(&d, "logs/test.conf", &warnings) == 0);
    CHECK(d.worker_count == DEFAULT_WORKER_COUNT); /* out-of-range ignored */
    CHECK(d.max_retries == 2);
    CHECK(warnings == 2);
    CHECK(config_load(&d, "logs/missing.conf", &warnings) == 1);
    remove("logs/test.conf");
}

int main(void)
{
    printf("test_fault_handling\n");
    RUN(test_retry_and_limits);
    RUN(test_corrupted_journal);
    RUN(test_metrics_snapshot_file);
    RUN(test_config_roundtrip);
    remove(T_EXEC); remove(T_ERR); remove(T_JOURNAL);
    return TEST_SUMMARY();
}
