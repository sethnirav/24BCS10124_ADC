/*
 * benchmark.c - Compares three ways of running an IDENTICAL workload:
 *   1. sequential   (one thread, one job after another)
 *   2. thread-per-job (pthread_create for every job)
 *   3. fixed thread pool (this project's scheduler + worker pool)
 *
 * Usage: ./bin/benchmark [workers] [heavy_jobs] [heavy_n] [light_jobs] [light_n]
 * Results are printed and written to reports/benchmark_results.txt.
 * Every number comes from the run itself; nothing is hard-coded.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "logger.h"
#include "scheduler.h"
#include "task_handlers.h"
#include "worker_pool.h"

typedef struct {
    const char *name;
    int         jobs_ok;
    int         jobs_total;
    double      wall_ms;
    double      sum_exec_ms;
    double      cpu_ms;      /* user + system CPU time used by the process */
    long        ctx_switches;
    int         thread_create_failures;
} Result;

typedef struct { Job *job; double exec_ms; int rc; } ThreadArg;

static double cpu_ms_now(long *ctx)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (ctx) *ctx = ru.ru_nvcsw + ru.ru_nivcsw;
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static Job **make_jobs(int count, uint64_t n)
{
    Job **jobs = calloc((size_t)count, sizeof *jobs);
    if (!jobs) return NULL;
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.calc.op = CALC_PRIME_COUNT;
    in.calc.n = n;
    for (int i = 0; i < count; i++) {
        jobs[i] = job_create((unsigned)i + 1, "bench", TASK_CALCULATION, PRIORITY_NORMAL, &in, 0);
        if (!jobs[i]) { for (int k = 0; k < i; k++) job_destroy(jobs[k]); free(jobs); return NULL; }
    }
    return jobs;
}

static void free_jobs(Job **jobs, int count)
{
    for (int i = 0; i < count; i++) job_destroy(jobs[i]);
    free(jobs);
}

static void run_one(ThreadArg *a)
{
    char res[JOB_MESSAGE_LEN], err[JOB_MESSAGE_LEN];
    struct timespec t0, t1;
    timespec_now(&t0);
    a->rc = task_execute(a->job, res, sizeof res, err, sizeof err);
    timespec_now(&t1);
    a->exec_ms = timespec_diff_ms(&t0, &t1);
}

static void *thread_entry(void *p) { run_one(p); return NULL; }

static Result bench_sequential(int count, uint64_t n)
{
    Result r = { "Sequential", 0, count, 0, 0, 0, 0, 0 };
    Job **jobs = make_jobs(count, n);
    if (!jobs) return r;
    long c0, c1;
    double cpu0 = cpu_ms_now(&c0);
    struct timespec t0, t1;
    timespec_now(&t0);
    for (int i = 0; i < count; i++) {
        ThreadArg a = { jobs[i], 0, -1 };
        run_one(&a);
        r.sum_exec_ms += a.exec_ms;
        if (a.rc == 0) r.jobs_ok++;
    }
    timespec_now(&t1);
    r.cpu_ms = cpu_ms_now(&c1) - cpu0;
    r.ctx_switches = c1 - c0;
    r.wall_ms = timespec_diff_ms(&t0, &t1);
    free_jobs(jobs, count);
    return r;
}

static Result bench_thread_per_job(int count, uint64_t n)
{
    Result r = { "Thread-per-job", 0, count, 0, 0, 0, 0, 0 };
    Job **jobs = make_jobs(count, n);
    ThreadArg *args = calloc((size_t)count, sizeof *args);
    pthread_t *tids = calloc((size_t)count, sizeof *tids);
    char *started = calloc((size_t)count, 1);
    if (!jobs || !args || !tids || !started) goto out;

    long c0, c1;
    double cpu0 = cpu_ms_now(&c0);
    struct timespec t0, t1;
    timespec_now(&t0);
    for (int i = 0; i < count; i++) {
        args[i] = (ThreadArg){ jobs[i], 0, -1 };
        if (pthread_create(&tids[i], NULL, thread_entry, &args[i]) == 0) started[i] = 1;
        else r.thread_create_failures++; /* e.g. thread limit reached */
    }
    for (int i = 0; i < count; i++) {
        if (!started[i]) continue;
        pthread_join(tids[i], NULL);
        r.sum_exec_ms += args[i].exec_ms;
        if (args[i].rc == 0) r.jobs_ok++;
    }
    timespec_now(&t1);
    r.cpu_ms = cpu_ms_now(&c1) - cpu0;
    r.ctx_switches = c1 - c0;
    r.wall_ms = timespec_diff_ms(&t0, &t1);
out:
    if (jobs) free_jobs(jobs, count);
    free(args); free(tids); free(started);
    return r;
}

