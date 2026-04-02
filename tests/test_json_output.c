/*
 * test_json_output.c — Unit tests for json_output.c
 *
 * Tests:
 *   1. Minimal struct produces valid JSON with status field present
 *   2. status field is always present
 *   3. files_modified array — empty vs non-empty
 *   4. tool_calls array — empty vs non-empty
 *   5. duration_ms renders as integer
 *   6. NULL result renders as null (not absent)
 *
 * All test harness output goes to stderr so the internal dup2/pipe trick
 * used to capture json_output_print's stdout writes does not swallow it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/json_output.h"

/* --------------------------------------------------------------------------
 * Minimal test harness — uses stderr to avoid stdout capture conflict
 * -------------------------------------------------------------------------- */

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        fprintf(stderr, "  PASS: %s\n", msg); \
        g_passed++; \
    } else { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failed++; \
    } \
} while (0)

/* Capture stdout into a buffer via a pipe/dup2 pair. */
static char *capture_stdout_of(void (*fn)(const JsonOutput *), const JsonOutput *arg,
                                 char *buf, size_t bufsz)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    /* Redirect stdout to the write end */
    int saved_fd = dup(STDOUT_FILENO);
    fflush(stdout);
    dup2(pipefd[1], STDOUT_FILENO);

    fn(arg);
    fflush(stdout);

    /* Restore stdout */
    dup2(saved_fd, STDOUT_FILENO);
    close(saved_fd);
    close(pipefd[1]);

    ssize_t n = read(pipefd[0], buf, (ssize_t)bufsz - 1);
    close(pipefd[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return buf;
}

/* --------------------------------------------------------------------------
 * Test functions
 * -------------------------------------------------------------------------- */

static void test_minimal(void)
{
    fprintf(stderr, "test_minimal:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status = "done";
    out.result = "hello";
    out.duration_ms = 42;

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"status\"") != NULL,  "has 'status' key");
    CHECK(strstr(buf, "\"done\"")   != NULL,  "status value is 'done'");
    CHECK(strstr(buf, "\"result\"") != NULL,  "has 'result' key");
    CHECK(strstr(buf, "\"hello\"")  != NULL,  "result value is 'hello'");
    CHECK(buf[0] == '{',                      "starts with {");
    CHECK(strrchr(buf, '}') != NULL,          "ends with }");

    json_output_free(&out);
}

static void test_status_always_present(void)
{
    fprintf(stderr, "test_status_always_present:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    /* status left NULL — should default to "error" */

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"status\"") != NULL,  "has 'status' key even when not set");
    CHECK(strstr(buf, "\"error\"")  != NULL,  "defaults to 'error' status");

    json_output_free(&out);
}

static void test_files_modified_empty(void)
{
    fprintf(stderr, "test_files_modified_empty:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status = "done";
    /* files_modified left NULL */

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"files_modified\":[]") != NULL, "empty files_modified renders as []");

    json_output_free(&out);
}

static void test_files_modified_non_empty(void)
{
    fprintf(stderr, "test_files_modified_non_empty:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status = "done";

    char *files[] = {"src/main.c", "include/foo.h", NULL};
    out.files_modified   = files;
    out.n_files_modified = 2;

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"src/main.c\"")    != NULL, "first file present");
    CHECK(strstr(buf, "\"include/foo.h\"") != NULL, "second file present");

    json_output_free(&out);
}

static void test_tool_calls_empty(void)
{
    fprintf(stderr, "test_tool_calls_empty:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status = "done";

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"tool_calls\":[]") != NULL, "empty tool_calls renders as []");

    json_output_free(&out);
}

static void test_tool_calls_non_empty(void)
{
    fprintf(stderr, "test_tool_calls_non_empty:\n");
    char buf[4096];

    typedef struct { char *tool; char *args_json; long result_size; } TC;
    TC tc[1] = {{"bash", "{\"cmd\":\"ls\"}", 128}};

    JsonOutput out;
    json_output_init(&out);
    out.status       = "done";
    out.tool_calls   = (void *)tc;
    out.n_tool_calls = 1;

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"bash\"")            != NULL, "tool name present");
    CHECK(strstr(buf, "\"cmd\":\"ls\"")      != NULL, "args_json emitted verbatim");
    CHECK(strstr(buf, "\"result_size\":128") != NULL, "result_size present");

    json_output_free(&out);
}

static void test_duration_ms_integer(void)
{
    fprintf(stderr, "test_duration_ms_integer:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status      = "done";
    out.duration_ms = 1234;

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"duration_ms\":1234") != NULL, "duration_ms renders as integer");

    json_output_free(&out);
}

static void test_null_result_renders_as_null(void)
{
    fprintf(stderr, "test_null_result_renders_as_null:\n");
    char buf[4096];
    JsonOutput out;
    json_output_init(&out);
    out.status = "done";
    out.result = NULL;

    capture_stdout_of(json_output_print, &out, buf, sizeof(buf));

    CHECK(strstr(buf, "\"result\":null") != NULL, "null result renders as JSON null");

    json_output_free(&out);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_json_output ===\n");

    test_minimal();
    test_status_always_present();
    test_files_modified_empty();
    test_files_modified_non_empty();
    test_tool_calls_empty();
    test_tool_calls_non_empty();
    test_duration_ms_integer();
    test_null_result_renders_as_null();

    fprintf(stderr, "\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
