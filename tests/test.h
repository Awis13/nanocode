/*
 * test.h — minimal unit test harness
 *
 * Usage:
 *   #include "test.h"
 *
 *   TEST(test_foo) { ASSERT_EQ(1 + 1, 2); }
 *   TEST(test_bar) { ASSERT_STR_EQ("hello", "hello"); }
 *
 *   int main(void) {
 *       RUN_TEST(test_foo);
 *       RUN_TEST(test_bar);
 *       PRINT_SUMMARY();
 *       return g_failures > 0 ? 1 : 0;
 *   }
 */

#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int g_passed   = 0;
static int g_failures = 0;
static int g_in_test  = 0;  /* set to 1 while a test is running */
static int g_test_failed = 0;

/* Declare a test function. */
#define TEST(name) static void name(void)

/* Run a test and record pass/fail. */
#define RUN_TEST(name) do {                                          \
    g_test_failed = 0;                                               \
    g_in_test = 1;                                                   \
    name();                                                          \
    g_in_test = 0;                                                   \
    if (g_test_failed) {                                             \
        fprintf(stderr, "  FAIL: %s\n", #name);                     \
        g_failures++;                                                \
    } else {                                                         \
        fprintf(stderr, "  PASS: %s\n", #name);                     \
        g_passed++;                                                  \
    }                                                                \
} while (0)

/* Print final summary line. */
#define PRINT_SUMMARY() do {                                         \
    fprintf(stderr, "%s: %d passed, %d failed\n",                   \
            g_failures > 0 ? "FAILED" : "OK",                       \
            g_passed, g_failures);                                   \
} while (0)

/* -------------------------------------------------------------------------
 * Assertion macros — all mark the current test as failed and continue.
 * ---------------------------------------------------------------------- */

#define ASSERT_TRUE(expr) do {                                       \
    if (!(expr)) {                                                   \
        fprintf(stderr, "    assertion failed: %s (%s:%d)\n",       \
                #expr, __FILE__, __LINE__);                          \
        g_test_failed = 1;                                           \
    }                                                                \
} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_NULL(ptr)  ASSERT_TRUE((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)

#define ASSERT_EQ(a, b) do {                                         \
    long long _a = (long long)(a);                                   \
    long long _b = (long long)(b);                                   \
    if (_a != _b) {                                                  \
        fprintf(stderr, "    ASSERT_EQ failed: %s == %s "           \
                "(%lld != %lld) (%s:%d)\n",                         \
                #a, #b, _a, _b, __FILE__, __LINE__);                 \
        g_test_failed = 1;                                           \
    }                                                                \
} while (0)

#define ASSERT_NE(a, b) do {                                         \
    long long _a = (long long)(a);                                   \
    long long _b = (long long)(b);                                   \
    if (_a == _b) {                                                  \
        fprintf(stderr, "    ASSERT_NE failed: %s != %s "           \
                "(%lld == %lld) (%s:%d)\n",                         \
                #a, #b, _a, _b, __FILE__, __LINE__);                 \
        g_test_failed = 1;                                           \
    }                                                                \
} while (0)

#define ASSERT_STR_EQ(a, b) do {                                     \
    const char *_a = (const char *)(a);                              \
    const char *_b = (const char *)(b);                              \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {          \
        fprintf(stderr, "    ASSERT_STR_EQ failed: %s == %s "       \
                "(\"%s\" != \"%s\") (%s:%d)\n",                     \
                #a, #b,                                              \
                _a ? _a : "(null)", _b ? _b : "(null)",              \
                __FILE__, __LINE__);                                  \
        g_test_failed = 1;                                           \
    }                                                                \
} while (0)

#define ASSERT_MEM_EQ(a, b, len) do {                                \
    if (memcmp((a), (b), (len)) != 0) {                              \
        fprintf(stderr, "    ASSERT_MEM_EQ failed: %s (%s:%d)\n",   \
                #a, __FILE__, __LINE__);                             \
        g_test_failed = 1;                                           \
    }                                                                \
} while (0)

#endif /* TEST_H */
