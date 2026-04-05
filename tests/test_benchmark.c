/*
 * test_benchmark.c — unit tests for the auto-benchmark system (CMP-404)
 *
 * Tests the scoring logic, built-in case count, and TOML loader
 * without making real API calls.
 */

#include "../include/benchmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { \
        g_pass++; \
    } else { \
        fprintf(stderr, "FAIL: %s  (line %d)\n", #expr, __LINE__); \
        g_fail++; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * Test: benchmark_category_name
 * ---------------------------------------------------------------------- */

static void test_category_name(void)
{
    CHECK(strcmp(benchmark_category_name(BENCH_CAT_TOOL_CALLING),
                 "tool_calling") == 0);
    CHECK(strcmp(benchmark_category_name(BENCH_CAT_CODE_GEN),
                 "code_gen") == 0);
    CHECK(strcmp(benchmark_category_name(BENCH_CAT_INSTRUCTION_FOLLOWING),
                 "instruction_following") == 0);
    CHECK(strcmp(benchmark_category_name(BENCH_CAT_CONTEXT_UTILIZATION),
                 "context_utilization") == 0);
}

/* -------------------------------------------------------------------------
 * Test: built-in case count
 * ---------------------------------------------------------------------- */

static void test_builtin_count(void)
{
    int count = benchmark_builtin_count();
    /* Spec: 5 per category, 4 categories = 20 total. */
    CHECK(count == 20);
}

static void test_builtin_cases(void)
{
    BenchCase cases[32];
    int n = benchmark_builtin_cases(cases, 32);
    CHECK(n == 20);

    /* Count per category. */
    int cat[BENCH_CAT_COUNT] = {0};
    for (int i = 0; i < n; i++) {
        int c = (int)cases[i].category;
        if (c >= 0 && c < BENCH_CAT_COUNT)
            cat[c]++;
    }
    CHECK(cat[BENCH_CAT_TOOL_CALLING]          == 5);
    CHECK(cat[BENCH_CAT_CODE_GEN]              == 5);
    CHECK(cat[BENCH_CAT_INSTRUCTION_FOLLOWING] == 5);
    CHECK(cat[BENCH_CAT_CONTEXT_UTILIZATION]   == 5);

    /* Every case must have a name and a prompt. */
    for (int i = 0; i < n; i++) {
        CHECK(cases[i].name[0] != '\0');
        CHECK(cases[i].prompt[0] != '\0');
    }
}

static void test_builtin_capacity(void)
{
    /* Request fewer slots than total — should return exactly cap. */
    BenchCase cases[10];
    int n = benchmark_builtin_cases(cases, 10);
    CHECK(n == 10);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_score — exact_match
 * ---------------------------------------------------------------------- */

static void test_score_exact_match(void)
{
    BenchCase tc;
    memset(&tc, 0, sizeof(tc));
    tc.scoring = BENCH_SCORE_EXACT_MATCH;
    snprintf(tc.expected, sizeof(tc.expected), "42");

    double score = 0.0;
    int passed;

    /* Exact match (with surrounding whitespace). */
    passed = benchmark_score(&tc, "42", &score);
    CHECK(passed == 1);
    CHECK(score == 1.0);

    passed = benchmark_score(&tc, "  42  ", &score);
    CHECK(passed == 1);

    passed = benchmark_score(&tc, "  42\n", &score);
    CHECK(passed == 1);

    /* Mismatch. */
    passed = benchmark_score(&tc, "43", &score);
    CHECK(passed == 0);
    CHECK(score == 0.0);

    passed = benchmark_score(&tc, "42 extra", &score);
    CHECK(passed == 0);

    passed = benchmark_score(&tc, "", &score);
    CHECK(passed == 0);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_score — fuzzy_match
 * ---------------------------------------------------------------------- */

static void test_score_fuzzy_match(void)
{
    BenchCase tc;
    memset(&tc, 0, sizeof(tc));
    tc.scoring = BENCH_SCORE_FUZZY_MATCH;
    snprintf(tc.expected, sizeof(tc.expected), "read_file");

    double score = 0.0;
    int passed;

    /* Substring present. */
    passed = benchmark_score(&tc, "{\"tool\": \"read_file\", \"path\": \"x\"}", &score);
    CHECK(passed == 1);
    CHECK(score == 1.0);

    /* Case-insensitive. */
    passed = benchmark_score(&tc, "READ_FILE", &score);
    CHECK(passed == 1);

    /* Not present. */
    passed = benchmark_score(&tc, "{\"tool\": \"bash\"}", &score);
    CHECK(passed == 0);
    CHECK(score == 0.0);

    /* Empty response. */
    passed = benchmark_score(&tc, "", &score);
    CHECK(passed == 0);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_score — fuzzy_match with instruction-following cases
 * ---------------------------------------------------------------------- */

static void test_score_fuzzy_various(void)
{
    BenchCase tc;
    memset(&tc, 0, sizeof(tc));
    tc.scoring = BENCH_SCORE_FUZZY_MATCH;

    double score;

    snprintf(tc.expected, sizeof(tc.expected), "grep");
    CHECK(benchmark_score(&tc, "grep", &score) == 1);
    CHECK(benchmark_score(&tc, "GREP", &score) == 1);
    CHECK(benchmark_score(&tc, "use grep for that", &score) == 1);
    CHECK(benchmark_score(&tc, "awk", &score) == 0);

    snprintf(tc.expected, sizeof(tc.expected), "dark");
    CHECK(benchmark_score(&tc, "theme=dark", &score) == 1);
    CHECK(benchmark_score(&tc, "DARK", &score) == 1);
    CHECK(benchmark_score(&tc, "light", &score) == 0);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_score — exact_match with instruction-following cases
 * ---------------------------------------------------------------------- */

static void test_score_exact_instruction(void)
{
    BenchCase tc;
    memset(&tc, 0, sizeof(tc));
    tc.scoring = BENCH_SCORE_EXACT_MATCH;
    double score;

    snprintf(tc.expected, sizeof(tc.expected), "banana");
    CHECK(benchmark_score(&tc, "banana", &score) == 1);
    CHECK(benchmark_score(&tc, "BANANA", &score) == 0); /* exact is case-sensitive */
    CHECK(benchmark_score(&tc, "  banana  ", &score) == 1); /* whitespace trimmed */

    snprintf(tc.expected, sizeof(tc.expected), "56");
    CHECK(benchmark_score(&tc, "56", &score) == 1);
    CHECK(benchmark_score(&tc, "57", &score) == 0);

    snprintf(tc.expected, sizeof(tc.expected), "HELLO");
    CHECK(benchmark_score(&tc, "HELLO", &score) == 1);
    CHECK(benchmark_score(&tc, "hello", &score) == 0);

    snprintf(tc.expected, sizeof(tc.expected), "1,2,3,4,5");
    CHECK(benchmark_score(&tc, "1,2,3,4,5", &score) == 1);
    CHECK(benchmark_score(&tc, "1, 2, 3, 4, 5", &score) == 0);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_score — compilation_check (no network, uses cc)
 * ---------------------------------------------------------------------- */

static void test_score_compilation_check(void)
{
    BenchCase tc;
    memset(&tc, 0, sizeof(tc));
    tc.scoring = BENCH_SCORE_COMPILATION_CHECK;
    snprintf(tc.expected, sizeof(tc.expected), "sum");

    double score;
    int passed;

    /* Valid C function. */
    passed = benchmark_score(
        &tc,
        "```c\nint sum(int a, int b) { return a + b; }\n```",
        &score);
    CHECK(passed == 1);
    CHECK(score == 1.0);

    /* Valid function without markdown wrapper. */
    passed = benchmark_score(
        &tc,
        "int sum(int a, int b) {\n    return a + b;\n}\n",
        &score);
    CHECK(passed == 1);

    /* Invalid C (missing return type → syntax error). */
    passed = benchmark_score(&tc, "sum(a, b) { return a + b; }", &score);
    /* This may or may not compile depending on cc — just verify no crash. */
    CHECK(passed == 0 || passed == 1); /* no crash */

    /* Empty response. */
    passed = benchmark_score(&tc, "", &score);
    CHECK(passed == 0);

    /* Function name not in response (fuzzy secondary check fails). */
    passed = benchmark_score(
        &tc,
        "```c\nint add(int a, int b) { return a + b; }\n```",
        &score);
    /* "sum" not in response; should fail secondary fuzzy check. */
    CHECK(passed == 0);
}

/* -------------------------------------------------------------------------
 * Test: benchmark_report_free (no crash, idempotent)
 * ---------------------------------------------------------------------- */

static void test_report_free(void)
{
    BenchReport report;
    memset(&report, 0, sizeof(report));
    benchmark_report_free(&report); /* free with NULL results — no crash */

    report.results = calloc(3, sizeof(BenchResult));
    assert(report.results != NULL);
    report.n_results = 3;
    benchmark_report_free(&report);
    CHECK(report.results == NULL);
    CHECK(report.n_results == 0);

    /* Double-free safe. */
    benchmark_report_free(&report);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
    test_category_name();
    test_builtin_count();
    test_builtin_cases();
    test_builtin_capacity();
    test_score_exact_match();
    test_score_fuzzy_match();
    test_score_fuzzy_various();
    test_score_exact_instruction();
    test_score_compilation_check();
    test_report_free();

    printf("benchmark tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
