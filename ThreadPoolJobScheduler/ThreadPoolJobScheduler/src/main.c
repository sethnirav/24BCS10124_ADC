/*
 * main.c - Menu-driven command-line interface.
 *
 * Usage:  ./bin/scheduler [--workers N] [--no-color] [--demo]
 *   --demo  submits a demonstration workload, waits for it, prints the
 *           dashboard / job table / metrics and shuts down (non-interactive).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "job.h"
#include "logger.h"
#include "metrics.h"
#include "scheduler.h"
#include "task_handlers.h"
#include "worker_pool.h"

/* ------------------------------------------------------------ app state */

typedef struct App {
    SchedulerConfig cfg;
    Scheduler      *sched;
    WorkerPool     *pool;
    Logger         *logger;
    bool            color;
    JournalSummary  history;     /* from previous runs (persistent) */
    MetricsSnapshot prev;        /* last session's saved metrics    */
    bool            has_prev;
    time_t          prev_saved_at;
} App;

/* ----------------------------------------------------- terminal colours */

typedef enum { C_RESET, C_BOLD, C_DIM, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_CYAN, C_MAGENTA } Color;

static const char *const ANSI[] = {
    "\033[0m", "\033[1m", "\033[2m", "\033[31m", "\033[32m", "\033[33m", "\033[34m", "\033[36m", "\033[35m"
};

static const App *g_color_app; /* read-only pointer used by col() */

static const char *col(Color c)
{
    return (g_color_app && g_color_app->color) ? ANSI[c] : "";
}

static Color status_color(JobStatus s)
{
    switch (s) {
    case JOB_COMPLETED: return C_GREEN;
    case JOB_FAILED:    return C_RED;
    case JOB_RUNNING:   return C_CYAN;
    case JOB_RETRYING:  return C_MAGENTA;
    case JOB_QUEUED:    return C_YELLOW;
    case JOB_PENDING:
    default:            return C_DIM;
    }
}

static void rule(void)
{
    printf("%s%s%s\n", col(C_DIM),
           "------------------------------------------------------------------------------------------",
           col(C_RESET));
}

static void title(const char *t)
{
    printf("\n%s%s==[ %s ]==%s\n", col(C_BOLD), col(C_BLUE), t, col(C_RESET));
}

/* ------------------------------------------------------------ input I/O */

/* Reads one line; returns false on EOF. Over-long input is discarded. */
static bool read_line(const char *prompt, char *buf, size_t n)
{
    if (prompt) { printf("%s", prompt); fflush(stdout); }
    if (!fgets(buf, (int)n, stdin)) return false;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    else { int c; while ((c = getchar()) != EOF && c != '\n') {} }
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return true;
}

typedef enum { INPUT_OK, INPUT_DEFAULT, INPUT_EOF } InputResult;

/* Integer in [lo, hi]. Empty input returns INPUT_DEFAULT if allowed. */
static InputResult read_long(const char *prompt, long lo, long hi, bool allow_empty, long *out)
{
    char buf[64];
    for (;;) {
        if (!read_line(prompt, buf, sizeof buf)) return INPUT_EOF;
        if (buf[0] == '\0' && allow_empty) return INPUT_DEFAULT;
        char *end = NULL;
        errno = 0;
        long v = strtol(buf, &end, 10);
        if (errno == 0 && end != buf && *end == '\0' && v >= lo && v <= hi) { *out = v; return INPUT_OK; }
        printf("  %sPlease enter a whole number between %ld and %ld.%s\n", col(C_RED), lo, hi, col(C_RESET));
    }
}

static void wait_enter(void)
{
    char buf[8];
    read_line("\nPress Enter to continue...", buf, sizeof buf);
}

/* ------------------------------------------------------------- display */

