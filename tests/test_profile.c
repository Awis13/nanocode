/*
 * test_profile.c — unit tests for provider profiles (CMP-382)
 *
 * Tests cover:
 *   - Built-in profile loading by name (claude, gpt, ollama, default)
 *   - Unknown name returns a non-NULL empty/sentinel profile
 *   - Auto-selection by model prefix (profile_for_model)
 *   - Float-to-x1000 temperature / top_p parsing
 *   - User file override (write to temp dir, point XDG_CONFIG_HOME there)
 *   - profile_list() runs without crashing (smoke test)
 */

#include "test.h"
#include "../include/profile.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

static Arena *new_arena(void)
{
    return arena_new(256 * 1024);
}

/* Write a TOML string to <dir>/<name>.toml */
static int write_profile_file(const char *dir, const char *name,
                               const char *toml)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.toml", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(toml, f);
    fclose(f);
    return 0;
}

/* =========================================================================
 * Built-in profiles
 * ====================================================================== */

TEST(test_builtin_claude)
{
    Arena *a = new_arena();
    ASSERT_NOT_NULL(a);

    ProviderProfile *p = profile_load(a, "claude");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "claude");
    ASSERT_STR_EQ(p->tool_format, "native");
    ASSERT_EQ(p->context_window, 200000);
    ASSERT_EQ(p->max_output_tokens, 8192);
    /* temperature = 1.0 → 1000 */
    ASSERT_EQ(p->temperature_x1000, 1000);
    /* top_p = 1.0 → 1000 */
    ASSERT_EQ(p->top_p_x1000, 1000);
    /* no stop sequences */
    ASSERT_TRUE(p->stop_sequences == NULL || p->stop_sequences[0] == '\0');

    arena_free(a);
}

TEST(test_builtin_gpt)
{
    Arena *a = new_arena();
    ASSERT_NOT_NULL(a);

    ProviderProfile *p = profile_load(a, "gpt");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "gpt");
    ASSERT_STR_EQ(p->tool_format, "function_calling");
    ASSERT_EQ(p->context_window, 128000);
    ASSERT_EQ(p->max_output_tokens, 4096);

    arena_free(a);
}

TEST(test_builtin_ollama)
{
    Arena *a = new_arena();
    ASSERT_NOT_NULL(a);

    ProviderProfile *p = profile_load(a, "ollama");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "ollama");
    /* temperature = 0.7 → 700 */
    ASSERT_EQ(p->temperature_x1000, 700);
    /* top_p = 0.9 → 900 */
    ASSERT_EQ(p->top_p_x1000, 900);

    arena_free(a);
}

TEST(test_builtin_default)
{
    Arena *a = new_arena();
    ASSERT_NOT_NULL(a);

    ProviderProfile *p = profile_load(a, "default");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "default");
    /* sentinel values */
    ASSERT_EQ(p->temperature_x1000, -1);
    ASSERT_EQ(p->top_p_x1000, -1);
    ASSERT_EQ(p->context_window, 0);
    ASSERT_EQ(p->max_output_tokens, 0);

    arena_free(a);
}

TEST(test_unknown_profile_nonnull)
{
    Arena *a = new_arena();
    ASSERT_NOT_NULL(a);

    /* Unknown names should not crash and return a non-NULL profile */
    ProviderProfile *p = profile_load(a, "nonexistent_profile_xyz");
    ASSERT_NOT_NULL(p);
    /* Sentinel defaults */
    ASSERT_EQ(p->temperature_x1000, -1);
    ASSERT_EQ(p->top_p_x1000, -1);

    arena_free(a);
}

/* =========================================================================
 * Auto-selection by model name
 * ====================================================================== */

TEST(test_model_prefix_claude)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "claude-opus-4-6");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "claude");
    arena_free(a);
}

TEST(test_model_prefix_claude_sonnet)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "claude-sonnet-4-5");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "claude");
    arena_free(a);
}

