/*
 * task_handlers.c - The five built-in task implementations and the
 *                   function-pointer registry that dispatches to them.
 */
#include "task_handlers.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- CRC32 */

/* Bitwise IEEE CRC-32 (no shared lookup table => trivially thread-safe).
 * Pass crc = 0 to start; feed the previous result to continue a stream. */
uint32_t crc32_compute(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---------------------------------------------------- 1. Checksum task */

static int handle_checksum(const Job *job, char *result, size_t rlen, char *error, size_t elen)
{
    size_t len = strnlen(job->input.text, sizeof job->input.text);
    if (len == 0) { snprintf(error, elen, "checksum: no input data"); return -1; }

    uint32_t crc = crc32_compute(0, job->input.text, len);
    unsigned long byte_sum = 0;
    for (size_t i = 0; i < len; i++) byte_sum += (unsigned char)job->input.text[i];

    snprintf(result, rlen, "CRC32=0x%08X bytes=%zu byte_sum=%lu", (unsigned)crc, len, byte_sum);
    return 0;
}

/* ---------------------------------------------- 2. Data validation task */
/* Input: "name:age;name:age;..."  name = 1..32 letters/spaces, age 0..150 */

static bool validate_record(char *record)
{
    char *colon = strchr(record, ':');
    if (!colon) return false;
    *colon = '\0';
    const char *name = record, *age_text = colon + 1;

    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 32) return false;
    for (size_t i = 0; i < nlen; i++)
        if (!isalpha((unsigned char)name[i]) && name[i] != ' ') return false;

    char *end = NULL;
    errno = 0;
    long age = strtol(age_text, &end, 10);
    if (errno != 0 || end == age_text || *end != '\0') return false;
    return age >= 0 && age <= 150;
}

static int handle_validation(const Job *job, char *result, size_t rlen, char *error, size_t elen)
{
    char buf[JOB_INPUT_LEN];
    snprintf(buf, sizeof buf, "%s", job->input.text); /* strtok_r modifies */

    int total = 0, valid = 0, first_invalid = 0;
    char *save = NULL;
    for (char *rec = strtok_r(buf, ";", &save); rec; rec = strtok_r(NULL, ";", &save)) {
        while (*rec == ' ') rec++;
        if (*rec == '\0') continue;
        total++;
        if (validate_record(rec)) valid++;
        else if (!first_invalid) first_invalid = total;
    }
    if (total == 0) { snprintf(error, elen, "validation: no records found"); return -1; }

    if (first_invalid)
        snprintf(result, rlen, "records=%d valid=%d invalid=%d first_invalid=#%d",
                 total, valid, total - valid, first_invalid);
    else
        snprintf(result, rlen, "records=%d valid=%d invalid=0 (all records valid)", total, valid);
    return 0;
}

/* ---------------------------------------------- 3. File processing task */
/* Input: sub-directory of TEST_DATA_DIR ("" = the directory itself).
 * Only regular files directly inside it are read (no recursion). */

static bool safe_relative_path(const char *p)
{
    if (p[0] == '/') return false;
    if (strstr(p, "..")) return false;
    for (const char *c = p; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '/' && *c != '.')
            return false;
    return true;
}

static int handle_file_processing(const Job *job, char *result, size_t rlen, char *error, size_t elen)
{
    const char *sub = job->input.text;
    if (!safe_relative_path(sub)) {
        snprintf(error, elen, "file_proc: path '%s' rejected (must stay inside %s/)", sub, TEST_DATA_DIR);
        return -1;
    }
    char dir_path[512];
    snprintf(dir_path, sizeof dir_path, "%s%s%s", TEST_DATA_DIR, sub[0] ? "/" : "", sub);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        char ebuf[128];
        int err = errno;
        if (strerror_r(err, ebuf, sizeof ebuf) != 0) snprintf(ebuf, sizeof ebuf, "errno %d", err);
        snprintf(error, elen, "file_proc: cannot open '%s': %s", dir_path, ebuf);
        return -1;
    }

    int files = 0, unreadable = 0;
    unsigned long long bytes = 0, lines = 0;
    uint32_t combined = 0;
    unsigned char chunk[4096];
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; /* skip ., .. and hidden */
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir_path, entry->d_name) >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        FILE *fp = fopen(path, "rb");
        if (!fp) { unreadable++; continue; }
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
            bytes += n;
            for (size_t i = 0; i < n; i++) if (chunk[i] == '\n') lines++;
            combined = crc32_compute(combined, chunk, n);
        }
        if (ferror(fp)) unreadable++;
        else files++;
        fclose(fp);
    }
    closedir(dir);

    if (files == 0 && unreadable > 0) {
        snprintf(error, elen, "file_proc: %d file(s) in '%s' could not be read", unreadable, dir_path);
        return -1;
    }
    snprintf(result, rlen, "dir=%s files=%d bytes=%llu lines=%llu crc32=0x%08X unreadable=%d",
             dir_path, files, bytes, lines, (unsigned)combined, unreadable);
    return 0;
}

/* -------------------------------------------------- 4. Calculation task */

static unsigned long long count_primes(unsigned long long n, int *ok)
{
    *ok = 1;
    if (n < 2) return 0;
    /* Sieve of Eratosthenes; calloc gives a zeroed "is composite" array. */
    unsigned char *composite = calloc((size_t)n + 1, 1);
    if (!composite) { *ok = 0; return 0; }
    unsigned long long count = 0;
    for (unsigned long long i = 2; i <= n; i++) {
        if (composite[i]) continue;
        count++;
        for (unsigned long long j = i * i; j <= n; j += i) composite[j] = 1;
    }
    free(composite);
    return count;
}