static void fmt_time(time_t t, char *buf, size_t n)
{
    if (t == 0) { snprintf(buf, n, "-"); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%H:%M:%S", &tm);
}

static void print_job_row(const Job *j)
{
    const char *msg = j->status == JOB_COMPLETED ? j->result : j->error;
    printf("%5u  %-18.18s %-11s %-7s %s%-10s%s %3d/%-3d %10.3f  %.38s\n",
           j->id, j->name, task_type_str(j->type), job_priority_str(j->priority),
           col(status_color(j->status)), job_status_str(j->status), col(C_RESET),
           j->retry_count, j->max_retries, j->total_exec_ms, msg);
}

static void print_jobs(App *a, int filter, const char *heading)
{
    title(heading);
    Job *jobs = NULL;
    size_t n = scheduler_snapshot_jobs(a->sched, filter, &jobs);
    if (n == 0) { printf("  (no jobs)\n"); return; }
    printf("%s%5s  %-18s %-11s %-7s %-10s %-7s %10s  %s%s\n", col(C_BOLD),
           "ID", "Name", "Type", "Prio", "Status", "Retry", "Exec(ms)", "Result / Error", col(C_RESET));
    rule();
    for (size_t i = 0; i < n; i++) print_job_row(&jobs[i]);
    rule();
    printf("  %zu job(s)\n", n);
    free(jobs);
}

static void print_job_detail(const Job *j)
{
    char sub[16], st[16], done[16];
    fmt_time(j->submitted_wall, sub, sizeof sub);
    fmt_time(j->started_wall, st, sizeof st);
    fmt_time(j->completed_wall, done, sizeof done);
    printf("  Job ID          : %u\n", j->id);
    printf("  Name            : %s\n", j->name);
    printf("  Task type       : %s (%s)\n", task_type_str(j->type), task_registry_get(j->type)->name);
    printf("  Priority        : %s\n", job_priority_str(j->priority));
    printf("  Status          : %s%s%s\n", col(status_color(j->status)), job_status_str(j->status), col(C_RESET));
    printf("  Submitted       : %s\n", sub);
    printf("  Started (last)  : %s\n", st);
    printf("  Finished        : %s\n", done);
    printf("  Last attempt    : %.3f ms\n", j->last_exec_ms);
    printf("  Total exec time : %.3f ms\n", j->total_exec_ms);
    printf("  Retries         : %d of max %d\n", j->retry_count, j->max_retries);
    if (j->result[0]) printf("  Result          : %s\n", j->result);
    if (j->error[0])  printf("  %sLast error      : %s%s\n", col(C_RED), j->error, col(C_RESET));
}

static void runtime_stats(App *a, RuntimeStats *rt)
{
    size_t c[JOB_STATUS_COUNT];
    scheduler_status_counts(a->sched, c);
    rt->pending     = c[JOB_PENDING] + c[JOB_QUEUED] + c[JOB_RETRYING];
    rt->running     = c[JOB_RUNNING];
    rt->workers     = (size_t)worker_pool_size(a->pool);
    rt->utilization = worker_pool_utilization(a->pool);
}

static void show_dashboard(App *a)
{
    size_t c[JOB_STATUS_COUNT];
    scheduler_status_counts(a->sched, c);
    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(a->sched), &m);

    title("DASHBOARD");
    printf("  Uptime %.1f s | Workers %d | Utilization %.1f %% | Ageing %ld ms | Default retries %d\n",
           m.uptime_s, worker_pool_size(a->pool), worker_pool_utilization(a->pool) * 100.0,
           scheduler_get_ageing_ms(a->sched), scheduler_get_default_retries(a->sched));
    rule();
    printf("  %sQUEUED%s %-4zu %sRUNNING%s %-4zu %sRETRYING%s %-4zu %sCOMPLETED%s %-4zu %sFAILED%s %-4zu\n",
           col(C_YELLOW), col(C_RESET), c[JOB_QUEUED] + c[JOB_PENDING],
           col(C_CYAN), col(C_RESET), c[JOB_RUNNING],
           col(C_MAGENTA), col(C_RESET), c[JOB_RETRYING],
           col(C_GREEN), col(C_RESET), c[JOB_COMPLETED],
           col(C_RED), col(C_RESET), c[JOB_FAILED]);
    size_t cap = scheduler_queue_capacity(a->sched);
    printf("  Queue depth  HIGH %zu/%zu   NORMAL %zu/%zu   LOW %zu/%zu\n",
           scheduler_queue_depth(a->sched, PRIORITY_HIGH), cap,
           scheduler_queue_depth(a->sched, PRIORITY_NORMAL), cap,
           scheduler_queue_depth(a->sched, PRIORITY_LOW), cap);
    printf("  Submitted %llu | Rejected %llu | Retries %llu | Avg exec %.3f ms | Avg wait %.3f ms | %.2f jobs/s\n",
           (unsigned long long)m.submitted, (unsigned long long)m.rejected, (unsigned long long)m.retries,
           m.avg_exec_ms, m.avg_wait_ms, m.throughput);
    rule();
    printf("  %sHistory (persistent journal): %zu records from earlier sessions (%zu completed, %zu failed",
           col(C_DIM), a->history.valid_records, a->history.completed, a->history.failed);
    if (a->history.malformed_records) printf(", %zu malformed lines skipped", a->history.malformed_records);
    printf(")%s\n", col(C_RESET));
    if (a->has_prev) {
        char t[16];
        fmt_time(a->prev_saved_at, t, sizeof t);
        printf("  %sPrevious session (saved %s): %llu completed, %llu failed, avg exec %.3f ms%s\n",
               col(C_DIM), t, (unsigned long long)a->prev.completed, (unsigned long long)a->prev.failed,
               a->prev.avg_exec_ms, col(C_RESET));
    }
}

