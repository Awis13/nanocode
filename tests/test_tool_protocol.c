/*
 * test_tool_protocol.c — unit tests for CMP-119 tool protocol
 *
 * Tests:
 *   - Parse Claude tool_use response block
 *   - Parse OpenAI function_call response
 *   - Parse response with no tool calls (0 returned)
 *   - Parse multiple tool calls in one response
 *   - OpenAI arguments string unescaping
 *   - tool_dispatch_all dispatches to executor and adds conversation turns
 *   - Loop terminates when there are no tool calls (dispatch returns 0)
 *   - tool_build_schema_payload returns valid JSON for Claude
 *   - tool_build_schema_payload wraps schemas for OpenAI
 */

#include "test.h"
#include "../src/agent/tool_protocol.h"
#include "../src/agent/conversation.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Test fixtures
 * ---------------------------------------------------------------------- */

/* Minimal Claude completed-response JSON with one tool_use block. */
static const char *CLAUDE_ONE_TOOL =
    "{"
    "\"id\":\"msg_01\","
    "\"type\":\"message\","
    "\"role\":\"assistant\","
    "\"content\":["
      "{\"type\":\"text\",\"text\":\"Running bash\"},"
      "{\"type\":\"tool_use\","
       "\"id\":\"toolu_01abc\","
       "\"name\":\"bash\","
       "\"input\":{\"cmd\":\"ls -la\"}}"
    "],"
    "\"stop_reason\":\"tool_use\""
    "}";

/* Claude response with two tool_use blocks. */
static const char *CLAUDE_TWO_TOOLS =
    "{"
    "\"content\":["
      "{\"type\":\"tool_use\","
       "\"id\":\"t1\","
       "\"name\":\"bash\","
       "\"input\":{\"cmd\":\"pwd\"}},"
      "{\"type\":\"tool_use\","
       "\"id\":\"t2\","
       "\"name\":\"read_file\","
       "\"input\":{\"path\":\"/etc/hosts\"}}"
    "],"
    "\"stop_reason\":\"tool_use\""
    "}";

/* Claude response with no tool_use blocks (text only). */
static const char *CLAUDE_NO_TOOLS =
    "{"
    "\"content\":["
      "{\"type\":\"text\",\"text\":\"Hello!\"}"
    "],"
    "\"stop_reason\":\"end_turn\""
    "}";

/* OpenAI function_call response with one tool call. */
static const char *OPENAI_ONE_TOOL =
    "{"
    "\"choices\":[{"
      "\"message\":{"
        "\"role\":\"assistant\","
        "\"tool_calls\":[{"
          "\"id\":\"call_xyz\","
          "\"type\":\"function\","
          "\"function\":{"
            "\"name\":\"bash\","
            "\"arguments\":\"{\\\"cmd\\\":\\\"echo hello\\\"}\""
          "}"
        "}]"
      "},"
      "\"finish_reason\":\"tool_calls\""
    "}]"
    "}";

/* OpenAI response with no tool_calls. */
static const char *OPENAI_NO_TOOLS =
    "{"
    "\"choices\":[{"
      "\"message\":{"
        "\"role\":\"assistant\","
        "\"content\":\"Hi!\""
      "},"
      "\"finish_reason\":\"stop\""
    "}]"
    "}";

/* -------------------------------------------------------------------------
 * Dummy tool handler for executor dispatch tests
 * ---------------------------------------------------------------------- */

static ToolResult dummy_tool_handler(Arena *arena, const char *args_json)
{
    (void)args_json;
    static const char msg[] = "\"ok\"";
    char *buf = arena_alloc(arena, sizeof(msg));
    if (buf) memcpy(buf, msg, sizeof(msg));
    ToolResult r = { .error = 0, .content = buf, .len = sizeof(msg) - 1 };
    return r;
}

/* =========================================================================
 * Parse tests — Claude
 * ====================================================================== */

TEST(test_parse_claude_one_tool)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_CLAUDE,
                                CLAUDE_ONE_TOOL, strlen(CLAUDE_ONE_TOOL),
                                a, &calls);
    ASSERT_EQ(n, 1);
    ASSERT_NOT_NULL(calls);
    ASSERT_STR_EQ(calls[0].id,    "toolu_01abc");
    ASSERT_STR_EQ(calls[0].name,  "bash");
    ASSERT_NOT_NULL(calls[0].input);
    /* input should contain the JSON object — verify it starts with '{' */
    ASSERT_TRUE(calls[0].input[0] == '{');

    arena_free(a);
}

TEST(test_parse_claude_two_tools)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_CLAUDE,
                                CLAUDE_TWO_TOOLS, strlen(CLAUDE_TWO_TOOLS),
                                a, &calls);
    ASSERT_EQ(n, 2);
    ASSERT_NOT_NULL(calls);
    ASSERT_STR_EQ(calls[0].id,   "t1");
    ASSERT_STR_EQ(calls[0].name, "bash");
    ASSERT_STR_EQ(calls[1].id,   "t2");
    ASSERT_STR_EQ(calls[1].name, "read_file");

    arena_free(a);
}

TEST(test_parse_claude_no_tools)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_CLAUDE,
                                CLAUDE_NO_TOOLS, strlen(CLAUDE_NO_TOOLS),
                                a, &calls);
    ASSERT_EQ(n, 0);

    arena_free(a);
}

TEST(test_parse_claude_malformed)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_CLAUDE, "not json", 8, a, &calls);
    ASSERT_EQ(n, -1);

    arena_free(a);
}

