/*
 * test_config.c — unit tests for the config system (CMP-141)
 *
 * Tests cover:
 *   - Defaults returned when no file is loaded
 *   - TOML parsing: strings, ints, bools, sections, inline comments
 *   - Unknown / missing keys fall back to compiled-in defaults
 *   - Invalid TOML lines are skipped without crashing
 *   - config_load_path with a temp file
 *   - ASan-clean: no leaks or invalid accesses (run with DEBUG=1)
 */

#include "test.h"
#include "../include/config.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

/*
 * Write TOML text to a temp file and load it.
 * The caller owns the Arena and must free it.
 */
static Config *load_str(Arena *a, const char *toml)
{
    char path[] = "/tmp/test_config_XXXXXX.toml";
    int fd = mkstemps(path, 5);
    if (fd < 0) return NULL;
    write(fd, toml, strlen(toml));
    close(fd);
    Config *cfg = config_load_path(a, path);
    unlink(path);
    return cfg;
}

/* =========================================================================
 * Null / OOM safety
 * ====================================================================== */

TEST(test_null_arena)
{
    ASSERT_NULL(config_load(NULL));
    ASSERT_NULL(config_load_path(NULL, "/tmp/x.toml"));
}

TEST(test_null_path)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    ASSERT_NULL(config_load_path(a, NULL));
    arena_free(a);
}

TEST(test_null_cfg_getters)
{
    /* Getters must not crash and must return sensible values for NULL cfg. */
    ASSERT_STR_EQ(config_get_str(NULL, "provider.model"), "");
    ASSERT_EQ(config_get_int(NULL,  "provider.timeout_ms"), 0);
    ASSERT_EQ(config_get_bool(NULL, "sandbox.enabled"),     0);
}

TEST(test_null_key_getters)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg, NULL), "");
    ASSERT_EQ(config_get_int(cfg,  NULL), 0);
    ASSERT_EQ(config_get_bool(cfg, NULL), 0);

    arena_free(a);
}

/* =========================================================================
 * Default values
 * ====================================================================== */

TEST(test_defaults_when_empty)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    /* Load an empty TOML file — all getters must return compiled-in defaults. */
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg,  "provider.base_url"),  "https://api.anthropic.com");
    ASSERT_STR_EQ(config_get_str(cfg,  "provider.model"),     "claude-opus-4-6");
    ASSERT_EQ    (config_get_int(cfg,  "provider.timeout_ms"), 30000);
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.enabled"),     1);
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.profile"),    "strict");
    ASSERT_STR_EQ(config_get_str(cfg,  "ui.theme"),           "dark");
    ASSERT_EQ    (config_get_bool(cfg, "ui.word_wrap"),        1);
    ASSERT_EQ    (config_get_int(cfg,  "ui.stream_delay_ms"),  0);

    arena_free(a);
}

TEST(test_unknown_key_returns_empty)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);

    /* Unknown key must return "" (not crash). */
    ASSERT_STR_EQ(config_get_str(cfg,  "nonexistent.key"), "");
    ASSERT_EQ    (config_get_int(cfg,  "nonexistent.key"),  0);
    ASSERT_EQ    (config_get_bool(cfg, "nonexistent.key"),  0);

    arena_free(a);
}

/* =========================================================================
 * TOML parsing — strings
 * ====================================================================== */

TEST(test_parse_quoted_string)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "model = \"gpt-4o\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "gpt-4o");

    arena_free(a);
}

TEST(test_parse_unquoted_string)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[ui]\n"
        "theme = light\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "ui.theme"), "light");

    arena_free(a);
}

TEST(test_parse_string_with_spaces_around_equals)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "  base_url   =   \"https://example.com\"  \n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.base_url"), "https://example.com");

    arena_free(a);
}

/* =========================================================================
 * TOML parsing — integers
 * ====================================================================== */

TEST(test_parse_int)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "timeout_ms = 5000\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "provider.timeout_ms"), 5000);

    arena_free(a);
}

TEST(test_parse_int_zero)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[ui]\n"
        "stream_delay_ms = 0\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "ui.stream_delay_ms"), 0);

    arena_free(a);
}

/* =========================================================================
 * TOML parsing — booleans
 * ====================================================================== */

TEST(test_parse_bool_true)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[sandbox]\n"
        "enabled = true\n"
        "[ui]\n"
        "word_wrap = true\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "sandbox.enabled"), 1);
    ASSERT_EQ(config_get_bool(cfg, "ui.word_wrap"),    1);

    arena_free(a);
}

TEST(test_parse_bool_false)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[sandbox]\n"
        "enabled = false\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "sandbox.enabled"), 0);

    arena_free(a);
}