static void show_metrics(App *a)
{
    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(a->sched), &m);
    RuntimeStats rt;
    runtime_stats(a, &rt);

    title("PERFORMANCE METRICS");
    printf("  Total jobs submitted     : %llu\n", (unsigned long long)m.submitted);
    printf("  Submissions rejected     : %llu\n", (unsigned long long)m.rejected);
    printf("  Total jobs completed     : %s%llu%s\n", col(C_GREEN), (unsigned long long)m.completed, col(C_RESET));
    printf("  Total jobs failed        : %s%llu%s\n", col(C_RED), (unsigned long long)m.failed, col(C_RESET));
    printf("  Jobs pending (in queues) : %zu\n", rt.pending);
    printf("  Jobs running now         : %zu\n", rt.running);
    printf("  Total retry attempts     : %llu\n", (unsigned long long)m.retries);
    printf("  Attempts executed        : %llu\n", (unsigned long long)m.exec_samples);
    printf("  Average execution time   : %.3f ms\n", m.avg_exec_ms);
    printf("  Minimum execution time   : %.3f ms\n", m.min_exec_ms);
    printf("  Maximum execution time   : %.3f ms\n", m.max_exec_ms);
    printf("  Average queue wait time  : %.3f ms\n", m.avg_wait_ms);
    printf("  Throughput               : %.3f jobs/s over %.1f s uptime\n", m.throughput, m.uptime_s);
    printf("  Worker utilization       : %.2f %% (busy time / alive time, %zu workers)\n",
           rt.utilization * 100.0, rt.workers);
}

static void show_workers(App *a)
{
    WorkerInfo info[MAX_WORKER_COUNT];
    size_t n = worker_pool_info(a->pool, info, MAX_WORKER_COUNT);
    title("WORKER POOL");
    printf("%s%6s  %-8s %-8s %10s %12s %8s%s\n", col(C_BOLD), "Worker", "State", "Job", "Processed",
           "Busy(ms)", "Util", col(C_RESET));
    rule();
    for (size_t i = 0; i < n; i++) {
        char job[16];
        if (info[i].current_job) snprintf(job, sizeof job, "#%u", info[i].current_job);
        else snprintf(job, sizeof job, "-");
        double u = info[i].alive_ms > 0 ? info[i].busy_ms / info[i].alive_ms * 100.0 : 0.0;
        printf("%6d  %s%-8s%s %-8s %10llu %12.3f %7.2f%%\n", info[i].id,
               col(info[i].state == WORKER_BUSY ? C_CYAN : C_DIM), worker_state_str(info[i].state),
               col(C_RESET), job, (unsigned long long)info[i].jobs_processed, info[i].busy_ms, u);
    }
    rule();
    printf("  %zu worker thread(s), created once and reused for every job.\n", n);
}

/* ------------------------------------------------------------- actions */

static void report_submit(SubmitResult r, unsigned id, const char *err)
{
    if (r == SUBMIT_OK)
        printf("  %sAccepted: job #%u queued.%s\n", col(C_GREEN), id, col(C_RESET));
    else
        printf("  %sRejected (%s): %s%s\n", col(C_RED), submit_result_str(r), err, col(C_RESET));
}

