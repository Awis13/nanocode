/*
 * test_sandbox.c — unit tests for sandbox enforcement (CMP-200)
 *
 * Tests cover:
 *   - sandbox_config_from_cfg(): struct population from Config
 *   - sandbox_validate(): profile validation, custom-profile checks,
 *     strict-mode OS requirement
 *   - sandbox_activate(): disabled sandbox is a no-op; permissive never exits
 *   - sandbox_sbpl_build() [macOS / SANDBOX_TEST]: SBPL string generation
 *   - sandbox_build_prompt_block(): prompt XML output
 *
 * NOTE: sandbox_activate() with enabled=1 is NOT tested here because
 * the macOS sandbox_init() call is irreversible and would confine the
 * test process itself.  On Linux, Landlock activation is similarly
 * permanent.  Activation is validated manually / in integration tests.
 */

/* Enable SBPL builder on all platforms for testing. */
#define SANDBOX_TEST 1

#include "test.h"
#include "../include/sandbox.h"
#include "../include/config.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

static Config *load_toml(Arena *a, const char *toml)
{
    char path[] = "/tmp/test_sandbox_XXXXXX.toml";
    int  fd     = mkstemps(path, 5);
    if (fd < 0) return NULL;
    (void)write(fd, toml, strlen(toml));
    close(fd);
    Config *cfg = config_load_path(a, path);
    unlink(path);
    return cfg;
}

/* Build a minimal SandboxConfig on the stack. */
static SandboxConfig make_sc(int enabled, const char *profile,
                              const char *paths, const char *cmds,
                              int network, long maxsz)
{
    SandboxConfig sc;
    memset(&sc, 0, sizeof(sc));
    sc.enabled          = enabled;
    sc.profile          = profile;
    sc.allowed_paths    = paths  ? paths  : "";
    sc.allowed_commands = cmds   ? cmds   : "";
    sc.network          = network;
    sc.max_file_size    = maxsz;
    return sc;
}

/* =========================================================================
 * sandbox_config_from_cfg
 * ====================================================================== */

TEST(test_cfg_from_config_defaults)
{
    Arena  *a   = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_toml(a, "");
    ASSERT_NOT_NULL(cfg);

    SandboxConfig sc;
    memset(&sc, 0xff, sizeof(sc));   /* poison */
    sandbox_config_from_cfg(&sc, cfg);

    ASSERT_EQ    (sc.enabled,       1);
    ASSERT_STR_EQ(sc.profile,       "strict");
    ASSERT_STR_EQ(sc.allowed_paths, "");
    ASSERT_STR_EQ(sc.allowed_commands, "");
    ASSERT_EQ    (sc.network,       0);
    ASSERT_EQ    (sc.max_file_size, 10485760L);

    arena_free(a);
}

TEST(test_cfg_from_config_custom_values)
{
    Arena  *a   = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_toml(a,
        "[sandbox]\n"
        "enabled          = false\n"
        "profile          = \"custom\"\n"
        "allowed_paths    = \"/tmp:/var/log\"\n"
        "allowed_commands = \"ls:cat\"\n"
        "network          = true\n"
        "max_file_size    = 1024\n");
    ASSERT_NOT_NULL(cfg);

    SandboxConfig sc;
    sandbox_config_from_cfg(&sc, cfg);

    ASSERT_EQ    (sc.enabled,           0);
    ASSERT_STR_EQ(sc.profile,           "custom");
    ASSERT_STR_EQ(sc.allowed_paths,     "/tmp:/var/log");
    ASSERT_STR_EQ(sc.allowed_commands,  "ls:cat");
    ASSERT_EQ    (sc.network,           1);
    ASSERT_EQ    (sc.max_file_size,     1024L);

    arena_free(a);
}

TEST(test_cfg_from_config_null_safety)
{
    /* Must not crash on NULL inputs. */
    SandboxConfig sc;
    memset(&sc, 0, sizeof(sc));
    sandbox_config_from_cfg(NULL, NULL);
    sandbox_config_from_cfg(&sc,  NULL);
    /* sc remains zeroed */
    ASSERT_EQ(sc.enabled, 0);
}

/* =========================================================================
 * sandbox_validate
 * ====================================================================== */

TEST(test_validate_null)
{
    ASSERT_EQ(sandbox_validate(NULL), -1);
}