static Result bench_pool(int count, uint64_t n, int workers)
{
    Result r = { "Thread pool", 0, count, 0, 0, 0, 0, 0 };
    SchedulerConfig cfg;
    config_set_defaults(&cfg);
    cfg.queue_capacity = (size_t)count;
    cfg.max_retries = 0;
    Scheduler *s = scheduler_create(&cfg, NULL); /* no logging: fair comparison */
    if (!s) return r;
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.calc.op = CALC_PRIME_COUNT;
    in.calc.n = n;

    long c0, c1;
    double cpu0 = cpu_ms_now(&c0);
    struct timespec t0, t1;
    timespec_now(&t0);
    WorkerPool *pool = worker_pool_create(s, workers); /* creation cost included */
    if (!pool) { scheduler_destroy(s); return r; }
    for (int i = 0; i < count; i++)
        scheduler_submit(s, "bench", TASK_CALCULATION, PRIORITY_NORMAL, &in, 0, NULL, NULL, 0);
    scheduler_wait_idle(s);
    scheduler_shutdown(s);
    worker_pool_destroy(pool);
    timespec_now(&t1);
    r.cpu_ms = cpu_ms_now(&c1) - cpu0;
    r.ctx_switches = c1 - c0;
    r.wall_ms = timespec_diff_ms(&t0, &t1);

    MetricsSnapshot m;
    metrics_snapshot(scheduler_metrics(s), &m);
    r.jobs_ok = (int)m.completed;
    r.sum_exec_ms = m.total_exec_ms;
    scheduler_destroy(s);
    return r;
}

static void print_results(FILE *out, const char *label, int jobs, uint64_t n, int workers,
                          const Result *res, int count)
{
    fprintf(out, "\nWorkload: %s - %d x prime_count(%llu), pool workers = %d\n",
            label, jobs, (unsigned long long)n, workers);
    fprintf(out, "%-16s %8s %12s %14s %14s %12s %10s\n",
            "Approach", "Done", "Wall(ms)", "AvgExec(ms)", "Jobs/s", "CPU(ms)", "CtxSw");
    fprintf(out, "------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < count; i++) {
        const Result *r = &res[i];
        double avg = r->jobs_ok ? r->sum_exec_ms / r->jobs_ok : 0.0;
        double tput = r->wall_ms > 0 ? r->jobs_ok / (r->wall_ms / 1000.0) : 0.0;
        fprintf(out, "%-16s %4d/%-4d %12.2f %14.4f %14.1f %12.1f %10ld",
                r->name, r->jobs_ok, r->jobs_total, r->wall_ms, avg, tput, r->cpu_ms, r->ctx_switches);
        if (r->thread_create_failures) fprintf(out, "  (%d pthread_create failures)", r->thread_create_failures);
        fprintf(out, "\n");
    }
}

static long arg_or(int argc, char **argv, int idx, long def)
{
    if (argc <= idx) return def;
    long v = strtol(argv[idx], NULL, 10);
    return v > 0 ? v : def;
}

int main(int argc, char **argv)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = (int)arg_or(argc, argv, 1, cpus > 0 && cpus <= MAX_WORKER_COUNT ? cpus : 4);
    if (workers > MAX_WORKER_COUNT) workers = MAX_WORKER_COUNT;
    int heavy_jobs = (int)arg_or(argc, argv, 2, 64);
    uint64_t heavy_n = (uint64_t)arg_or(argc, argv, 3, 2000000);
    int light_jobs = (int)arg_or(argc, argv, 4, 2000);
    uint64_t light_n = (uint64_t)arg_or(argc, argv, 5, 2000);
    if (heavy_n > CALC_PRIME_MAX) heavy_n = CALC_PRIME_MAX;
    if (light_n > CALC_PRIME_MAX) light_n = CALC_PRIME_MAX;

    ensure_directory(REPORT_DIR);
    FILE *file = fopen(REPORT_DIR "/benchmark_results.txt", "w");

    Result heavy[3] = { bench_sequential(heavy_jobs, heavy_n),
                        bench_thread_per_job(heavy_jobs, heavy_n),
                        bench_pool(heavy_jobs, heavy_n, workers) };
    Result light[3] = { bench_sequential(light_jobs, light_n),
                        bench_thread_per_job(light_jobs, light_n),
                        bench_pool(light_jobs, light_n, workers) };

    FILE *outs[2] = { stdout, file };
    for (int k = 0; k < 2; k++) {
        FILE *o = outs[k];
        if (!o) continue;
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char when[32];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(o, "Thread Pool Job Scheduler - Benchmark (%s)\n", when);
        fprintf(o, "Online CPUs: %ld   (all values measured in this run)\n", cpus);
        print_results(o, "CPU-heavy", heavy_jobs, heavy_n, workers, heavy, 3);
        print_results(o, "many tiny jobs", light_jobs, light_n, workers, light, 3);
        fprintf(o, "\nNotes: Wall = elapsed time incl. thread creation/teardown. CPU = user+sys time of\n"
                   "the whole process during the run. AvgExec = mean time inside the task handler.\n"
                   "With N cores, parallel approaches can be up to ~N times faster than sequential for\n"
                   "CPU-bound jobs; for tiny jobs, per-job thread creation overhead dominates.\n");
    }
    if (file) { fclose(file); printf("\nSaved to %s/benchmark_results.txt\n", REPORT_DIR); }
    return 0;
}
