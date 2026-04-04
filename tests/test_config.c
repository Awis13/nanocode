/*
 * test_config.c — unit tests for the config system (CMP-141, CMP-187)
 *
 * Tests cover:
 *   - Defaults returned when no file is loaded
 *   - TOML parsing: strings, ints, bools, sections, inline comments
 *   - Unknown / missing keys fall back to compiled-in defaults
 *   - Invalid TOML lines are skipped without crashing
 *   - config_load_path with a temp file
 *   - CMP-187: new config sections (rendering, theme, layout, behavior,
 *     keys, performance) and their defaults
 *   - CMP-187: config_set() -- live mutation
 *   - CMP-187: config_save() -- round-trip persist and reload
 *   - CMP-187: config_cmd_set() -- ":set key value" parsing
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

    ASSERT_STR_EQ(config_get_str(cfg,  "provider.type"),      "claude");
    ASSERT_STR_EQ(config_get_str(cfg,  "provider.base_url"),  "api.anthropic.com");
    ASSERT_EQ    (config_get_int(cfg,  "provider.port"),      0);
    ASSERT_STR_EQ(config_get_str(cfg,  "provider.model"),     "claude-opus-4-6");
    ASSERT_EQ    (config_get_int(cfg,  "provider.timeout_ms"), 30000);
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.enabled"),          1);
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.profile"),         "strict");
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_paths"),   "");
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_commands"),"");
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.network"),          0);
    ASSERT_EQ    (config_get_int(cfg,  "sandbox.max_file_size"),    10485760);
    ASSERT_STR_EQ(config_get_str(cfg,  "ui.theme"),                "dark");
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
    ASSERT_STR_EQ(config_get_str(cfg, "provider.base_url"), "api.anthropic.com");
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
 * CMP-187: new section defaults
 * ====================================================================== */

TEST(test_rendering_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "rendering.target_fps"),     60);
    ASSERT_EQ(config_get_int(cfg, "rendering.frame_batch_ms"), 16);
    ASSERT_STR_EQ(config_get_str(cfg, "rendering.scroll_mode"), "smooth");
    arena_free(a);
}

TEST(test_theme_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "theme.accent_color"),   "cyan");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.diff_add_color"), "green");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.diff_rm_color"),  "red");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.syntax_theme"),   "monokai");
    ASSERT_EQ(config_get_bool(cfg, "theme.true_color"), 1);
    arena_free(a);
}

TEST(test_layout_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "layout.panel_split"),         "horizontal");
    ASSERT_STR_EQ(config_get_str(cfg, "layout.status_bar_position"), "bottom");
    ASSERT_EQ(config_get_int(cfg, "layout.max_width"), 0);
    ASSERT_EQ(config_get_int(cfg, "layout.padding"),   1);
    arena_free(a);
}

TEST(test_behavior_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "behavior.auto_approve_tools"),  0);
    ASSERT_EQ(config_get_bool(cfg, "behavior.confirm_destructive"), 1);
    ASSERT_EQ(config_get_int(cfg,  "behavior.max_context_tokens"),  100000);
    arena_free(a);
}

TEST(test_keys_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "keys.submit"),      "enter");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.cancel"),      "ctrl-c");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.scroll_up"),   "ctrl-u");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.scroll_down"), "ctrl-d");
    arena_free(a);
}

TEST(test_performance_defaults)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "performance.idle_timeout_ms"),  5000);
    ASSERT_EQ(config_get_int(cfg, "performance.max_output_lines"), 10000);
    ASSERT_EQ(config_get_int(cfg, "performance.history_limit_mb"), 10);
    arena_free(a);
}

/* =========================================================================
 * CMP-187: new section TOML parsing
 * ====================================================================== */

TEST(test_rendering_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[rendering]\n"
        "target_fps     = 30\n"
        "frame_batch_ms = 8\n"
        "scroll_mode    = \"jump\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "rendering.target_fps"),     30);
    ASSERT_EQ(config_get_int(cfg, "rendering.frame_batch_ms"), 8);
    ASSERT_STR_EQ(config_get_str(cfg, "rendering.scroll_mode"), "jump");
    arena_free(a);
}

TEST(test_theme_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[theme]\n"
        "accent_color   = \"#ff6600\"\n"
        "diff_add_color = \"blue\"\n"
        "diff_rm_color  = \"magenta\"\n"
        "syntax_theme   = \"solarized\"\n"
        "true_color     = false\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "theme.accent_color"),   "#ff6600");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.diff_add_color"), "blue");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.diff_rm_color"),  "magenta");
    ASSERT_STR_EQ(config_get_str(cfg, "theme.syntax_theme"),   "solarized");
    ASSERT_EQ(config_get_bool(cfg, "theme.true_color"), 0);
    arena_free(a);
}

