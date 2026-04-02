/*
 * test_tool_display.c — unit tests for tui/tool_display
 */

#include "test.h"
#include "tui/tool_display.h"
#include "tools/executor.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

/*
 * Capture output written to a pipe by calling fn(fds[1], ...).
 * Returns a heap-allocated NUL-terminated string; caller must free().
 */
typedef void (*DisplayFn1)(int fd, const char *a);
typedef void (*DisplayFn2)(int fd, const char *a, const char *b);
typedef void (*DisplayFnR)(int fd, const ToolResult *r);

static char *capture1(DisplayFn1 fn, const char *a)
{
    int fds[2];
    if (pipe(fds) != 0) return strdup("");
    fn(fds[1], a);
    close(fds[1]);

    char buf[4096];
    int  n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return strdup(buf);
}

static char *capture2(DisplayFn2 fn, const char *a, const char *b)
{
    int fds[2];
    if (pipe(fds) != 0) return strdup("");
    fn(fds[1], a, b);
    close(fds[1]);

    char buf[4096];
    int  n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return strdup(buf);
}

static char *capture_result(int fd_unused, const ToolResult *r)
{
    (void)fd_unused;
    int fds[2];
    if (pipe(fds) != 0) return strdup("");
    tool_display_result(fds[1], r);
    close(fds[1]);

    char buf[8192];
    int  n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return strdup(buf);
}

static ToolResult make_result(int error, const char *content)
{
    ToolResult r;
    r.error   = error;
    r.content = (char *)(uintptr_t)content; /* const cast for test */
    r.len     = content ? strlen(content) : 0;
    return r;
}

/* =========================================================================
 * tool_display_invocation
 * ====================================================================== */

TEST(test_invocation_basic)
{
    char *out = capture2(tool_display_invocation, "bash", "echo hello");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "> bash: echo hello") != NULL);
    free(out);
}

TEST(test_invocation_contains_ansi_dim)
{
    char *out = capture2(tool_display_invocation, "bash", "ls");
    ASSERT_NOT_NULL(out);
    /* Starts with DIM sequence */
    ASSERT_TRUE(strstr(out, "\033[2m") != NULL);
    ASSERT_TRUE(strstr(out, "\033[0m") != NULL);
    free(out);
}

TEST(test_invocation_null_tool)
{
    /* Must not crash */
    char *out = capture2(tool_display_invocation, NULL, "arg");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, ">") != NULL);
    free(out);
}

TEST(test_invocation_null_args)
{
    char *out = capture2(tool_display_invocation, "bash", NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "bash") != NULL);
    free(out);
}

TEST(test_invocation_long_args_truncated)
{
    /* Args longer than ~74 chars should be truncated with "..." */
    char long_args[256];
    memset(long_args, 'x', sizeof(long_args) - 1);
    long_args[sizeof(long_args) - 1] = '\0';

    char *out = capture2(tool_display_invocation, "bash", long_args);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "...") != NULL);
    free(out);
}

TEST(test_invocation_ends_with_newline)
{
    char *out = capture2(tool_display_invocation, "grep", "pattern file.c");
    ASSERT_NOT_NULL(out);
    size_t len = strlen(out);
    ASSERT_TRUE(len > 0 && out[len - 1] == '\n');
    free(out);
}

/* =========================================================================
 * tool_display_result
 * ====================================================================== */

TEST(test_result_shows_content)
{
    ToolResult r = make_result(0, "hello world\n");
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "hello world") != NULL);
    free(out);
}

TEST(test_result_indented_2_spaces)
{
    ToolResult r = make_result(0, "line one\n");
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    /* Output line must start with "  " */
    ASSERT_TRUE(strncmp(out, "  ", 2) == 0 || strstr(out, "\n  ") != NULL ||
                /* after ANSI reset, first content char is after indent */
                strstr(out, "  line one") != NULL);
    free(out);
}

TEST(test_result_truncated_at_5_lines)
{
    /* 8 lines of content */
    const char *content =
        "line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\n";
    ToolResult r = make_result(0, content);
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    /* Should see "3 more lines" footer */
    ASSERT_TRUE(strstr(out, "3 more line") != NULL);
    /* First 5 lines visible */
    ASSERT_TRUE(strstr(out, "line5") != NULL);
    /* line6 should NOT appear in the text (it's in the "more" count) */
    ASSERT_TRUE(strstr(out, "line6") == NULL);
    free(out);
}

TEST(test_result_no_footer_when_le_5_lines)
{
    const char *content = "a\nb\nc\nd\ne\n";
    ToolResult r = make_result(0, content);
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "more line") == NULL);
    free(out);
}

TEST(test_result_footer_singular)
{
    /* Exactly 6 lines → "1 more line" */
    const char *content = "a\nb\nc\nd\ne\nf\n";
    ToolResult r = make_result(0, content);
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "1 more line") != NULL);
    /* Plural "lines" should NOT appear */
    ASSERT_TRUE(strstr(out, "1 more lines") == NULL);
    free(out);
}

TEST(test_result_empty_content)
{
    ToolResult r = make_result(0, "");
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "(empty)") != NULL);
    free(out);
}

TEST(test_result_null_content)
{
    ToolResult r = make_result(0, NULL);
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "(empty)") != NULL);
    free(out);
}

TEST(test_result_error_delegates_to_error)
{
    ToolResult r = make_result(1, "something went wrong");
    char *out = capture_result(0, &r);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "tool error") != NULL);
    ASSERT_TRUE(strstr(out, "something went wrong") != NULL);
    free(out);
}

TEST(test_result_null_safe)
{
    /* Must not crash */
    tool_display_result(STDOUT_FILENO, NULL);
}

/* =========================================================================
 * tool_display_diff
 * ====================================================================== */

