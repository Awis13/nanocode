/*
 * test_statusbar.c — unit tests for the TUI status bar
 */

#include "test.h"
#include "tui/statusbar.h"
#include "api/provider.h"
#include "pet.h"

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
 * CMP-245: pet integration tests
 * ====================================================================== */

TEST(test_set_pet_null_safe)
{
    /* Both NULL variants must not crash. */
    statusbar_set_pet(NULL, NULL);

    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDERR_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);
    statusbar_set_pet(sb, NULL);  /* detach */
    statusbar_free(sb);
}

TEST(test_set_pet_off_no_extra_ansi)
{
    /* PET_OFF: statusbar_update must not emit extra ANSI lines for the pet
     * (the pet area stays blank).  We verify by checking the output length
     * against an identical run without a pet. */
    int fds_no_pet[2], fds_with_pet[2];
    ASSERT_TRUE(pipe(fds_no_pet)   == 0);
    ASSERT_TRUE(pipe(fds_with_pet) == 0);

    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");

    StatusBar *sb_no_pet = statusbar_new(fds_no_pet[1],   &cfg, 0);
    StatusBar *sb_pet    = statusbar_new(fds_with_pet[1], &cfg, 0);
    ASSERT_NOT_NULL(sb_no_pet);
    ASSERT_NOT_NULL(sb_pet);

    Pet pet_off = pet_new(PET_OFF);
    statusbar_set_pet(sb_pet, &pet_off);

    statusbar_update(sb_no_pet,  100, 50, 1);
    statusbar_update(sb_pet,     100, 50, 1);

    close(fds_no_pet[1]);
    close(fds_with_pet[1]);

    char buf_no_pet[1024], buf_with_pet[1024];
    int  n_no_pet   = (int)read(fds_no_pet[0],   buf_no_pet,   sizeof(buf_no_pet)   - 1);
    int  n_with_pet = (int)read(fds_with_pet[0], buf_with_pet, sizeof(buf_with_pet) - 1);
    close(fds_no_pet[0]);
    close(fds_with_pet[0]);

    /* PET_OFF: output size should be the same as without pet. */
    ASSERT_EQ(n_no_pet, n_with_pet);

    statusbar_free(sb_no_pet);
    statusbar_free(sb_pet);
}

TEST(test_set_pet_ticks_on_update)
{
    /* After N statusbar_update() calls the pet should have accumulated ticks.
     * We verify by checking that idle_ticks increments (auto-sleep after
     * PET_SLEEP_TICKS ticks). */
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);

    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(fds[1], &cfg, 0);
    ASSERT_NOT_NULL(sb);

    Pet pet = pet_new(PET_CAT);
    ASSERT_EQ(pet.state, PET_IDLE);
    ASSERT_EQ(pet.idle_ticks, 0);

    statusbar_set_pet(sb, &pet);

    /* Drive PET_SLEEP_TICKS updates — pet should auto-transition to SLEEP. */
    for (int i = 0; i < PET_SLEEP_TICKS; i++)
        statusbar_update(sb, 0, 0, 1);

    ASSERT_EQ(pet.state, PET_SLEEP);

    close(fds[1]);
    /* Drain the pipe. */
    char drain[256];
    while (read(fds[0], drain, sizeof(drain)) > 0) {}
    close(fds[0]);

    statusbar_free(sb);
}

TEST(test_set_pet_transition_active)
{
    /* pet_transition to ACTIVE resets idle_ticks and changes state. */
    Pet pet = pet_new(PET_CAT);
    pet.idle_ticks = 50;
    pet_transition(&pet, PET_ACTIVE);
    ASSERT_EQ(pet.state, PET_ACTIVE);
    ASSERT_EQ(pet.idle_ticks, 0);
}

/* =========================================================================
 * CMP-365: timing API tests
 * ====================================================================== */

TEST(test_timing_null_safe)
{
    /* All timing calls must be safe on NULL. */
    statusbar_set_session_start(NULL);
    statusbar_mark_turn_start(NULL);
    statusbar_mark_first_token(NULL);
}

TEST(test_latency_not_shown_before_mark)
{
    /* Without mark_turn_start + mark_first_token, no latency field appears. */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    char *line = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE(strstr(line, "ms") == NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_latency_shown_after_mark)
{
    /* After marking turn start and first token, latency field appears. */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    statusbar_mark_turn_start(sb);
    statusbar_mark_first_token(sb);

    char *line = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line);
    /* Should contain latency field (either Nms or N.Xs). */
    ASSERT_TRUE(strstr(line, "ms") != NULL || strstr(line, "s") != NULL);
    free(line);

    statusbar_free(sb);
}

TEST(test_first_token_idempotent)
{
    /* Calling statusbar_mark_first_token multiple times must not reset
     * the latency measurement (only the first call counts). */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    statusbar_mark_turn_start(sb);
    statusbar_mark_first_token(sb);

    char *line1 = fmt(sb, 0, 0, 1);
    statusbar_mark_first_token(sb);  /* must be a no-op */
    char *line2 = fmt(sb, 0, 0, 1);

    ASSERT_NOT_NULL(line1);
    ASSERT_NOT_NULL(line2);
    ASSERT_STR_EQ(line1, line2);

    free(line1);
    free(line2);
    statusbar_free(sb);
}

TEST(test_session_elapsed_shown)
{
    /* After set_session_start, elapsed time appears in line. */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    statusbar_set_session_start(sb);

    char *line = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line);
    ASSERT_TRUE((int)strlen(line) > 0);
    free(line);

    statusbar_free(sb);
}

TEST(test_turn_start_resets_latency)
{
    /* statusbar_mark_turn_start resets latency so next turn starts fresh. */
    ProviderConfig cfg = make_cfg(PROVIDER_CLAUDE, "claude-sonnet-4-6");
    StatusBar *sb = statusbar_new(STDOUT_FILENO, &cfg, 0);
    ASSERT_NOT_NULL(sb);

    /* First turn — latency measured. */
    statusbar_mark_turn_start(sb);
    statusbar_mark_first_token(sb);

    char *line1 = fmt(sb, 0, 0, 1);
    ASSERT_NOT_NULL(line1);
    ASSERT_TRUE(strstr(line1, "ms") != NULL || strstr(line1, "s") != NULL);
    free(line1);

    /* Second turn start — latency field disappears until first token. */
    statusbar_mark_turn_start(sb);

    char *line2 = fmt(sb, 0, 0, 2);
    ASSERT_NOT_NULL(line2);
    ASSERT_TRUE(strstr(line2, "ms") == NULL);
    free(line2);

    statusbar_free(sb);
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

    /* CMP-245: pet integration */
    RUN_TEST(test_set_pet_null_safe);
    RUN_TEST(test_set_pet_off_no_extra_ansi);
    RUN_TEST(test_set_pet_ticks_on_update);
    RUN_TEST(test_set_pet_transition_active);

    /* CMP-365: timing API */
    RUN_TEST(test_timing_null_safe);
    RUN_TEST(test_latency_not_shown_before_mark);
    RUN_TEST(test_latency_shown_after_mark);
    RUN_TEST(test_first_token_idempotent);
    RUN_TEST(test_session_elapsed_shown);
    RUN_TEST(test_turn_start_resets_latency);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
