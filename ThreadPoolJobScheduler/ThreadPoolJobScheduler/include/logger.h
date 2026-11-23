/*
 * logger.h - Thread-safe persistent logging:
 *   execution log  (all events, text)
 *   error log      (errors only, text)
 *   job journal    (append-only, one '|' separated record per finished job)
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>

#include "job.h"

#if defined(__GNUC__)
#define PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define PRINTF_LIKE(fmt_idx, arg_idx)
#endif

typedef enum { LOG_INFO = 0, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct Logger Logger; /* opaque */

typedef struct JournalSummary {
    size_t valid_records;
    size_t malformed_records;
    size_t completed;
    size_t failed;
} JournalSummary;

#define JOURNAL_FORMAT_TAG "J1"
#define JOURNAL_FIELDS     10

Logger *logger_open(const char *exec_path, const char *error_path,
                    const char *journal_path);
void    logger_close(Logger *lg);

/* All functions accept lg == NULL and then do nothing. */
void logger_log(Logger *lg, LogLevel level, const char *fmt, ...) PRINTF_LIKE(3, 4);
void logger_journal(Logger *lg, const Job *job);

/* Reads a journal safely: malformed or truncated lines are counted and
 * skipped. Returns 0 (also when the file does not exist) or -1. */
int logger_read_journal(const char *path, JournalSummary *out);

/* mkdir that treats "already exists" as success. */
int ensure_directory(const char *path);

#endif /* LOGGER_H */