/* Returns false on EOF. */
static bool submit_interactive(App *a)
{
    char name[JOB_NAME_LEN + 8];
    long v;
    title("SUBMIT NEW JOB");
    if (!read_line("  Job name: ", name, sizeof name)) return false;

    printf("  Task types:\n");
    for (size_t i = 0; i < task_registry_count(); i++) {
        const TaskDescriptor *d = task_registry_get((TaskType)i);
        printf("    %zu) %-19s %s%s%s\n", i + 1, d->name, col(C_DIM), d->description, col(C_RESET));
    }
    if (read_long("  Choose task type [1-5]: ", 1, 5, false, &v) == INPUT_EOF) return false;
    TaskType type = (TaskType)(v - 1);

    if (read_long("  Priority 1=HIGH 2=NORMAL 3=LOW [2]: ", 1, 3, true, &v) == INPUT_EOF) return false;
    JobPriority prio = PRIORITY_NORMAL;
    if (v == 1) prio = PRIORITY_HIGH; else if (v == 3) prio = PRIORITY_LOW;

    TaskInput in;
    memset(&in, 0, sizeof in);
    switch (type) {
    case TASK_CHECKSUM:
        if (!read_line("  Text to checksum: ", in.text, sizeof in.text)) return false;
        break;
    case TASK_VALIDATION:
        if (!read_line("  Records (name:age;name:age...): ", in.text, sizeof in.text)) return false;
        break;
    case TASK_FILE_PROCESSING:
        printf("  Directory inside %s/ (empty = %s itself)\n", TEST_DATA_DIR, TEST_DATA_DIR);
        if (!read_line("  Directory: ", in.text, sizeof in.text)) return false;
        break;
    case TASK_CALCULATION: {
        printf("  Operations: 1) sum 1..n  2) factorial n  3) fibonacci n  4) prime count <= n\n");
        if (read_long("  Operation [1-4]: ", 1, 4, false, &v) == INPUT_EOF) return false;
        in.calc.op = (CalcOp)(v - 1);
        if (read_long("  n: ", 0, (long)CALC_SUM_MAX, false, &v) == INPUT_EOF) return false;
        in.calc.n = (uint64_t)v;
        break;
    }
    case TASK_FAILURE_SIM:
        if (read_long("  Number of attempts that should fail (-1 = always): ", -1,
                      MAX_RETRY_LIMIT + 1, false, &v) == INPUT_EOF) return false;
        in.failure.fail_attempts = (int)v;
        break;
    case TASK_TYPE_COUNT:
    default:
        return true;
    }

    char prompt[96];
    snprintf(prompt, sizeof prompt, "  Max retries 0-%d [default %d]: ", MAX_RETRY_LIMIT,
             scheduler_get_default_retries(a->sched));
    InputResult ir = read_long(prompt, 0, MAX_RETRY_LIMIT, true, &v);
    if (ir == INPUT_EOF) return false;
    int retries = ir == INPUT_DEFAULT ? -1 : (int)v;

    unsigned id = 0;
    char err[JOB_MESSAGE_LEN] = "";
    SubmitResult r = scheduler_submit(a->sched, name, type, prio, &in, retries, &id, err, sizeof err);
    report_submit(r, id, err);
    return true;
}

