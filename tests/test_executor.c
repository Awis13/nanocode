/*
 * test_executor.c — unit tests for tool executor framework
 */

#include "test.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Helper handlers
 * ---------------------------------------------------------------------- */

static ToolResult handler_echo(Arena *arena, const char *args_json)
{
    /* Return the args as-is for easy assertion. */
    size_t len  = strlen(args_json);
    char  *copy = arena_alloc(arena, len + 1);
    memcpy(copy, args_json, len + 1);

    ToolResult r;
    r.error   = 0;
    r.content = copy;
    r.len     = len;
    return r;
}

static ToolResult handler_noop(Arena *arena, const char *args_json)
{
    (void)args_json;
    char *msg = arena_alloc(arena, 3);
    msg[0] = 'o'; msg[1] = 'k'; msg[2] = '\0';

    ToolResult r;
    r.error   = 0;
    r.content = msg;
    r.len     = 2;
    return r;
}

static ToolResult handler_fail(Arena *arena, const char *args_json)
{
    (void)args_json;
    char *msg = arena_alloc(arena, 6);
    memcpy(msg, "oops!", 6);

    ToolResult r;
    r.error   = 1;
    r.content = msg;
    r.len     = 5;
    return r;
}

/* -------------------------------------------------------------------------
 * tool_register / tool_invoke basics
 * ---------------------------------------------------------------------- */

TEST(test_register_and_invoke_noop) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("noop", "{}", handler_noop);

    ToolResult r = tool_invoke(a, "noop", "{}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_STR_EQ(r.content, "ok");

    arena_free(a);
}

TEST(test_invoke_unknown_tool_returns_error) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r = tool_invoke(a, "no_such_tool", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    /* Content should mention the tool name. */
    ASSERT_TRUE(strstr(r.content, "no_such_tool") != NULL);

    arena_free(a);
}

TEST(test_invoke_routes_to_correct_handler) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("noop", "{}", handler_noop);
    tool_register("echo", "{}", handler_echo);

    ToolResult r = tool_invoke(a, "echo", "{\"x\":\"hello\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(r.content, "{\"x\":\"hello\"}");

    arena_free(a);
}

TEST(test_invoke_multiple_tools_registered) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("t1", "{}", handler_noop);
    tool_register("t2", "{}", handler_echo);
    tool_register("t3", "{}", handler_fail);

    ToolResult r1 = tool_invoke(a, "t1", "{}");
    ASSERT_EQ(r1.error, 0);
    ASSERT_STR_EQ(r1.content, "ok");

    ToolResult r3 = tool_invoke(a, "t3", "{}");
    ASSERT_EQ(r3.error, 1);

    arena_free(a);
}

TEST(test_invoke_null_args_uses_empty_object) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("echo", "{}", handler_echo);

    /* NULL args_json should be replaced with "{}". */
    ToolResult r = tool_invoke(a, "echo", NULL);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(r.content, "{}");

    arena_free(a);
}

TEST(test_all_allocs_from_arena) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("noop", "{}", handler_noop);

    size_t before = a->used;
    ToolResult r = tool_invoke(a, "noop", "{}");
    size_t after = a->used;

    ASSERT_EQ(r.error, 0);
    /* Some arena memory was consumed for the result. */
    ASSERT_TRUE(after >= before);
    /* Result content pointer is within the arena. */
    ASSERT_TRUE((char *)r.content >= a->base);
    ASSERT_TRUE((char *)r.content < a->base + a->size);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * tool_result_to_json
 * ---------------------------------------------------------------------- */

TEST(test_result_to_json_success) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 0;
    r.content = "hello world";
    r.len     = 11;

    char *json = tool_result_to_json(a, "tu_001", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"tool_result\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"tu_001\"")      != NULL);
    ASSERT_TRUE(strstr(json, "hello world")     != NULL);
    /* No is_error on success. */
    ASSERT_TRUE(strstr(json, "is_error") == NULL);

    arena_free(a);
}