static int handle_calculation(const Job *job, char *result, size_t rlen, char *error, size_t elen)
{
    unsigned long long n = job->input.calc.n, value = 0;
    switch (job->input.calc.op) {
    case CALC_SUM:
        for (unsigned long long i = 1; i <= n; i++) value += i;
        break;
    case CALC_FACTORIAL:
        value = 1;
        for (unsigned long long i = 2; i <= n; i++) value *= i;
        break;
    case CALC_FIBONACCI: {
        unsigned long long a = 0, b = 1;
        for (unsigned long long i = 0; i < n; i++) { unsigned long long t = a + b; a = b; b = t; }
        value = a;
        break;
    }
    case CALC_PRIME_COUNT: {
        int ok;
        value = count_primes(n, &ok);
        if (!ok) { snprintf(error, elen, "calculation: out of memory for sieve(%llu)", n); return -1; }
        break;
    }
    case CALC_OP_COUNT:
    default:
        snprintf(error, elen, "calculation: unknown operation %d", (int)job->input.calc.op);
        return -1;
    }
    snprintf(result, rlen, "%s(%llu) = %llu", calc_op_str(job->input.calc.op), n, value);
    return 0;
}

/* ------------------------------------------- 5. Failure simulation task */
/* Fails on the first 'fail_attempts' attempts (retry_count counts them),
 * then succeeds. fail_attempts = -1 fails every time. */

static int handle_failure_sim(const Job *job, char *result, size_t rlen, char *error, size_t elen)
{
    int planned = job->input.failure.fail_attempts;
    int attempt = job->retry_count + 1;
    if (planned < 0 || job->retry_count < planned) {
        if (planned < 0)
            snprintf(error, elen, "simulated permanent failure (attempt %d)", attempt);
        else
            snprintf(error, elen, "simulated transient failure (attempt %d of %d failing)", attempt, planned);
        return -1;
    }
    snprintf(result, rlen, "succeeded on attempt %d after %d simulated failure(s)", attempt, planned);
    return 0;
}

/* --------------------------------------------------------- the registry */
/* Indexed by TaskType so lookup is O(1); the 'type' field is checked by
 * the unit tests to make sure the table and the enum stay in sync. */

static const TaskDescriptor REGISTRY[TASK_TYPE_COUNT] = {
    { TASK_CHECKSUM,        "Checksum",          "CRC-32 and byte sum of the supplied text",            handle_checksum },
    { TASK_VALIDATION,      "Data Validation",   "Validate 'name:age;...' records (age 0-150)",         handle_validation },
    { TASK_FILE_PROCESSING, "File Processing",   "Count files/bytes/lines and CRC-32 in " TEST_DATA_DIR "/", handle_file_processing },
    { TASK_CALCULATION,     "Calculation",       "sum / factorial / fibonacci / prime count",           handle_calculation },
    { TASK_FAILURE_SIM,     "Failure Simulation","Fails N times to exercise the retry mechanism",       handle_failure_sim },
};

size_t task_registry_count(void) { return TASK_TYPE_COUNT; }

const TaskDescriptor *task_registry_get(TaskType type)
{
    return task_type_valid((int)type) ? &REGISTRY[type] : NULL;
}

TaskHandler task_registry_lookup(TaskType type)
{
    const TaskDescriptor *d = task_registry_get(type);
    return d ? d->handler : NULL;
}

int task_execute(const Job *job, char *result, size_t result_len, char *error, size_t error_len)
{
    if (!job || !result || !error || result_len == 0 || error_len == 0) return -1;
    result[0] = error[0] = '\0';
    TaskHandler handler = task_registry_lookup(job->type);
    if (!handler) {
        snprintf(error, error_len, "no handler registered for task type %d", (int)job->type);
        return -1;
    }
    return handler(job, result, result_len, error, error_len); /* call via pointer */
}

int task_validate_input(TaskType type, const TaskInput *in, char *error, size_t elen)
{
    if (!in) { snprintf(error, elen, "missing input"); return -1; }
    switch (type) {
    case TASK_CHECKSUM:
    case TASK_VALIDATION:
        if (memchr(in->text, '\0', sizeof in->text) == NULL) { snprintf(error, elen, "input not terminated"); return -1; }
        if (in->text[0] == '\0') { snprintf(error, elen, "input text is empty"); return -1; }
        return 0;
    case TASK_FILE_PROCESSING:
        if (memchr(in->text, '\0', sizeof in->text) == NULL) { snprintf(error, elen, "input not terminated"); return -1; }
        if (!safe_relative_path(in->text)) { snprintf(error, elen, "directory must be a relative path inside %s/ without '..'", TEST_DATA_DIR); return -1; }
        return 0;
    case TASK_CALCULATION: {
        unsigned long long n = in->calc.n, limit;
        switch (in->calc.op) {
        case CALC_SUM:         limit = CALC_SUM_MAX; break;
        case CALC_FACTORIAL:   limit = CALC_FACTORIAL_MAX; break;
        case CALC_FIBONACCI:   limit = CALC_FIBONACCI_MAX; break;
        case CALC_PRIME_COUNT: limit = CALC_PRIME_MAX; break;
        case CALC_OP_COUNT:
        default: snprintf(error, elen, "unknown calculation operation"); return -1;
        }
        if (n > limit) { snprintf(error, elen, "%s requires n <= %llu", calc_op_str(in->calc.op), limit); return -1; }
        return 0;
    }
    case TASK_FAILURE_SIM:
        if (in->failure.fail_attempts < -1 || in->failure.fail_attempts > MAX_RETRY_LIMIT + 1) {
            snprintf(error, elen, "fail_attempts must be -1..%d", MAX_RETRY_LIMIT + 1);
            return -1;
        }
        return 0;
    case TASK_TYPE_COUNT:
    default:
        snprintf(error, elen, "unknown task type");
        return -1;
    }
}