static void submit_demo(App *a)
{
    typedef struct { const char *name; TaskType type; JobPriority prio; int retries; TaskInput in; } Spec;
    Spec specs[] = {
        { "crc-hello",       TASK_CHECKSUM,        PRIORITY_NORMAL, -1, { .text = "Hello, thread pool!" } },
        { "crc-config",      TASK_CHECKSUM,        PRIORITY_LOW,    -1, { .text = "worker_count=4;max_retries=3" } },
        { "validate-staff",  TASK_VALIDATION,      PRIORITY_HIGH,   -1, { .text = "alice:30;bob:41;carol:29" } },
        { "validate-dirty",  TASK_VALIDATION,      PRIORITY_NORMAL, -1, { .text = "dave:45;:20;eve:abc;frank:200" } },
        { "scan-test-data",  TASK_FILE_PROCESSING, PRIORITY_NORMAL, -1, { .text = "" } },
        { "scan-docs",       TASK_FILE_PROCESSING, PRIORITY_LOW,    -1, { .text = "docs" } },
        { "scan-missing",    TASK_FILE_PROCESSING, PRIORITY_LOW,     1, { .text = "no_such_dir" } },
        { "primes-2M",       TASK_CALCULATION,     PRIORITY_LOW,    -1, { .calc = { CALC_PRIME_COUNT, 2000000 } } },
        { "primes-500k",     TASK_CALCULATION,     PRIORITY_HIGH,   -1, { .calc = { CALC_PRIME_COUNT, 500000 } } },
        { "sum-50M",         TASK_CALCULATION,     PRIORITY_NORMAL, -1, { .calc = { CALC_SUM, 50000000 } } },
        { "factorial-20",    TASK_CALCULATION,     PRIORITY_HIGH,   -1, { .calc = { CALC_FACTORIAL, 20 } } },
        { "fibonacci-90",    TASK_CALCULATION,     PRIORITY_NORMAL, -1, { .calc = { CALC_FIBONACCI, 90 } } },
        { "flaky-2-fails",   TASK_FAILURE_SIM,     PRIORITY_HIGH,    3, { .failure = { 2 } } },
        { "always-fails",    TASK_FAILURE_SIM,     PRIORITY_NORMAL,  2, { .failure = { -1 } } },
        { "no-retry-fail",   TASK_FAILURE_SIM,     PRIORITY_LOW,     0, { .failure = { 1 } } },
    };
    title("DEMO WORKLOAD");
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        unsigned id = 0;
        char err[JOB_MESSAGE_LEN] = "";
        SubmitResult r = scheduler_submit(a->sched, specs[i].name, specs[i].type, specs[i].prio,
                                          &specs[i].in, specs[i].retries, &id, err, sizeof err);
        printf("  %-16s ", specs[i].name);
        report_submit(r, id, err);
    }
    /* One deliberately invalid submission to show input validation. */
    TaskInput bad = { .calc = { CALC_FACTORIAL, 99 } };
    char err[JOB_MESSAGE_LEN] = "";
    printf("  %-16s ", "factorial-99");
    report_submit(scheduler_submit(a->sched, "factorial-99", TASK_CALCULATION, PRIORITY_NORMAL,
                                   &bad, -1, NULL, err, sizeof err), 0, err);
}

static bool search_job(App *a)
{
    long v;
    title("SEARCH JOB BY ID");
    if (read_long("  Job ID: ", 1, 100000000L, false, &v) == INPUT_EOF) return false;
    Job j;
    if (scheduler_get_job(a->sched, (unsigned)v, &j) != 0) printf("  %sNo job with ID %ld.%s\n", col(C_RED), v, col(C_RESET));
    else print_job_detail(&j);
    return true;
}

static void save_config(App *a)
{
    a->cfg.worker_count = worker_pool_size(a->pool);
    a->cfg.max_retries  = scheduler_get_default_retries(a->sched);
    a->cfg.ageing_ms    = scheduler_get_ageing_ms(a->sched);
    if (config_save(&a->cfg, CONFIG_PATH) == 0)
        printf("  %sSaved to %s%s\n", col(C_DIM), CONFIG_PATH, col(C_RESET));
    else
        printf("  %sCould not save %s%s\n", col(C_RED), CONFIG_PATH, col(C_RESET));
}

