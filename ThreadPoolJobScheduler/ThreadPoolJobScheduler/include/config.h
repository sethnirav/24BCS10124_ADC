/*
 * config.h - Compile-time limits, file locations and the runtime
 *            scheduler configuration (loaded from / saved to scheduler.conf).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define APP_NAME    "Thread Pool Job Scheduler"
#define APP_VERSION "1.0.0"

/* Worker pool limits */
#define DEFAULT_WORKER_COUNT 4
#define MIN_WORKER_COUNT     1
#define MAX_WORKER_COUNT     64

/* Queue / retry / ageing defaults */
#define DEFAULT_QUEUE_CAPACITY 128   /* capacity of EACH priority queue */
#define MAX_QUEUE_CAPACITY     100000
#define DEFAULT_MAX_RETRIES    3
#define MAX_RETRY_LIMIT        10
#define DEFAULT_AGEING_MS      5000L /* wait time that promotes a job one level */
#define MAX_AGEING_MS          3600000L

/* String buffer sizes */
#define JOB_NAME_LEN    64
#define JOB_INPUT_LEN   256
#define JOB_MESSAGE_LEN 256

/* Persistent file locations (relative to the working directory) */
#define LOG_DIR               "logs"
#define REPORT_DIR            "reports"
#define EXEC_LOG_PATH         "logs/execution.log"
#define ERROR_LOG_PATH        "logs/error.log"
#define JOURNAL_PATH          "logs/job_history.journal"
#define REPORT_PATH           "reports/performance_report.txt"
#define METRICS_SNAPSHOT_PATH "reports/metrics_snapshot.bin"
#define CONFIG_PATH           "scheduler.conf"
#define TEST_DATA_DIR         "test_data"

typedef struct SchedulerConfig {
    int    worker_count;   /* MIN_WORKER_COUNT..MAX_WORKER_COUNT */
    int    max_retries;    /* default retry limit for new jobs   */
    long   ageing_ms;      /* 0 disables priority ageing         */
    size_t queue_capacity; /* per-priority queue capacity        */
} SchedulerConfig;

void config_set_defaults(SchedulerConfig *cfg);

/* Returns 0 on success, 1 if the file does not exist (defaults kept),
 * -1 on I/O error. Invalid lines are ignored and counted in *warnings. */
int config_load(SchedulerConfig *cfg, const char *path, int *warnings);

/* Atomically rewrites the config file (temp file + rename). 0 on success. */
int config_save(const SchedulerConfig *cfg, const char *path);

/* Returns 1 if every field is inside its documented range. */
int config_is_valid(const SchedulerConfig *cfg);

#endif /* CONFIG_H */