TEST(test_model_prefix_gpt)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "gpt-4o");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "gpt");
    arena_free(a);
}

TEST(test_model_prefix_o1)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "o1-preview");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "gpt");
    arena_free(a);
}

TEST(test_model_prefix_o3)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "o3-mini");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "gpt");
    arena_free(a);
}

TEST(test_model_prefix_llama)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "llama3.1");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "ollama");
    arena_free(a);
}

TEST(test_model_prefix_qwen)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "qwen2.5:9b");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "ollama");
    arena_free(a);
}

TEST(test_model_prefix_unknown_defaults)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, "some-unknown-model");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "default");
    arena_free(a);
}

TEST(test_model_null_defaults)
{
    Arena *a = new_arena();
    ProviderProfile *p = profile_for_model(a, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "default");
    arena_free(a);
}

/* =========================================================================
 * User file override
 * ====================================================================== */

TEST(test_user_file_override)
{
    /* Create a temp dir to act as XDG_CONFIG_HOME */
    char tmpdir[] = "/tmp/test_profile_xdg_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) {
        /* Skip on tmpdir failure */
        ASSERT_TRUE(1);
        return;
    }

    /* Create profiles subdirectory */
    char profdir[512];
    snprintf(profdir, sizeof(profdir), "%s/nanocode/profiles", tmpdir);
    mkdir(profdir + 0, 0700); /* /tmp/test_profile_xdg_XXXXXX/nanocode */
    char nanodir[512];
    snprintf(nanodir, sizeof(nanodir), "%s/nanocode", tmpdir);
    mkdir(nanodir, 0700);
    mkdir(profdir, 0700);

    /* Write a custom "claude" profile */
    const char *custom =
        "tool_format       = \"function_calling\"\n"
        "context_window    = 50000\n"
        "thinking_budget   = 5000\n"
        "temperature       = 0.5\n"
        "top_p             = 0.8\n"
        "max_output_tokens = 2048\n";
    write_profile_file(profdir, "claude", custom);

    /* Point XDG_CONFIG_HOME at our temp dir */
    setenv("XDG_CONFIG_HOME", tmpdir, 1);

    Arena *a = new_arena();
    ProviderProfile *p = profile_load(a, "claude");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->tool_format, "function_calling");
    ASSERT_EQ(p->context_window, 50000);
    ASSERT_EQ(p->thinking_budget, 5000);
    ASSERT_EQ(p->temperature_x1000, 500);
    ASSERT_EQ(p->top_p_x1000, 800);
    ASSERT_EQ(p->max_output_tokens, 2048);
    arena_free(a);

    /* Clean up */
    unsetenv("XDG_CONFIG_HOME");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    (void)system(cmd);
}

/* =========================================================================
 * Smoke tests
 * ====================================================================== */

TEST(test_profile_list_smoke)
{
    /* profile_list() should not crash — redirect stdout to /dev/null */
    int saved = dup(STDOUT_FILENO);
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        dup2(fileno(devnull), STDOUT_FILENO);
        fclose(devnull);
    }
    profile_list();
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    ASSERT_TRUE(1); /* just reaching here without crash is a pass */
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    RUN_TEST(test_builtin_claude);
    RUN_TEST(test_builtin_gpt);
    RUN_TEST(test_builtin_ollama);
    RUN_TEST(test_builtin_default);
    RUN_TEST(test_unknown_profile_nonnull);

    RUN_TEST(test_model_prefix_claude);
    RUN_TEST(test_model_prefix_claude_sonnet);
    RUN_TEST(test_model_prefix_gpt);
    RUN_TEST(test_model_prefix_o1);
    RUN_TEST(test_model_prefix_o3);
    RUN_TEST(test_model_prefix_llama);
    RUN_TEST(test_model_prefix_qwen);
    RUN_TEST(test_model_prefix_unknown_defaults);
    RUN_TEST(test_model_null_defaults);

    RUN_TEST(test_user_file_override);
    RUN_TEST(test_profile_list_smoke);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