static bool configure(App *a)
{
    long v;
    title("SCHEDULER CONFIGURATION");
    printf("  1) Worker thread count   (now %d)\n", worker_pool_size(a->pool));
    printf("  2) Default retry limit   (now %d)\n", scheduler_get_default_retries(a->sched));
    printf("  3) Priority ageing (ms)  (now %ld, 0 = off)\n", scheduler_get_ageing_ms(a->sched));
    printf("  4) Back\n");
    if (read_long("  Choice: ", 1, 4, false, &v) == INPUT_EOF) return false;
    switch (v) {
    case 1:
        if (read_long("  New worker count: ", MIN_WORKER_COUNT, MAX_WORKER_COUNT, false, &v) == INPUT_EOF) return false;
        printf("  Resizing pool (busy workers finish their current job first)...\n");
        if (worker_pool_resize(a->pool, (int)v) == 0) {
            printf("  %sPool now has %d workers.%s\n", col(C_GREEN), worker_pool_size(a->pool), col(C_RESET));
            logger_log(a->logger, LOG_INFO, "Worker pool resized to %d", worker_pool_size(a->pool));
        } else {
            printf("  %sResize incomplete: pool has %d workers.%s\n", col(C_RED), worker_pool_size(a->pool), col(C_RESET));
        }
        save_config(a);
        break;
    case 2:
        if (read_long("  New default retry limit: ", 0, MAX_RETRY_LIMIT, false, &v) == INPUT_EOF) return false;
        scheduler_set_default_retries(a->sched, (int)v);
        save_config(a);
        break;
    case 3:
        if (read_long("  New ageing threshold (ms): ", 0, MAX_AGEING_MS, false, &v) == INPUT_EOF) return false;
        scheduler_set_ageing_ms(a->sched, v);
        save_config(a);
        break;
    default:
        break;
    }
    return true;
}

static void write_report(App *a, const char *path)
{
    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(a->sched), &m);
    RuntimeStats rt;
    runtime_stats(a, &rt);
    if (metrics_write_report(&m, &rt, path) == 0)
        printf("  Performance report written to %s\n", path);
    else
        printf("  %sCould not write %s%s\n", col(C_RED), path, col(C_RESET));
}

static void app_shutdown(App *a, bool graceful)
{
    title("SHUTDOWN");
    if (graceful) {
        printf("  Waiting for queued and running jobs to finish...\n");
        scheduler_wait_idle(a->sched);
    }
    write_report(a, REPORT_PATH); /* while the pool still exists */
    scheduler_shutdown(a->sched);
    worker_pool_destroy(a->pool); /* joins every worker thread */
    a->pool = NULL;
    size_t cancelled = scheduler_cancel_pending(a->sched);
    if (cancelled) printf("  %zu queued job(s) cancelled and recorded as FAILED.\n", cancelled);

    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(a->sched), &m);
    if (metrics_save_binary(&m, METRICS_SNAPSHOT_PATH) != 0)
        printf("  %sCould not save %s%s\n", col(C_RED), METRICS_SNAPSHOT_PATH, col(C_RESET));
    printf("  Final: %llu completed, %llu failed, %llu retries.\n",
           (unsigned long long)m.completed, (unsigned long long)m.failed, (unsigned long long)m.retries);
    logger_log(a->logger, LOG_INFO, "Scheduler stopped: %llu completed, %llu failed",
               (unsigned long long)m.completed, (unsigned long long)m.failed);

    scheduler_destroy(a->sched);
    logger_close(a->logger);
    a->sched = NULL;
    a->logger = NULL;
    printf("  All worker threads joined and memory released. Goodbye.\n");
}

/* --------------------------------------------------------------- main */

static void print_menu(void)
{
    title(APP_NAME " v" APP_VERSION);
    printf("   1) Dashboard               8) View Failed Jobs\n");
    printf("   2) Submit New Job          9) Worker Pool Information\n");
    printf("   3) View All Jobs          10) Performance Metrics\n");
    printf("   4) Search Job by ID       11) Scheduler Configuration\n");
    printf("   5) View Pending Jobs      12) Shutdown System\n");
    printf("   6) View Running Jobs      13) Load Demo Workload\n");
    printf("   7) View Completed Jobs    14) Export Performance Report\n");
}

