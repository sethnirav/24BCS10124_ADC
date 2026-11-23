/*
 * test_common.h - Minimal assertion helpers shared by the test programs.
 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <string.h>

static int g_tests_run, g_tests_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_tests_run++;                                                       \
        if (!(cond)) {                                                       \
            g_tests_failed++;                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                    \
    } while (0)

#define CHECK_STR_CONTAINS(haystack, needle) CHECK(strstr((haystack), (needle)) != NULL)

#define RUN(test_fn)                                \
    do {                                            \
        int before_ = g_tests_failed;               \
        test_fn();                                  \
        printf("  [%s] %s\n",                       \
               g_tests_failed == before_ ? "PASS" : "FAIL", #test_fn); \
    } while (0)

#define TEST_SUMMARY()                                                     \
    (printf("  %d checks, %d failed\n", g_tests_run, g_tests_failed),      \
     g_tests_failed == 0 ? 0 : 1)

#endif