TEST(test_result_to_json_error) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 1;
    r.content = "command failed";
    r.len     = 14;

    char *json = tool_result_to_json(a, "tu_002", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"is_error\":true") != NULL);
    ASSERT_TRUE(strstr(json, "command failed")    != NULL);

    arena_free(a);
}

TEST(test_result_to_json_escapes_quotes) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 0;
    r.content = "say \"hi\"";
    r.len     = 8;

    char *json = tool_result_to_json(a, "tu_003", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\\\"hi\\\"") != NULL);

    arena_free(a);
}

TEST(test_result_to_json_escapes_backslash) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 0;
    r.content = "path\\to\\file";
    r.len     = 12;

    char *json = tool_result_to_json(a, "tu_004", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "path\\\\to\\\\file") != NULL);

    arena_free(a);
}

TEST(test_result_to_json_escapes_newline) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 0;
    r.content = "line1\nline2";
    r.len     = 11;

    char *json = tool_result_to_json(a, "tu_005", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\\n") != NULL);

    arena_free(a);
}

TEST(test_result_to_json_empty_content) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    ToolResult r;
    r.error   = 0;
    r.content = "";
    r.len     = 0;

    char *json = tool_result_to_json(a, "tu_006", &r);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"content\":\"\"") != NULL);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * tool_names_json
 * ---------------------------------------------------------------------- */

TEST(test_tool_names_json_empty) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *json = tool_names_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_STR_EQ(json, "[]");

    arena_free(a);
}

TEST(test_tool_names_json_single_tool) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{\"name\":\"bash\"}", handler_noop);

    char *json = tool_names_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_STR_EQ(json, "[{\"name\":\"bash\"}]");

    arena_free(a);
}

TEST(test_tool_names_json_multiple_tools) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{\"name\":\"bash\"}", handler_noop);
    tool_register("grep", "{\"name\":\"grep\"}", handler_noop);

    char *json = tool_names_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "{\"name\":\"bash\"}") != NULL);
    ASSERT_TRUE(strstr(json, "{\"name\":\"grep\"}") != NULL);
    ASSERT_EQ(json[0], '[');
    ASSERT_EQ(json[strlen(json) - 1], ']');

    arena_free(a);
}

TEST(test_tool_names_json_excludes_tool_search) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{\"name\":\"bash\"}", handler_noop);
    tool_search_register();

    char *json = tool_names_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"bash\"") != NULL);
    ASSERT_TRUE(strstr(json, "tool_search") == NULL);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * tool_schemas_json
 * ---------------------------------------------------------------------- */

TEST(test_tool_schemas_json_empty) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *json = tool_schemas_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_STR_EQ(json, "[]");

    arena_free(a);
}

TEST(test_tool_schemas_json_includes_full_schema) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("mytool",
                  "{\"name\":\"mytool\",\"description\":\"does stuff\"}",
                  handler_noop);

    char *json = tool_schemas_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"does stuff\"") != NULL);
    ASSERT_EQ(json[0], '[');
    ASSERT_EQ(json[strlen(json) - 1], ']');

    arena_free(a);
}

TEST(test_tool_schemas_json_includes_tool_search) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{\"name\":\"bash\"}", handler_noop);
    tool_search_register();

    char *json = tool_schemas_json(a);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "\"tool_search\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"bash\"") != NULL);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * tool_search handler
 * ---------------------------------------------------------------------- */

TEST(test_tool_search_returns_schema) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("mytool",
                  "{\"name\":\"mytool\",\"description\":\"test tool\"}",
                  handler_noop);
    tool_search_register();

    ToolResult r = tool_invoke(a, "tool_search", "{\"name\":\"mytool\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "\"test tool\"") != NULL);

    arena_free(a);
}

TEST(test_tool_search_unknown_name_returns_error) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_search_register();

    ToolResult r = tool_invoke(a, "tool_search", "{\"name\":\"no_such_tool\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "no_such_tool") != NULL);

    arena_free(a);
}

TEST(test_tool_search_missing_name_arg_returns_error) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_search_register();

    ToolResult r = tool_invoke(a, "tool_search", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);

    arena_free(a);
}