TEST(test_layout_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[layout]\n"
        "panel_split         = \"vertical\"\n"
        "status_bar_position = \"top\"\n"
        "max_width           = 120\n"
        "padding             = 2\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "layout.panel_split"),         "vertical");
    ASSERT_STR_EQ(config_get_str(cfg, "layout.status_bar_position"), "top");
    ASSERT_EQ(config_get_int(cfg, "layout.max_width"), 120);
    ASSERT_EQ(config_get_int(cfg, "layout.padding"),   2);
    arena_free(a);
}

TEST(test_behavior_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[behavior]\n"
        "auto_approve_tools  = true\n"
        "confirm_destructive = false\n"
        "max_context_tokens  = 200000\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "behavior.auto_approve_tools"),  1);
    ASSERT_EQ(config_get_bool(cfg, "behavior.confirm_destructive"), 0);
    ASSERT_EQ(config_get_int(cfg,  "behavior.max_context_tokens"),  200000);
    arena_free(a);
}

TEST(test_keys_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[keys]\n"
        "submit      = \"ctrl-j\"\n"
        "cancel      = \"escape\"\n"
        "scroll_up   = \"page-up\"\n"
        "scroll_down = \"page-down\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "keys.submit"),      "ctrl-j");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.cancel"),      "escape");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.scroll_up"),   "page-up");
    ASSERT_STR_EQ(config_get_str(cfg, "keys.scroll_down"), "page-down");
    arena_free(a);
}

TEST(test_performance_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[performance]\n"
        "idle_timeout_ms  = 1000\n"
        "max_output_lines = 5000\n"
        "history_limit_mb = 50\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "performance.idle_timeout_ms"),  1000);
    ASSERT_EQ(config_get_int(cfg, "performance.max_output_lines"), 5000);
    ASSERT_EQ(config_get_int(cfg, "performance.history_limit_mb"), 50);
    arena_free(a);
}

/* =========================================================================
 * CMP-187: config_set
 * ====================================================================== */

TEST(test_config_set_new_key)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    int rc = config_set(cfg, "provider.model", "gpt-4o");
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "gpt-4o");
    arena_free(a);
}

TEST(test_config_set_overwrite)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "[rendering]\ntarget_fps = 30\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_int(cfg, "rendering.target_fps"), 30);
    ASSERT_EQ(config_set(cfg, "rendering.target_fps", "120"), 0);
    ASSERT_EQ(config_get_int(cfg, "rendering.target_fps"), 120);
    arena_free(a);
}

TEST(test_config_set_null_safety)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_set(NULL, "k", "v"), -1);
    ASSERT_EQ(config_set(cfg,  NULL, "v"), -1);
    arena_free(a);
}

/* =========================================================================
 * CMP-187: config_save round-trip
 * ====================================================================== */

TEST(test_config_save_round_trip)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[provider]\nmodel = \"ollama/llama3\"\n"
        "[rendering]\ntarget_fps = 30\n"
        "[theme]\ntrue_color = false\n");
    ASSERT_NOT_NULL(cfg);

    char path[] = "/tmp/test_config_save_XXXXXX.toml";
    int fd = mkstemps(path, 5);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    ASSERT_EQ(config_save(cfg, path), 0);

    Arena *a2 = arena_new(1 << 17);
    ASSERT_NOT_NULL(a2);
    Config *cfg2 = config_load_path(a2, path);
    ASSERT_NOT_NULL(cfg2);

    ASSERT_STR_EQ(config_get_str(cfg2, "provider.model"),            "ollama/llama3");
    ASSERT_EQ(config_get_int(cfg2,  "rendering.target_fps"),         30);
    ASSERT_EQ(config_get_bool(cfg2, "theme.true_color"),             0);
    ASSERT_EQ(config_get_bool(cfg2, "behavior.confirm_destructive"), 1);

    unlink(path);
    arena_free(a);
    arena_free(a2);
}

TEST(test_config_save_null_safety)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_save(NULL, "/tmp/x.toml"), -1);
    ASSERT_EQ(config_save(cfg,  NULL),          -1);
    arena_free(a);
}