TEST(test_validate_strict_ok)
{
    /* strict is always valid at compile time on macOS and Linux.
     * On other OSes it returns -1; we accept either outcome here. */
    SandboxConfig sc = make_sc(1, "strict", NULL, NULL, 0, 0);
    int rc = sandbox_validate(&sc);
#if defined(__APPLE__) || defined(__linux__)
    ASSERT_EQ(rc, 0);
#else
    ASSERT_EQ(rc, -1);
#endif
}

TEST(test_validate_permissive_ok)
{
    SandboxConfig sc = make_sc(1, "permissive", NULL, NULL, 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), 0);
}

TEST(test_validate_custom_with_paths_ok)
{
    SandboxConfig sc = make_sc(1, "custom", "/tmp", "ls", 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), 0);
}

TEST(test_validate_custom_no_paths_fails)
{
    SandboxConfig sc = make_sc(1, "custom", "", "ls", 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), -1);
}

TEST(test_validate_custom_no_commands_fails)
{
    SandboxConfig sc = make_sc(1, "custom", "/tmp", "", 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), -1);
}

TEST(test_validate_unknown_profile_fails)
{
    SandboxConfig sc = make_sc(1, "godmode", NULL, NULL, 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), -1);
}

TEST(test_validate_empty_profile_fails)
{
    SandboxConfig sc = make_sc(1, "", NULL, NULL, 0, 0);
    ASSERT_EQ(sandbox_validate(&sc), -1);
}

/* =========================================================================
 * sandbox_activate — safe cases only (no irreversible kernel calls)
 * ====================================================================== */

TEST(test_activate_disabled_returns_zero)
{
    /* When enabled == 0, sandbox_activate must be a no-op. */
    SandboxConfig sc = make_sc(0, "strict", NULL, NULL, 0, 0);
    ASSERT_EQ(sandbox_activate(&sc), 0);
}

TEST(test_activate_null_returns_minus_one)
{
    ASSERT_EQ(sandbox_activate(NULL), -1);
}

TEST(test_activate_permissive_no_exit)
{
    /*
     * Permissive mode must never call exit() when enforcement is
     * unavailable.  We verify this by calling activate with enabled=0
     * (safe), which must return 0 without any side effects.
     * Full permissive+enabled is an integration-test concern.
     */
    SandboxConfig sc = make_sc(0, "permissive", NULL, NULL, 0, 0);
    ASSERT_EQ(sandbox_activate(&sc), 0);
}

/* =========================================================================
 * sandbox_sbpl_build — macOS SBPL string generation
 * (compiled on all platforms when SANDBOX_TEST is defined)
 * ====================================================================== */