/* =========================================================================
 * Parse tests — OpenAI
 * ====================================================================== */

TEST(test_parse_openai_one_tool)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_OPENAI,
                                OPENAI_ONE_TOOL, strlen(OPENAI_ONE_TOOL),
                                a, &calls);
    ASSERT_EQ(n, 1);
    ASSERT_NOT_NULL(calls);
    ASSERT_STR_EQ(calls[0].id,   "call_xyz");
    ASSERT_STR_EQ(calls[0].name, "bash");
    /* arguments were JSON-string encoded — after unescape: {"cmd":"echo hello"} */
    ASSERT_NOT_NULL(calls[0].input);
    ASSERT_TRUE(strstr(calls[0].input, "echo hello") != NULL);

    arena_free(a);
}

TEST(test_parse_openai_no_tools)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_OPENAI,
                                OPENAI_NO_TOOLS, strlen(OPENAI_NO_TOOLS),
                                a, &calls);
    ASSERT_EQ(n, 0);

    arena_free(a);
}

/* Ollama uses the same wire format as OpenAI for tool_calls. */
TEST(test_parse_ollama_same_as_openai)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_OLLAMA,
                                OPENAI_ONE_TOOL, strlen(OPENAI_ONE_TOOL),
                                a, &calls);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(calls[0].name, "bash");

    arena_free(a);
}

/* =========================================================================
 * Dispatch tests
 * ====================================================================== */

TEST(test_dispatch_all_adds_turns)
{
    tool_registry_reset();
    tool_register("bash", "{}", dummy_tool_handler);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    /* Seed with a user turn. */
    conv_add(conv, "user", "please run ls");
    ASSERT_EQ(conv->nturn, 1);

    ToolCall calls[1];
    calls[0].id    = "tu_001";
    calls[0].name  = "bash";
    calls[0].input = "{\"cmd\":\"ls\"}";

    int dispatched = tool_dispatch_all(calls, 1, conv, a);
    ASSERT_EQ(dispatched, 1);

    /* Should have added tool_use (assistant) + tool_result (user) turns. */
    ASSERT_EQ(conv->nturn, 3);
    ASSERT_STR_EQ(conv->turns[1].role, "assistant");
    ASSERT_EQ(conv->turns[1].is_tool, 1);
    ASSERT_STR_EQ(conv->turns[2].role, "user");
    ASSERT_EQ(conv->turns[2].is_tool, 1);

    arena_free(a);
    tool_registry_reset();
}

TEST(test_dispatch_loop_terminates_on_no_tool_calls)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    /* Simulate parsing a text-only response (0 tool calls). */
    ToolCall *calls = NULL;
    int n = tool_parse_response(PROVIDER_CLAUDE,
                                CLAUDE_NO_TOOLS, strlen(CLAUDE_NO_TOOLS),
                                a, &calls);
    ASSERT_EQ(n, 0);

    /* Dispatch returns 0 — loop should terminate. */
    int dispatched = tool_dispatch_all(calls, n, conv, a);
    ASSERT_EQ(dispatched, 0);

    arena_free(a);
}

TEST(test_dispatch_null_safety)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    /* All-null / zero-count combinations must not crash. */
    ASSERT_EQ(tool_dispatch_all(NULL, 0, conv, a), 0);
    ASSERT_EQ(tool_dispatch_all(NULL, 5, conv, a), 0);

    arena_free(a);
}

/* =========================================================================
 * Schema payload tests
 * ====================================================================== */

TEST(test_build_schema_claude_returns_array)
{
    tool_registry_reset();
    tool_register("bash", "{\"name\":\"bash\",\"description\":\"run shell\","
                          "\"input_schema\":{\"type\":\"object\"}}",
                  dummy_tool_handler);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *payload = tool_build_schema_payload(PROVIDER_CLAUDE, a);
    ASSERT_NOT_NULL(payload);
    /* Should be a JSON array. */
    ASSERT_TRUE(payload[0] == '[');

    arena_free(a);
    tool_registry_reset();
}

TEST(test_build_schema_openai_wraps_function)
{
    tool_registry_reset();
    tool_register("bash", "{\"name\":\"bash\",\"description\":\"run shell\","
                          "\"input_schema\":{\"type\":\"object\"}}",
                  dummy_tool_handler);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *payload = tool_build_schema_payload(PROVIDER_OPENAI, a);
    ASSERT_NOT_NULL(payload);
    ASSERT_TRUE(payload[0] == '[');
    /* Should contain the "function" wrapper. */
    ASSERT_TRUE(strstr(payload, "\"type\":\"function\"") != NULL);
    ASSERT_TRUE(strstr(payload, "\"function\"") != NULL);

    arena_free(a);
    tool_registry_reset();
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    /* Parse — Claude */
    RUN_TEST(test_parse_claude_one_tool);
    RUN_TEST(test_parse_claude_two_tools);
    RUN_TEST(test_parse_claude_no_tools);
    RUN_TEST(test_parse_claude_malformed);

    /* Parse — OpenAI / Ollama */
    RUN_TEST(test_parse_openai_one_tool);
    RUN_TEST(test_parse_openai_no_tools);
    RUN_TEST(test_parse_ollama_same_as_openai);

    /* Dispatch */
    RUN_TEST(test_dispatch_all_adds_turns);
    RUN_TEST(test_dispatch_loop_terminates_on_no_tool_calls);
    RUN_TEST(test_dispatch_null_safety);

    /* Schema payload */
    RUN_TEST(test_build_schema_claude_returns_array);
    RUN_TEST(test_build_schema_openai_wraps_function);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