TEST(test_parse_bool_numeric)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[sandbox]\n"
        "enabled = 1\n"
        "[ui]\n"
        "word_wrap = 0\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "sandbox.enabled"), 1);
    ASSERT_EQ(config_get_bool(cfg, "ui.word_wrap"),    0);

    arena_free(a);
}

/* =========================================================================
 * TOML parsing — sections, comments, edge cases
 * ====================================================================== */

TEST(test_parse_multiple_sections)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "model = \"claude-opus-4-6\"\n"
        "timeout_ms = 10000\n"
        "\n"
        "[sandbox]\n"
        "enabled = false\n"
        "profile = \"permissive\"\n"
        "\n"
        "[ui]\n"
        "theme = \"light\"\n");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg,  "provider.model"),    "claude-opus-4-6");
    ASSERT_EQ    (config_get_int(cfg,  "provider.timeout_ms"), 10000);
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.enabled"),      0);
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.profile"),    "permissive");
    ASSERT_STR_EQ(config_get_str(cfg,  "ui.theme"),           "light");

    arena_free(a);
}

TEST(test_parse_inline_comment)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[ui]\n"
        "theme = dark   # this is a comment\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "ui.theme"), "dark");

    arena_free(a);
}

TEST(test_parse_full_line_comment)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "# full-line comment\n"
        "[provider]\n"
        "# another comment\n"
        "model = \"test\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "test");

    arena_free(a);
}

TEST(test_parse_blank_lines_skipped)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "\n"
        "\n"
        "[provider]\n"
        "\n"
        "model = \"abc\"\n"
        "\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "abc");

    arena_free(a);
}

/* =========================================================================
 * Invalid TOML — must warn and skip, never crash
 * ====================================================================== */

TEST(test_invalid_no_equals_skipped)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    /* Lines without '=' are skipped; valid lines still parsed. */
    Config *cfg = load_str(a,
        "[provider]\n"
        "this is not valid toml\n"
        "model = \"ok\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "ok");

    arena_free(a);
}

TEST(test_invalid_malformed_section_skipped)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    /* Malformed section header is skipped; subsequent keys land in previous section. */
    Config *cfg = load_str(a,
        "[provider]\n"
        "model = \"good\"\n"
        "[not closed\n"
        "[ui]\n"
        "theme = \"ok\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "good");
    ASSERT_STR_EQ(config_get_str(cfg, "ui.theme"),       "ok");

    arena_free(a);
}

TEST(test_duplicate_key_last_wins)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "model = \"first\"\n"
        "model = \"second\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "second");

    arena_free(a);
}

/* =========================================================================
 * config_load_path — non-existent file
 * ====================================================================== */

TEST(test_missing_file_returns_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    /* Point at a path that does not exist; must not crash. */
    Config *cfg = config_load_path(a, "/tmp/nanocode_test_nonexistent_config_xyz.toml");
    ASSERT_NOT_NULL(cfg);

    /* Falls back to compiled-in defaults. */
    ASSERT_STR_EQ(config_get_str(cfg, "provider.base_url"), "https://api.anthropic.com");
    ASSERT_EQ    (config_get_int(cfg, "provider.timeout_ms"), 30000);

    /* Clean up the file that config_load_path may have created. */
    unlink("/tmp/nanocode_test_nonexistent_config_xyz.toml");

    arena_free(a);
}

/* =========================================================================
 * system_prompt section
 * ====================================================================== */

TEST(test_system_prompt_append)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[system_prompt]\n"
        "append = \"Always respond in JSON.\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "system_prompt.append"),
                  "Always respond in JSON.");

    arena_free(a);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_config ===\n");

    RUN_TEST(test_null_arena);
    RUN_TEST(test_null_path);
    RUN_TEST(test_null_cfg_getters);
    RUN_TEST(test_null_key_getters);

    RUN_TEST(test_defaults_when_empty);
    RUN_TEST(test_unknown_key_returns_empty);

    RUN_TEST(test_parse_quoted_string);
    RUN_TEST(test_parse_unquoted_string);
    RUN_TEST(test_parse_string_with_spaces_around_equals);

    RUN_TEST(test_parse_int);
    RUN_TEST(test_parse_int_zero);

    RUN_TEST(test_parse_bool_true);
    RUN_TEST(test_parse_bool_false);
    RUN_TEST(test_parse_bool_numeric);

    RUN_TEST(test_parse_multiple_sections);
    RUN_TEST(test_parse_inline_comment);
    RUN_TEST(test_parse_full_line_comment);
    RUN_TEST(test_parse_blank_lines_skipped);

    RUN_TEST(test_invalid_no_equals_skipped);
    RUN_TEST(test_invalid_malformed_section_skipped);
    RUN_TEST(test_duplicate_key_last_wins);

    RUN_TEST(test_missing_file_returns_defaults);
    RUN_TEST(test_system_prompt_append);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
