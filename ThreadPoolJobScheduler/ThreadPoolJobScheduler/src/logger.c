/*
 * logger.c - Thread-safe text logs and append-only job journal.
 *
 * Journal record (one line, '|' separated, 10 fields):
 *   J1|epoch|id|name|type|priority|status|retries|total_exec_ms|message
 * Text fields are sanitised so they can never contain '|' or newlines.
 */
#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

struct Logger {
    FILE           *exec;
    FILE           *err;
    FILE           *journal;
    pthread_mutex_t lock;
};

int ensure_directory(const char *path)
{
    if (!path) return -1;
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

Logger *logger_open(const char *exec_path, const char *error_path, const char *journal_path)
{
    if (!exec_path || !error_path || !journal_path) return NULL;
    Logger *lg = calloc(1, sizeof *lg);
    if (!lg) return NULL;

    lg->exec    = fopen(exec_path, "a");
    lg->err     = fopen(error_path, "a");
    lg->journal = fopen(journal_path, "a"); /* append-only */
    if (!lg->exec || !lg->err || !lg->journal || pthread_mutex_init(&lg->lock, NULL) != 0) {
        if (lg->exec) fclose(lg->exec);
        if (lg->err) fclose(lg->err);
        if (lg->journal) fclose(lg->journal);
        free(lg);
        return NULL;
    }
    return lg;
}

void logger_close(Logger *lg)
{
    if (!lg) return;
    fclose(lg->exec);
    fclose(lg->err);
    fclose(lg->journal);
    pthread_mutex_destroy(&lg->lock);
    free(lg);
}

static void timestamp(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm); /* reentrant version: safe across threads */
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *level_str(LogLevel l)
{
    switch (l) {
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    case LOG_INFO:
    default:        return "INFO";
    }
}

void logger_log(Logger *lg, LogLevel level, const char *fmt, ...)
{
    if (!lg || !fmt) return;
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char ts[32];
    timestamp(ts, sizeof ts);

    pthread_mutex_lock(&lg->lock);
    fprintf(lg->exec, "[%s] [%-5s] %s\n", ts, level_str(level), msg);
    fflush(lg->exec);
    if (level == LOG_ERROR) {
        fprintf(lg->err, "[%s] %s\n", ts, msg);
        fflush(lg->err);
    }
    pthread_mutex_unlock(&lg->lock);
}

static void sanitize(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++)
        dst[i] = (src[i] == '|' || src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
    dst[i] = '\0';
}

void logger_journal(Logger *lg, const Job *job)
{
    if (!lg || !job) return;
    char name[JOB_NAME_LEN], msg[JOB_MESSAGE_LEN];
    sanitize(name, sizeof name, job->name);
    sanitize(msg, sizeof msg, job->status == JOB_COMPLETED ? job->result : job->error);

    pthread_mutex_lock(&lg->lock);
    fprintf(lg->journal, "%s|%lld|%u|%s|%s|%s|%s|%d|%.3f|%s\n",
            JOURNAL_FORMAT_TAG, (long long)time(NULL), job->id, name,
            task_type_str(job->type), job_priority_str(job->priority),
            job_status_str(job->status), job->retry_count, job->total_exec_ms, msg);
    fflush(lg->journal);
    pthread_mutex_unlock(&lg->lock);
}

/* Validates one complete line (without '\n'). Returns status or -1. */
static int parse_journal_line(char *line)
{
    /* Exactly JOURNAL_FIELDS fields means exactly JOURNAL_FIELDS-1 separators. */
    int bars = 0;
    for (const char *c = line; *c; c++) if (*c == '|') bars++;
    if (bars != JOURNAL_FIELDS - 1) return -1;

    char *fields[JOURNAL_FIELDS];
    char *p = line;
    for (int n = 0; n < JOURNAL_FIELDS; n++) {
        fields[n] = p;
        char *bar = strchr(p, '|');
        if (bar) { *bar = '\0'; p = bar + 1; }
    }
    if (strcmp(fields[0], JOURNAL_FORMAT_TAG) != 0) return -1;

    char *end;
    errno = 0;
    unsigned long id = strtoul(fields[2], &end, 10);
    if (errno || *end || id == 0) return -1;
    if (strcmp(fields[6], "COMPLETED") == 0) return JOB_COMPLETED;
    if (strcmp(fields[6], "FAILED") == 0) return JOB_FAILED;
    return -1;
}

int logger_read_journal(const char *path, JournalSummary *out)
{
    if (!path || !out) return -1;
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "r");
    if (!fp) return errno == ENOENT ? 0 : -1;

    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {
            /* Over-long line: discard the rest of it. Or a final line
             * without '\n' (e.g. interrupted write): treat as corrupt. */
            int c;
            while ((c = fgetc(fp)) != EOF && c != '\n') {}
            out->malformed_records++;
            continue;
        }
        line[len - 1] = '\0';
        if (len >= 2 && line[len - 2] == '\r') line[len - 2] = '\0';
        if (line[0] == '\0') continue;

        int status = parse_journal_line(line);
        if (status < 0) { out->malformed_records++; continue; }
        out->valid_records++;
        if (status == JOB_COMPLETED) out->completed++;
        else out->failed++;
    }
    int err = ferror(fp);
    fclose(fp);
    return err ? -1 : 0;
}