static void usage(const char *prog)
{
    printf("Usage: %s [--workers N] [--no-color] [--demo]\n", prog);
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    g_color_app = &app;
    bool demo = false;
    int  workers_override = 0;

    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    app.color = isatty(STDOUT_FILENO) && !no_color && !(term && strcmp(term, "dumb") == 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) demo = true;
        else if (strcmp(argv[i], "--no-color") == 0) app.color = false;
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v < MIN_WORKER_COUNT || v > MAX_WORKER_COUNT) {
                fprintf(stderr, "--workers must be %d..%d\n", MIN_WORKER_COUNT, MAX_WORKER_COUNT);
                return EXIT_FAILURE;
            }
            workers_override = (int)v;
        } else { usage(argv[0]); return EXIT_FAILURE; }
    }

    if (ensure_directory(LOG_DIR) != 0 || ensure_directory(REPORT_DIR) != 0) {
        perror("cannot create logs/ or reports/");
        return EXIT_FAILURE;
    }

    config_set_defaults(&app.cfg);
    int warnings = 0;
    if (config_load(&app.cfg, CONFIG_PATH, &warnings) < 0)
        fprintf(stderr, "warning: could not read %s, using defaults\n", CONFIG_PATH);
    if (warnings) fprintf(stderr, "warning: %d invalid line(s) in %s ignored\n", warnings, CONFIG_PATH);
    if (workers_override) app.cfg.worker_count = workers_override;

    if (logger_read_journal(JOURNAL_PATH, &app.history) != 0)
        fprintf(stderr, "warning: could not read %s\n", JOURNAL_PATH);
    app.has_prev = metrics_load_binary(METRICS_SNAPSHOT_PATH, &app.prev, &app.prev_saved_at) == 0;

    app.logger = logger_open(EXEC_LOG_PATH, ERROR_LOG_PATH, JOURNAL_PATH);
    if (!app.logger) { perror("cannot open log files"); return EXIT_FAILURE; }
    app.sched = scheduler_create(&app.cfg, app.logger);
    if (!app.sched) { fprintf(stderr, "scheduler creation failed\n"); logger_close(app.logger); return EXIT_FAILURE; }
    app.pool = worker_pool_create(app.sched, app.cfg.worker_count);
    if (!app.pool) {
        fprintf(stderr, "worker pool creation failed\n");
        scheduler_destroy(app.sched);
        logger_close(app.logger);
        return EXIT_FAILURE;
    }
    logger_log(app.logger, LOG_INFO, "Scheduler started with %d workers (ageing %ld ms, retries %d)",
               app.cfg.worker_count, app.cfg.ageing_ms, app.cfg.max_retries);

    if (demo) {
        submit_demo(&app);
        printf("\n  Waiting for the workload to finish...\n");
        scheduler_wait_idle(app.sched);
        show_dashboard(&app);
        print_jobs(&app, -1, "ALL JOBS");
        show_workers(&app);
        show_metrics(&app);
        app_shutdown(&app, true);
        return EXIT_SUCCESS;
    }

    bool running = true;
    while (running) {
        print_menu();
        long choice;
        if (read_long("\n  Select option: ", 1, 14, false, &choice) == INPUT_EOF) break;
        bool ok = true;
        switch (choice) {
        case 1:  show_dashboard(&app); break;
        case 2:  ok = submit_interactive(&app); break;
        case 3:  print_jobs(&app, -1, "ALL JOBS"); break;
        case 4:  ok = search_job(&app); break;
        case 5:
            print_jobs(&app, JOB_QUEUED, "PENDING JOBS (QUEUED)");
            print_jobs(&app, JOB_RETRYING, "PENDING JOBS (WAITING FOR RETRY)");
            break;
        case 6:  print_jobs(&app, JOB_RUNNING, "RUNNING JOBS"); break;
        case 7:  print_jobs(&app, JOB_COMPLETED, "COMPLETED JOBS"); break;
        case 8:  print_jobs(&app, JOB_FAILED, "FAILED JOBS"); break;
        case 9:  show_workers(&app); break;
        case 10: show_metrics(&app); break;
        case 11: ok = configure(&app); break;
        case 12: {
            long g;
            InputResult r = read_long("  1) Graceful: finish queued jobs  2) Immediate: cancel queued jobs [1]: ",
                                      1, 2, true, &g);
            app_shutdown(&app, r != INPUT_OK || g == 1);
            return EXIT_SUCCESS;
        }
        case 13: submit_demo(&app); break;
        case 14: write_report(&app, REPORT_PATH); break;
        default: break;
        }
        if (!ok) break;
        if (choice != 2 && choice != 11 && choice != 13) wait_enter();
        if (feof(stdin)) running = false;
    }
    printf("\n  End of input - shutting down gracefully.\n");
    app_shutdown(&app, true);
    return EXIT_SUCCESS;
}