TEST(test_config_save_custom_key)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_set(cfg, "custom.my_flag", "hello"), 0);

    char path[] = "/tmp/test_config_custom_XXXXXX.toml";
    int fd = mkstemps(path, 5);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    ASSERT_EQ(config_save(cfg, path), 0);

    Arena *a2 = arena_new(1 << 17);
    ASSERT_NOT_NULL(a2);
    Config *cfg2 = config_load_path(a2, path);
    ASSERT_NOT_NULL(cfg2);
    ASSERT_STR_EQ(config_get_str(cfg2, "custom.my_flag"), "hello");

    unlink(path);
    arena_free(a);
    arena_free(a2);
}

/* =========================================================================
 * CMP-187: config_cmd_set
 * ====================================================================== */

TEST(test_cmd_set_colon_prefix)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_cmd_set(cfg, ":set provider.model gpt-4o-mini"), 0);
    ASSERT_STR_EQ(config_get_str(cfg, "provider.model"), "gpt-4o-mini");
    arena_free(a);
}

TEST(test_cmd_set_bare_key_value)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_cmd_set(cfg, "theme.accent_color blue"), 0);
    ASSERT_STR_EQ(config_get_str(cfg, "theme.accent_color"), "blue");
    arena_free(a);
}

TEST(test_cmd_set_overwrite_existing)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a,
        "[behavior]\nauto_approve_tools = true\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_get_bool(cfg, "behavior.auto_approve_tools"), 1);
    ASSERT_EQ(config_cmd_set(cfg, ":set behavior.auto_approve_tools false"), 0);
    ASSERT_EQ(config_get_bool(cfg, "behavior.auto_approve_tools"), 0);
    arena_free(a);
}

TEST(test_cmd_set_malformed)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_cmd_set(cfg, ":set provider.model"), -2);
    ASSERT_EQ(config_cmd_set(cfg, ":set "),               -2);
    ASSERT_EQ(config_cmd_set(cfg, "   "),                 -2);
    arena_free(a);
}

TEST(test_cmd_set_null_safety)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "");
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(config_cmd_set(NULL, ":set k v"), -1);
    ASSERT_EQ(config_cmd_set(cfg,  NULL),       -1);
    arena_free(a);
}

/* =========================================================================
 * CMP-199: sandbox config keys — TOML round-trip
 * ====================================================================== */

TEST(test_sandbox_new_keys_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[sandbox]\n"
        "allowed_paths    = \"/tmp:/var\"\n"
        "allowed_commands = \"ls:cat\"\n"
        "network          = true\n"
        "max_file_size    = 2097152\n");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_paths"),    "/tmp:/var");
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_commands"), "ls:cat");
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.network"),           1);
    ASSERT_EQ    (config_get_int(cfg,  "sandbox.max_file_size"),     2097152);

    arena_free(a);
}

TEST(test_sandbox_new_keys_defaults_when_absent)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    /* Load a config with only non-sandbox keys — new keys must fall back
     * to compiled-in defaults. */
    Config *cfg = load_str(a, "[provider]\nmodel = \"test\"\n");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_paths"),    "");
    ASSERT_STR_EQ(config_get_str(cfg,  "sandbox.allowed_commands"), "");
    ASSERT_EQ    (config_get_bool(cfg, "sandbox.network"),           0);
    ASSERT_EQ    (config_get_int(cfg,  "sandbox.max_file_size"),     10485760);

    arena_free(a);
}

/* =========================================================================
 * CMP-228: provider.type and provider.port TOML round-trip
 * ====================================================================== */

TEST(test_provider_type_ollama_parse)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);

    Config *cfg = load_str(a,
        "[provider]\n"
        "type = \"ollama\"\n"
        "port = 11434\n");
    ASSERT_NOT_NULL(cfg);

    ASSERT_STR_EQ(config_get_str(cfg, "provider.type"), "ollama");
    ASSERT_EQ    (config_get_int(cfg, "provider.port"), 11434);
    arena_free(a);
}

/* =========================================================================
 * CMP-244: pet config — parsing, defaults, set/override, validation
 * ====================================================================== */

TEST(test_pet_config_default)
{
    /* pet key defaults to empty string (auto-pick on first run). */
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "[provider]\nmodel = \"test\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "");
    arena_free(a);
}

TEST(test_pet_config_parse_cat)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"cat\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "cat");
    arena_free(a);
}

TEST(test_pet_config_parse_crab)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"crab\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "crab");
    arena_free(a);
}

TEST(test_pet_config_parse_dog)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"dog\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "dog");
    arena_free(a);
}