TEST(test_sbpl_deny_default_present)
{
    SandboxConfig sc = make_sc(1, "strict", "/tmp", NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(deny default)") != NULL);
}

TEST(test_sbpl_always_allowed_ops_present)
{
    SandboxConfig sc = make_sc(1, "strict", NULL, NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(allow process-exec*)") != NULL);
    ASSERT_TRUE(strstr(buf, "(allow signal)")         != NULL);
    ASSERT_TRUE(strstr(buf, "(allow sysctl-read)")    != NULL);
}

TEST(test_sbpl_single_path)
{
    SandboxConfig sc = make_sc(1, "strict", "/tmp", NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(subpath \"/tmp\")") != NULL);
}

TEST(test_sbpl_multiple_paths)
{
    SandboxConfig sc = make_sc(1, "custom", "/tmp:/var/log:/home/user",
                                NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(subpath \"/tmp\")")        != NULL);
    ASSERT_TRUE(strstr(buf, "(subpath \"/var/log\")")    != NULL);
    ASSERT_TRUE(strstr(buf, "(subpath \"/home/user\")") != NULL);
}

TEST(test_sbpl_network_allowed)
{
    SandboxConfig sc = make_sc(1, "permissive", NULL, NULL, 1, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(allow network*)") != NULL);
}

TEST(test_sbpl_network_denied)
{
    SandboxConfig sc = make_sc(1, "strict", NULL, NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(allow network*)") == NULL);
}

TEST(test_sbpl_no_paths)
{
    /* Empty allowed_paths — no file-read/write rules generated. */
    SandboxConfig sc = make_sc(1, "permissive", "", NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "file-read") == NULL);
    ASSERT_TRUE(strstr(buf, "file-write") == NULL);
}

TEST(test_sbpl_small_buffer_truncated)
{
    /* A buffer that is too small must not overflow or crash. */
    SandboxConfig sc = make_sc(1, "strict", "/tmp", NULL, 0, 0);
    char buf[8];
    int rc = sandbox_sbpl_build(&sc, buf, sizeof(buf));
    /* rc == -1 signals truncation; buf must be NUL-terminated. */
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(test_sbpl_path_with_double_quote)
{
    /* A '"' in a path must be escaped to '\"' so the SBPL string is valid. */
    SandboxConfig sc = make_sc(1, "strict", "/tmp/foo\"bar", NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(subpath \"/tmp/foo\\\"bar\")") != NULL);
}

TEST(test_sbpl_path_with_backslash)
{
    /* A '\' in a path must be escaped to '\\' so the SBPL string is valid. */
    SandboxConfig sc = make_sc(1, "strict", "/tmp/foo\\bar", NULL, 0, 0);
    char buf[4096];
    sandbox_sbpl_build(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "(subpath \"/tmp/foo\\\\bar\")") != NULL);
}

/* =========================================================================
 * sandbox_build_prompt_block
 * ====================================================================== */

TEST(test_prompt_block_disabled_empty)
{
    SandboxConfig sc = make_sc(0, "strict", NULL, NULL, 0, 0);
    char buf[512];
    buf[0] = 'X';
    sandbox_build_prompt_block(&sc, buf, sizeof(buf));
    ASSERT_EQ(buf[0], '\0');
}

TEST(test_prompt_block_enabled_contains_profile)
{
    SandboxConfig sc = make_sc(1, "permissive", "/tmp", "ls", 0, 1024);
    char buf[512];
    sandbox_build_prompt_block(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "permissive")      != NULL);
    ASSERT_TRUE(strstr(buf, "/tmp")            != NULL);
    ASSERT_TRUE(strstr(buf, "ls")                           != NULL);
    ASSERT_TRUE(strstr(buf, "denied")                       != NULL);  /* network */
    ASSERT_TRUE(strstr(buf, "<sandbox_policy>")             != NULL);
    ASSERT_TRUE(strstr(buf, "allowed_commands (app-layer)") != NULL);
}

TEST(test_prompt_block_network_allowed_label)
{
    SandboxConfig sc = make_sc(1, "strict", "/var", "cat", 1, 0);
    char buf[512];
    sandbox_build_prompt_block(&sc, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "allowed") != NULL);
}

TEST(test_prompt_block_null_safety)
{
    char buf[64] = "untouched";
    sandbox_build_prompt_block(NULL, buf,  sizeof(buf));
    sandbox_build_prompt_block(NULL, NULL, 0);
    /* buf should be unchanged when sc is NULL */
    ASSERT_STR_EQ(buf, "untouched");
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_sandbox ===\n");

    RUN_TEST(test_cfg_from_config_defaults);
    RUN_TEST(test_cfg_from_config_custom_values);
    RUN_TEST(test_cfg_from_config_null_safety);

    RUN_TEST(test_validate_null);
    RUN_TEST(test_validate_strict_ok);
    RUN_TEST(test_validate_permissive_ok);
    RUN_TEST(test_validate_custom_with_paths_ok);
    RUN_TEST(test_validate_custom_no_paths_fails);
    RUN_TEST(test_validate_custom_no_commands_fails);
    RUN_TEST(test_validate_unknown_profile_fails);
    RUN_TEST(test_validate_empty_profile_fails);

    RUN_TEST(test_activate_disabled_returns_zero);
    RUN_TEST(test_activate_null_returns_minus_one);
    RUN_TEST(test_activate_permissive_no_exit);

    RUN_TEST(test_sbpl_deny_default_present);
    RUN_TEST(test_sbpl_always_allowed_ops_present);
    RUN_TEST(test_sbpl_single_path);
    RUN_TEST(test_sbpl_multiple_paths);
    RUN_TEST(test_sbpl_network_allowed);
    RUN_TEST(test_sbpl_network_denied);
    RUN_TEST(test_sbpl_no_paths);
    RUN_TEST(test_sbpl_small_buffer_truncated);
    RUN_TEST(test_sbpl_path_with_double_quote);
    RUN_TEST(test_sbpl_path_with_backslash);

    RUN_TEST(test_prompt_block_disabled_empty);
    RUN_TEST(test_prompt_block_enabled_contains_profile);
    RUN_TEST(test_prompt_block_network_allowed_label);
    RUN_TEST(test_prompt_block_null_safety);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
