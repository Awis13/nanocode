/*
 * test_dryrun.c — unit tests for --dry-run and --readonly execution modes
 *
 * CMP-226: verify that EXEC_MODE_DRY_RUN and EXEC_MODE_READONLY behave
 * correctly in the tool executor.
 */

#include "test.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Helper handlers
 * ---------------------------------------------------------------------- */

static ToolResult handler_noop(Arena *arena, const char *args_json)
{
    (void)args_json;
    char *msg = arena_alloc(arena, 3);
    msg[0] = 'o'; msg[1] = 'k'; msg[2] = '\0';
    ToolResult r = { .error = 0, .content = msg, .len = 2 };
    return r;
}

/* -------------------------------------------------------------------------
 * Dry-run mode tests
 * ---------------------------------------------------------------------- */

TEST(test_dryrun_returns_synthetic_result) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop, TOOL_SAFE_MUTATING);
    tool_register("write_file", "{}", handler_noop, TOOL_SAFE_MUTATING);
    tool_register("read_file", "{}", handler_noop, TOOL_SAFE_READONLY);
    executor_set_mode(EXEC_MODE_DRY_RUN);

    ToolResult r = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "dry_run") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_dryrun_does_not_call_handler) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    /* handler_noop returns "ok"; dry-run should return {"dry_run":true} */
    tool_register("write_file", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_DRY_RUN);

    ToolResult r = tool_invoke(a, "write_file", "{}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    /* Must NOT be the real handler output "ok" */
    ASSERT_TRUE(strcmp(r.content, "ok") != 0);
    ASSERT_TRUE(strstr(r.content, "dry_run") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_dryrun_applies_to_all_tools) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("read_file", "{}", handler_noop, TOOL_SAFE_READONLY);
    tool_register("grep", "{}", handler_noop, TOOL_SAFE_READONLY);
    tool_register("glob", "{}", handler_noop, TOOL_SAFE_READONLY);
    executor_set_mode(EXEC_MODE_DRY_RUN);

    ToolResult r1 = tool_invoke(a, "read_file", "{}");
    ASSERT_EQ(r1.error, 0);
    ASSERT_TRUE(strstr(r1.content, "dry_run") != NULL);

    ToolResult r2 = tool_invoke(a, "grep", "{}");
    ASSERT_EQ(r2.error, 0);
    ASSERT_TRUE(strstr(r2.content, "dry_run") != NULL);

    ToolResult r3 = tool_invoke(a, "glob", "{}");
    ASSERT_EQ(r3.error, 0);
    ASSERT_TRUE(strstr(r3.content, "dry_run") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_dryrun_restores_to_normal) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_DRY_RUN);

    ToolResult r1 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r1.error, 0);
    ASSERT_TRUE(strstr(r1.content, "dry_run") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    ToolResult r2 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r2.error, 0);
    ASSERT_STR_EQ(r2.content, "ok");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Readonly mode tests
 * ---------------------------------------------------------------------- */

TEST(test_readonly_blocks_bash) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "readonly") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_readonly_blocks_write_file) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("write_file", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r = tool_invoke(a, "write_file", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "readonly") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_readonly_blocks_edit_file) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("edit_file", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r = tool_invoke(a, "edit_file", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "readonly") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_readonly_allows_read_file) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("read_file", "{}", handler_noop, TOOL_SAFE_READONLY);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r = tool_invoke(a, "read_file", "{}");
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(r.content, "ok");

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_readonly_allows_grep_and_glob) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("grep", "{}", handler_noop, TOOL_SAFE_READONLY);
    tool_register("glob", "{}", handler_noop, TOOL_SAFE_READONLY);
    tool_register("webfetch", "{}", handler_noop, TOOL_SAFE_READONLY);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r1 = tool_invoke(a, "grep", "{}");
    ASSERT_EQ(r1.error, 0);

    ToolResult r2 = tool_invoke(a, "glob", "{}");
    ASSERT_EQ(r2.error, 0);

    ToolResult r3 = tool_invoke(a, "webfetch", "{}");
    ASSERT_EQ(r3.error, 0);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_readonly_restores_to_normal) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop, TOOL_SAFE_MUTATING);
    executor_set_mode(EXEC_MODE_READONLY);

    ToolResult r1 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r1.error, 1);

    executor_set_mode(EXEC_MODE_NORMAL);
    ToolResult r2 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r2.error, 0);
    ASSERT_STR_EQ(r2.content, "ok");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Mode accessor tests
 * ---------------------------------------------------------------------- */

TEST(test_exec_mode_get_set) {
    tool_registry_reset();

    ASSERT_EQ(executor_get_mode(), EXEC_MODE_NORMAL);

    executor_set_mode(EXEC_MODE_DRY_RUN);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_DRY_RUN);

    executor_set_mode(EXEC_MODE_READONLY);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_READONLY);

    executor_set_mode(EXEC_MODE_NORMAL);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_NORMAL);
}

TEST(test_registry_reset_clears_exec_mode) {
    tool_registry_reset();

    executor_set_mode(EXEC_MODE_DRY_RUN);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_DRY_RUN);

    tool_registry_reset();
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_NORMAL);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_dryrun ===\n");

    RUN_TEST(test_dryrun_returns_synthetic_result);
    RUN_TEST(test_dryrun_does_not_call_handler);
    RUN_TEST(test_dryrun_applies_to_all_tools);
    RUN_TEST(test_dryrun_restores_to_normal);

    RUN_TEST(test_readonly_blocks_bash);
    RUN_TEST(test_readonly_blocks_write_file);
    RUN_TEST(test_readonly_blocks_edit_file);
    RUN_TEST(test_readonly_allows_read_file);
    RUN_TEST(test_readonly_allows_grep_and_glob);
    RUN_TEST(test_readonly_restores_to_normal);

    RUN_TEST(test_exec_mode_get_set);
    RUN_TEST(test_registry_reset_clears_exec_mode);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