TEST(test_pet_config_parse_off)
{
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"off\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "off");
    arena_free(a);
}

TEST(test_pet_config_cli_override)
{
    /* Simulate: config has "dog", CLI --pet crab overrides it. */
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"dog\"\n");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "dog");

    /* Simulate --pet crab: CLI override via config_set. */
    config_set(cfg, "pet", "crab");
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "crab");

    arena_free(a);
}

TEST(test_pet_config_no_pet_override)
{
    /* Simulate --no-pet: sets pet = "off", overriding config value. */
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"cat\"\n");
    ASSERT_NOT_NULL(cfg);

    config_set(cfg, "pet", "off");
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "off");

    arena_free(a);
}

TEST(test_pet_config_unknown_falls_back_to_cat)
{
    /* Unknown value parsed from config; caller validates and falls back. */
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "pet = \"fish\"\n");
    ASSERT_NOT_NULL(cfg);

    const char *val = config_get_str(cfg, "pet");
    /* Simulate main.c validation logic. */
    int valid = (strcmp(val, "cat")  == 0 ||
                 strcmp(val, "crab") == 0 ||
                 strcmp(val, "dog")  == 0 ||
                 strcmp(val, "off")  == 0);
    ASSERT_EQ(valid, 0);  /* "fish" is not a valid pet name */

    /* After fallback: */
    config_set(cfg, "pet", "cat");
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "cat");

    arena_free(a);
}

TEST(test_pet_config_first_run_empty_then_set)
{
    /* First run: empty pet; simulate main.c defaulting to "cat". */
    Arena *a = arena_new(1 << 17);
    ASSERT_NOT_NULL(a);
    Config *cfg = load_str(a, "[provider]\nmodel = \"test\"\n");
    ASSERT_NOT_NULL(cfg);

    const char *val = config_get_str(cfg, "pet");
    ASSERT_STR_EQ(val, "");  /* empty on first run */

    /* Simulate main.c stubbed default. */
    config_set(cfg, "pet", "cat");
    ASSERT_STR_EQ(config_get_str(cfg, "pet"), "cat");

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

    /* CMP-187: new sections -- defaults */
    RUN_TEST(test_rendering_defaults);
    RUN_TEST(test_theme_defaults);
    RUN_TEST(test_layout_defaults);
    RUN_TEST(test_behavior_defaults);
    RUN_TEST(test_keys_defaults);
    RUN_TEST(test_performance_defaults);

    /* CMP-187: new sections -- TOML parsing */
    RUN_TEST(test_rendering_parse);
    RUN_TEST(test_theme_parse);
    RUN_TEST(test_layout_parse);
    RUN_TEST(test_behavior_parse);
    RUN_TEST(test_keys_parse);
    RUN_TEST(test_performance_parse);

    /* CMP-187: config_set */
    RUN_TEST(test_config_set_new_key);
    RUN_TEST(test_config_set_overwrite);
    RUN_TEST(test_config_set_null_safety);

    /* CMP-187: config_save round-trip */
    RUN_TEST(test_config_save_round_trip);
    RUN_TEST(test_config_save_null_safety);
    RUN_TEST(test_config_save_custom_key);

    /* CMP-187: config_cmd_set */
    RUN_TEST(test_cmd_set_colon_prefix);
    RUN_TEST(test_cmd_set_bare_key_value);
    RUN_TEST(test_cmd_set_overwrite_existing);
    RUN_TEST(test_cmd_set_malformed);
    RUN_TEST(test_cmd_set_null_safety);

    /* CMP-199: sandbox new config keys */
    RUN_TEST(test_sandbox_new_keys_parse);
    RUN_TEST(test_sandbox_new_keys_defaults_when_absent);

    /* CMP-228: provider.type and provider.port TOML round-trip */
    /* CMP-228: provider.type and provider.port TOML round-trip */
    RUN_TEST(test_provider_type_ollama_parse);
    /* CMP-244: pet config */
    RUN_TEST(test_pet_config_default);
    RUN_TEST(test_pet_config_parse_cat);
    RUN_TEST(test_pet_config_parse_crab);
    RUN_TEST(test_pet_config_parse_dog);
    RUN_TEST(test_pet_config_parse_off);
    RUN_TEST(test_pet_config_cli_override);
    RUN_TEST(test_pet_config_no_pet_override);
    RUN_TEST(test_pet_config_unknown_falls_back_to_cat);
    RUN_TEST(test_pet_config_first_run_empty_then_set);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
