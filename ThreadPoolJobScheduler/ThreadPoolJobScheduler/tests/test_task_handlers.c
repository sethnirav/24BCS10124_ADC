/*
 * test_task_handlers.c - Each handler, the function-pointer registry and
 *                        input validation.
 */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"
#include "task_handlers.h"
#include "test_common.h"

static char result[JOB_MESSAGE_LEN], error[JOB_MESSAGE_LEN];

static int run(TaskType type, const TaskInput *in, int retry_count)
{
    Job *j = job_create(1, "t", type, PRIORITY_NORMAL, in, MAX_RETRY_LIMIT);
    j->retry_count = retry_count;
    int rc = task_execute(j, result, sizeof result, error, sizeof error);
    job_destroy(j);
    return rc;
}

static TaskInput text(const char *s)
{
    TaskInput in;
    memset(&in, 0, sizeof in);
    snprintf(in.text, sizeof in.text, "%s", s);
    return in;
}

static TaskInput calc(CalcOp op, uint64_t n)
{
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.calc.op = op;
    in.calc.n = n;
    return in;
}

static void test_registry(void)
{
    CHECK(task_registry_count() == TASK_TYPE_COUNT);
    for (int t = 0; t < TASK_TYPE_COUNT; t++) {
        const TaskDescriptor *d = task_registry_get((TaskType)t);
        CHECK(d != NULL && (int)d->type == t);
        CHECK(d && d->handler != NULL && d->handler == task_registry_lookup((TaskType)t));
    }
    CHECK(task_registry_get(TASK_TYPE_COUNT) == NULL);
    CHECK(task_registry_lookup((TaskType)99) == NULL);
}

static void test_checksum(void)
{
    CHECK(crc32_compute(0, "123456789", 9) == 0xCBF43926u); /* standard check value */
    /* streaming in two parts gives the same CRC */
    CHECK(crc32_compute(crc32_compute(0, "12345", 5), "6789", 4) == 0xCBF43926u);
    TaskInput in = text("123456789");
    CHECK(run(TASK_CHECKSUM, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "0xCBF43926");
    CHECK_STR_CONTAINS(result, "bytes=9");
    TaskInput empty = text("");
    CHECK(run(TASK_CHECKSUM, &empty, 0) == -1);
    CHECK(error[0] != '\0');
}

static void test_validation(void)
{
    TaskInput in = text("alice:30;bob:abc;:5;carol:200;dave:45");
    CHECK(run(TASK_VALIDATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "records=5 valid=2 invalid=3 first_invalid=#2");
    TaskInput good = text("ann lee:0;bo:150");
    CHECK(run(TASK_VALIDATION, &good, 0) == 0);
    CHECK_STR_CONTAINS(result, "invalid=0");
    TaskInput none = text(";;;");
    CHECK(run(TASK_VALIDATION, &none, 0) == -1);
}

static void test_calculation(void)
{
    TaskInput in = calc(CALC_FACTORIAL, 20);
    CHECK(run(TASK_CALCULATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "= 2432902008176640000");
    in = calc(CALC_FIBONACCI, 90);
    CHECK(run(TASK_CALCULATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "= 2880067194370816120");
    in = calc(CALC_PRIME_COUNT, 1000000);
    CHECK(run(TASK_CALCULATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "= 78498");
    in = calc(CALC_PRIME_COUNT, 1);
    CHECK(run(TASK_CALCULATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "= 0");
    in = calc(CALC_SUM, 100);
    CHECK(run(TASK_CALCULATION, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "= 5050");
}

static void test_input_validation(void)
{
    char err[128];
    TaskInput in = calc(CALC_FIBONACCI, 94);
    CHECK(task_validate_input(TASK_CALCULATION, &in, err, sizeof err) != 0);
    in = calc(CALC_PRIME_COUNT, CALC_PRIME_MAX + 1);
    CHECK(task_validate_input(TASK_CALCULATION, &in, err, sizeof err) != 0);
    in = calc((CalcOp)9, 1);
    CHECK(task_validate_input(TASK_CALCULATION, &in, err, sizeof err) != 0);
    in = calc(CALC_SUM, 10);
    CHECK(task_validate_input(TASK_CALCULATION, &in, err, sizeof err) == 0);
    TaskInput p = text("../etc");
    CHECK(task_validate_input(TASK_FILE_PROCESSING, &p, err, sizeof err) != 0);
    p = text("/etc");
    CHECK(task_validate_input(TASK_FILE_PROCESSING, &p, err, sizeof err) != 0);
    p = text("docs; rm -rf x");
    CHECK(task_validate_input(TASK_FILE_PROCESSING, &p, err, sizeof err) != 0);
    p = text("docs");
    CHECK(task_validate_input(TASK_FILE_PROCESSING, &p, err, sizeof err) == 0);
    TaskInput f;
    memset(&f, 0, sizeof f);
    f.failure.fail_attempts = -2;
    CHECK(task_validate_input(TASK_FAILURE_SIM, &f, err, sizeof err) != 0);
}

static void write_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(content, fp); fclose(fp); }
}

static void test_file_processing(void)
{
    ensure_directory(TEST_DATA_DIR);
    ensure_directory(TEST_DATA_DIR "/unit_tmp");
    write_file(TEST_DATA_DIR "/unit_tmp/a.txt", "one\ntwo\n");       /* 8 bytes, 2 lines */
    write_file(TEST_DATA_DIR "/unit_tmp/b.txt", "three\n");          /* 6 bytes, 1 line  */
    ensure_directory(TEST_DATA_DIR "/unit_tmp/nested");              /* ignored */

    TaskInput in = text("unit_tmp");
    CHECK(run(TASK_FILE_PROCESSING, &in, 0) == 0);
    CHECK_STR_CONTAINS(result, "files=2 bytes=14 lines=3");

    TaskInput missing = text("does_not_exist");
    CHECK(run(TASK_FILE_PROCESSING, &missing, 0) == -1);
    CHECK_STR_CONTAINS(error, "cannot open");

    TaskInput evil = text("../src");
    CHECK(run(TASK_FILE_PROCESSING, &evil, 0) == -1); /* handler re-checks */

    remove(TEST_DATA_DIR "/unit_tmp/a.txt");
    remove(TEST_DATA_DIR "/unit_tmp/b.txt");
    rmdir(TEST_DATA_DIR "/unit_tmp/nested");
    rmdir(TEST_DATA_DIR "/unit_tmp");
}

static void test_failure_sim(void)
{
    TaskInput in;
    memset(&in, 0, sizeof in);
    in.failure.fail_attempts = 2;
    CHECK(run(TASK_FAILURE_SIM, &in, 0) == -1);
    CHECK(run(TASK_FAILURE_SIM, &in, 1) == -1);
    CHECK(run(TASK_FAILURE_SIM, &in, 2) == 0);
    in.failure.fail_attempts = -1;
    CHECK(run(TASK_FAILURE_SIM, &in, 9) == -1);
}

int main(void)
{
    printf("test_task_handlers\n");
    RUN(test_registry);
    RUN(test_checksum);
    RUN(test_validation);
    RUN(test_calculation);
    RUN(test_input_validation);
    RUN(test_file_processing);
    RUN(test_failure_sim);
    return TEST_SUMMARY();
}
