/*
 * benchmark.h — auto-benchmark system (CMP-404)
 *
 * Runs a standard test suite against the configured model, scores results,
 * and optionally compares results across models or auto-tunes provider params.
 *
 * CLI flags:
 *   --benchmark            run test suite, store results, exit 0/1
 *   --benchmark --compare  show side-by-side results across stored models
 *   --benchmark --tune     run suite then adjust temperature based on scores
 *
 * Test cases live in:
 *   Built-in defaults     — embedded in benchmark.c (5 per category, 20 total)
 *   User-defined          — ~/.config/nanocode/benchmarks/mytest.toml
 *
 * Results are stored as JSON in:
 *   ~/.local/share/nanocode/benchmark-results/<model>-<timestamp>.json
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "../src/api/provider.h"
#include "../include/config.h"

/* -------------------------------------------------------------------------
 * Categories and scoring modes
 * ---------------------------------------------------------------------- */

typedef enum {
    BENCH_CAT_TOOL_CALLING = 0,
    BENCH_CAT_CODE_GEN,
    BENCH_CAT_INSTRUCTION_FOLLOWING,
    BENCH_CAT_CONTEXT_UTILIZATION,
    BENCH_CAT_COUNT
} BenchCategory;

typedef enum {
    BENCH_SCORE_EXACT_MATCH = 0,   /* response must contain expected string verbatim */
    BENCH_SCORE_FUZZY_MATCH,       /* expected string appears (case-insensitive) */
    BENCH_SCORE_COMPILATION_CHECK  /* response contains C code that compiles */
} BenchScoring;

/* -------------------------------------------------------------------------
 * A single test case
 * ---------------------------------------------------------------------- */

#define BENCH_PROMPT_MAX   2048
#define BENCH_EXPECTED_MAX  512
#define BENCH_NAME_MAX       64

typedef struct {
    char          name[BENCH_NAME_MAX];
    BenchCategory category;
    char          prompt[BENCH_PROMPT_MAX];
    char          expected[BENCH_EXPECTED_MAX]; /* substring to match against */
    BenchScoring  scoring;
} BenchCase;

/* -------------------------------------------------------------------------
 * Per-test result
 * ---------------------------------------------------------------------- */

#define BENCH_RESPONSE_MAX 8192
#define BENCH_ERROR_MAX     256

typedef struct {
    char          name[BENCH_NAME_MAX];
    BenchCategory category;
    int           passed;          /* 1 = passed threshold, 0 = failed */
    double        score;           /* 0.0–1.0 */
    char          response[BENCH_RESPONSE_MAX];
    char          error[BENCH_ERROR_MAX];
} BenchResult;

/* -------------------------------------------------------------------------
 * Full benchmark report
 * ---------------------------------------------------------------------- */

#define BENCH_MODEL_MAX  128
#define BENCH_TIME_MAX    32

typedef struct {
    char         model[BENCH_MODEL_MAX];
    char         timestamp[BENCH_TIME_MAX];   /* ISO-8601 UTC */
    int          n_results;
    BenchResult *results;                     /* heap-allocated array */
    int          n_passed;
    int          n_failed;
    double       overall_score;              /* average 0.0–1.0 */
} BenchReport;

/* -------------------------------------------------------------------------
 * CLI flags struct
 * ---------------------------------------------------------------------- */

typedef struct {
    int do_compare; /* --compare */
    int do_tune;    /* --tune    */
} BenchFlags;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * benchmark_run — top-level entry point called from main().
 *
 * Runs the test suite against the configured provider, stores results, and
 * (when flags->do_compare) prints a comparison table across stored results.
 * When flags->do_tune, adjusts provider temperature via config_set/config_save
 * based on scores.
 *
 * Returns 0 if all tests passed their threshold, 1 if any failed.
 */
int benchmark_run(const BenchFlags *flags, const ProviderConfig *pcfg,
                  Config *cfg);

/* -------------------------------------------------------------------------
 * Testable sub-functions (exposed for unit tests)
 * ---------------------------------------------------------------------- */

/*
 * Score a single test case's response against its expected value.
 * Returns 1 (pass) or 0 (fail).  score_out is set to 0.0 or 1.0.
 */
int benchmark_score(const BenchCase *tc, const char *response,
                    double *score_out);

/*
 * Count the number of built-in test cases.
 */
int benchmark_builtin_count(void);

/*
 * Fill `out` array with the built-in test cases.
 * `cap` is the capacity of `out`; returns number of cases written.
 */
int benchmark_builtin_cases(BenchCase *out, int cap);

/*
 * Render a summary table to stdout for a single report.
 */
void benchmark_print_report(const BenchReport *report);

/*
 * Free heap memory owned by a BenchReport.
 */
void benchmark_report_free(BenchReport *report);

/*
 * Return the human-readable category name.
 */
const char *benchmark_category_name(BenchCategory cat);

#endif /* BENCHMARK_H */