TEST(test_diff_added_line_green)
{
    char *out = capture1(tool_display_diff, "+added line\n");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\033[32m") != NULL);   /* GREEN */
    ASSERT_TRUE(strstr(out, "added line") != NULL);
    free(out);
}

TEST(test_diff_removed_line_red)
{
    char *out = capture1(tool_display_diff, "-removed line\n");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\033[31m") != NULL);   /* RED */
    ASSERT_TRUE(strstr(out, "removed line") != NULL);
    free(out);
}

TEST(test_diff_hunk_header_cyan)
{
    char *out = capture1(tool_display_diff, "@@ -1,3 +1,4 @@\n");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\033[36m") != NULL);   /* CYAN */
    free(out);
}

TEST(test_diff_context_line_dim)
{
    char *out = capture1(tool_display_diff, " context line\n");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\033[2m") != NULL);    /* DIM */
    free(out);
}

TEST(test_diff_resets_after_each_line)
{
    const char *diff = "+add\n-rem\n context\n";
    char *out = capture1(tool_display_diff, diff);
    ASSERT_NOT_NULL(out);
    /* At least three RESET sequences, one per line */
    const char *p = out;
    int resets = 0;
    while ((p = strstr(p, "\033[0m")) != NULL) {
        resets++;
        p++;
    }
    ASSERT_TRUE(resets >= 3);
    free(out);
}

TEST(test_diff_null_safe)
{
    tool_display_diff(STDOUT_FILENO, NULL);
}

TEST(test_diff_no_trailing_newline_in_input)
{
    /* Last line without \n should still be displayed */
    char *out = capture1(tool_display_diff, "+no newline at eof");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "no newline at eof") != NULL);
    free(out);
}

/* =========================================================================
 * tool_display_error
 * ====================================================================== */

TEST(test_error_contains_message)
{
    char *out = capture1(tool_display_error, "command not found");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "tool error") != NULL);
    ASSERT_TRUE(strstr(out, "command not found") != NULL);
    free(out);
}

TEST(test_error_uses_red)
{
    char *out = capture1(tool_display_error, "oops");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\033[31m") != NULL);   /* RED */
    free(out);
}

TEST(test_error_starts_with_bang)
{
    char *out = capture1(tool_display_error, "bad");
    ASSERT_NOT_NULL(out);
    /* Skip leading ANSI codes to find '!' */
    ASSERT_TRUE(strstr(out, "! tool error:") != NULL);
    free(out);
}

TEST(test_error_null_msg)
{
    /* Must not crash and must produce some output */
    int fds[2];
    if (pipe(fds) != 0) return;
    tool_display_error(fds[1], NULL);
    close(fds[1]);
    char buf[256];
    int n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    ASSERT_TRUE(n > 0);
}

TEST(test_error_ends_with_newline)
{
    char *out = capture1(tool_display_error, "msg");
    ASSERT_NOT_NULL(out);
    size_t len = strlen(out);
    ASSERT_TRUE(len > 0 && out[len - 1] == '\n');
    free(out);
}

/* =========================================================================
 * tool_progress (basic smoke tests)
 * ====================================================================== */

TEST(test_progress_start_no_crash)
{
    ToolProgress p;
    tool_progress_start(&p, STDOUT_FILENO);
}

TEST(test_progress_stop_before_threshold_no_output)
{
    int fds[2];
    if (pipe(fds) != 0) return;

    ToolProgress p;
    tool_progress_start(&p, fds[1]);
    /* tick immediately — less than 500 ms elapsed */
    tool_progress_tick(&p);
    tool_progress_stop(&p);
    close(fds[1]);

    char buf[64];
    int n = (int)read(fds[0], buf, sizeof(buf));
    close(fds[0]);
    /* Nothing should have been written */
    ASSERT_EQ(n, 0);
}

TEST(test_progress_null_tick_safe)
{
    tool_progress_tick(NULL);
}

TEST(test_progress_null_stop_safe)
{
    tool_progress_stop(NULL);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    /* invocation */
    RUN_TEST(test_invocation_basic);
    RUN_TEST(test_invocation_contains_ansi_dim);
    RUN_TEST(test_invocation_null_tool);
    RUN_TEST(test_invocation_null_args);
    RUN_TEST(test_invocation_long_args_truncated);
    RUN_TEST(test_invocation_ends_with_newline);

    /* result */
    RUN_TEST(test_result_shows_content);
    RUN_TEST(test_result_indented_2_spaces);
    RUN_TEST(test_result_truncated_at_5_lines);
    RUN_TEST(test_result_no_footer_when_le_5_lines);
    RUN_TEST(test_result_footer_singular);
    RUN_TEST(test_result_empty_content);
    RUN_TEST(test_result_null_content);
    RUN_TEST(test_result_error_delegates_to_error);
    RUN_TEST(test_result_null_safe);

    /* diff */
    RUN_TEST(test_diff_added_line_green);
    RUN_TEST(test_diff_removed_line_red);
    RUN_TEST(test_diff_hunk_header_cyan);
    RUN_TEST(test_diff_context_line_dim);
    RUN_TEST(test_diff_resets_after_each_line);
    RUN_TEST(test_diff_null_safe);
    RUN_TEST(test_diff_no_trailing_newline_in_input);

    /* error */
    RUN_TEST(test_error_contains_message);
    RUN_TEST(test_error_uses_red);
    RUN_TEST(test_error_starts_with_bang);
    RUN_TEST(test_error_null_msg);
    RUN_TEST(test_error_ends_with_newline);

    /* progress */
    RUN_TEST(test_progress_start_no_crash);
    RUN_TEST(test_progress_stop_before_threshold_no_output);
    RUN_TEST(test_progress_null_tick_safe);
    RUN_TEST(test_progress_null_stop_safe);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