TEST(test_tool_search_schema_roundtrip) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    const char *schema = "{\"name\":\"round\",\"description\":\"roundtrip\"}";
    tool_register("round", schema, handler_noop);
    tool_search_register();

    ToolResult r = tool_invoke(a, "tool_search", "{\"name\":\"round\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_STR_EQ(r.content, schema);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Plan mode
 * ---------------------------------------------------------------------- */

TEST(test_plan_mode_blocks_bash) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);

    ToolResult r = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "plan mode") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_plan_mode_blocks_write_file) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("write_file", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);

    ToolResult r = tool_invoke(a, "write_file", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "plan mode") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_plan_mode_blocks_edit_file) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("edit_file", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);

    ToolResult r = tool_invoke(a, "edit_file", "{}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "plan mode") != NULL);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_plan_mode_allows_read_tools) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("read_file", "{}", handler_noop);
    tool_register("grep", "{}", handler_noop);
    tool_register("glob", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);

    ToolResult r1 = tool_invoke(a, "read_file", "{}");
    ASSERT_EQ(r1.error, 0);

    ToolResult r2 = tool_invoke(a, "grep", "{}");
    ASSERT_EQ(r2.error, 0);

    ToolResult r3 = tool_invoke(a, "glob", "{}");
    ASSERT_EQ(r3.error, 0);

    executor_set_mode(EXEC_MODE_NORMAL);
    arena_free(a);
}

TEST(test_plan_mode_toggle_restores_normal) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);

    ToolResult r1 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r1.error, 1);

    executor_set_mode(EXEC_MODE_NORMAL);
    ToolResult r2 = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r2.error, 0);

    arena_free(a);
}

TEST(test_registry_reset_clears_plan_mode) {
    tool_registry_reset();
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    tool_register("bash", "{}", handler_noop);
    executor_set_mode(EXEC_MODE_PLAN);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_PLAN);

    tool_registry_reset();
    tool_register("bash", "{}", handler_noop);
    ASSERT_EQ(executor_get_mode(), EXEC_MODE_NORMAL);

    ToolResult r = tool_invoke(a, "bash", "{}");
    ASSERT_EQ(r.error, 0);

    arena_free(a);
}

int main(void)
{
    fprintf(stderr, "=== test_executor ===\n");

    RUN_TEST(test_register_and_invoke_noop);
    RUN_TEST(test_invoke_unknown_tool_returns_error);
    RUN_TEST(test_invoke_routes_to_correct_handler);
    RUN_TEST(test_invoke_multiple_tools_registered);
    RUN_TEST(test_invoke_null_args_uses_empty_object);
    RUN_TEST(test_all_allocs_from_arena);

    RUN_TEST(test_result_to_json_success);
    RUN_TEST(test_result_to_json_error);
    RUN_TEST(test_result_to_json_escapes_quotes);
    RUN_TEST(test_result_to_json_escapes_backslash);
    RUN_TEST(test_result_to_json_escapes_newline);
    RUN_TEST(test_result_to_json_empty_content);

    RUN_TEST(test_tool_names_json_empty);
    RUN_TEST(test_tool_names_json_single_tool);
    RUN_TEST(test_tool_names_json_multiple_tools);
    RUN_TEST(test_tool_names_json_excludes_tool_search);

    RUN_TEST(test_tool_schemas_json_empty);
    RUN_TEST(test_tool_schemas_json_includes_full_schema);
    RUN_TEST(test_tool_schemas_json_includes_tool_search);

    RUN_TEST(test_tool_search_returns_schema);
    RUN_TEST(test_tool_search_unknown_name_returns_error);
    RUN_TEST(test_tool_search_missing_name_arg_returns_error);
    RUN_TEST(test_tool_search_schema_roundtrip);

    RUN_TEST(test_plan_mode_blocks_bash);
    RUN_TEST(test_plan_mode_blocks_write_file);
    RUN_TEST(test_plan_mode_blocks_edit_file);
    RUN_TEST(test_plan_mode_allows_read_tools);
    RUN_TEST(test_plan_mode_toggle_restores_normal);
    RUN_TEST(test_registry_reset_clears_plan_mode);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
