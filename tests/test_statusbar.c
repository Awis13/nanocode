/*
 * test_statusbar.c — unit tests for the TUI status bar
 */

#include "test.h"
#include "tui/statusbar.h"
#include "api/provider.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

static ProviderConfig make_cfg(ProviderType type, const char *model)
{
    ProviderConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type  = type;
    cfg.model = model;
    return cfg;
}

/*
 * Call statusbar_format_line and return the result as a heap string.
 * Caller must free().
 */
static char *fmt(const StatusBar *sb, int in_tok, int out_tok, int turn)
{
    char buf[512];
    int  n = statusbar_format_line(sb, in_tok, out_tok, turn, buf, (int)sizeof(buf));
    if (n <= 0) return strdup("");
    return strndup(buf, (size_t)n);
}

/*
 * Capture output from statusbar_update into a buffer via a pipe.
 * Returns number of bytes read.
 */
static int capture_update(StatusBar *sb, int in_tok, int out_tok, int turn,
                          char *out, int cap)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;

    /* Swap the fd inside sb temporarily. This works because StatusBar is
     * an opaque struct — but here we test via the public update path by
     * creating sb with fds[1] directly. */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-opus-4-6");
    StatusBar *sb2 = statusbar_new(fds[1], &cfg, 0);

    statusbar_update(sb2, in_tok, out_tok, turn);

    close(fds[1]);
    int n = (int)read(fds[0], out, (size_t)(cap - 1));
    close(fds[0]);
    if (n < 0) n = 0;
    out[n] = '\0';

    statusbar_free(sb2);
    (void)sb;   /* suppress unused parameter warning */
    return n;
}

/* =========================================================================
 * Tests
 * ====================================================================== */

TEST(test_new_free_claude)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);
    statusbar_free(sb);
}

TEST(test_new_free_null_cfg)
{
    StatusBar *sb = statusbar_new(STDOUT_FILENO, NULL, 10);
    ASSERT_NOT_NULL(sb);
    statusbar_free(sb);
}

TEST(test_format_contains_model)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-opus-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "claude-opus-4-6") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_contains_tokens)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 1500, 300, 2);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "in:") != NULL);
    ASSERT_TRUE(strstr(line, "out:") != NULL);
    ASSERT_TRUE(strstr(line, "1,500") != NULL);
    ASSERT_TRUE(strstr(line, "300")   != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_thousands_separator)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    /* 1,234,567 input tokens */
    char *line = fmt(sb, 1234567, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "1,234,567") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_cost_opus)
{
    /* opus: $15 input / $75 output per MTok
     * 1,000,000 input + 0 output = $15.00 */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-opus-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 1000000, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "~$15.00") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_cost_sonnet)
{
    /* sonnet: $3 input / $15 output per MTok
     * 0 input + 1,000,000 output = $15.00 */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 1000000, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "~$15.00") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_cost_ollama_zero)
{
    ProviderConfig cfg = make_cfg(PROVIDER_OLLAMA, "qwen2.5:9b");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 500000, 200000, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "~$0.00") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_turn_unlimited)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);  /* max=0 → ∞ */
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 0, 7);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "turn 7/") != NULL);
    /* ∞ is UTF-8 0xE2 0x88 0x9E */
    ASSERT_TRUE(strstr(line, "\xe2\x88\x9e") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_turn_max)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 20);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 0, 5);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "turn 5/20") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_format_zero_tokens)
{
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-opus-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "in: 0") != NULL);
    ASSERT_TRUE(strstr(line, "out: 0") != NULL);
    ASSERT_TRUE(strstr(line, "~$0.00") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_update_writes_to_fd)
{
    char buf[512];
    int n = capture_update(NULL, 1000, 200, 3, buf, (int)sizeof(buf));
    ASSERT_TRUE(n > 0);
    /* Output should contain ANSI save-cursor sequence */
    ASSERT_TRUE(strstr(buf, "\033[s") != NULL);
    /* And the model name */
    ASSERT_TRUE(strstr(buf, "claude-opus-4-6") != NULL);
}

TEST(test_clear_writes_to_fd)
{
    int fds[2];
    if (pipe(fds) != 0) { ASSERT_TRUE(0); return; }

    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(fds[1], &cfg, 0);
    ASSERT_NOT_NULL(sb);

    statusbar_clear(sb);
    close(fds[1]);

    char buf[256];
    int n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    ASSERT_TRUE(n > 0);
    /* Should contain ANSI save/restore but no model name */
    ASSERT_TRUE(strstr(buf, "\033[s") != NULL);

    statusbar_free(sb);
}

TEST(test_update_null_safe)
{
    /* Must not crash */
    statusbar_update(NULL, 100, 50, 1);
}

TEST(test_clear_null_safe)
{
    statusbar_clear(NULL);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    RUN_TEST(test_new_free_claude);
    RUN_TEST(test_new_free_null_cfg);
    RUN_TEST(test_format_contains_model);
    RUN_TEST(test_format_contains_tokens);
    RUN_TEST(test_format_thousands_separator);
    RUN_TEST(test_format_cost_opus);
    RUN_TEST(test_format_cost_sonnet);
    RUN_TEST(test_format_cost_ollama_zero);
    RUN_TEST(test_format_turn_unlimited);
    RUN_TEST(test_format_turn_max);
    RUN_TEST(test_format_zero_tokens);
    RUN_TEST(test_update_writes_to_fd);
    RUN_TEST(test_clear_writes_to_fd);
    RUN_TEST(test_update_null_safe);
    RUN_TEST(test_clear_null_safe);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
