/*
 * test_json.c — unit tests for jsmn-based JSON helpers
 */

#include "test.h"
#include "../src/util/json.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * json_parse_ctx
 * ---------------------------------------------------------------------- */

TEST(test_parse_valid_object) {
    const char *json = "{\"key\":\"value\"}";
    JsonCtx ctx;
    int r = json_parse_ctx(&ctx, json, strlen(json));
    ASSERT_TRUE(r > 0);
    ASSERT_TRUE(ctx.ntok > 0);
}

TEST(test_parse_empty_object) {
    const char *json = "{}";
    JsonCtx ctx;
    int r = json_parse_ctx(&ctx, json, strlen(json));
    ASSERT_TRUE(r > 0);
}

TEST(test_parse_invalid_json) {
    const char *json = "{broken";
    JsonCtx ctx;
    int r = json_parse_ctx(&ctx, json, strlen(json));
    ASSERT_EQ(r, -1);
}

TEST(test_parse_empty_string) {
    const char *json = "";
    JsonCtx ctx;
    int r = json_parse_ctx(&ctx, json, 0);
    ASSERT_TRUE(r <= 0); /* either -1 or 0 is acceptable */
}

/* -------------------------------------------------------------------------
 * json_get_str
 * ---------------------------------------------------------------------- */

TEST(test_get_str_found) {
    const char *json = "{\"model\":\"claude-opus-4-6\"}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_str(&ctx, json, "model", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "claude-opus-4-6");
}

TEST(test_get_str_missing_key) {
    const char *json = "{\"model\":\"test\"}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_str(&ctx, json, "type", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

TEST(test_get_str_multiple_keys) {
    const char *json = "{\"a\":\"1\",\"b\":\"2\",\"c\":\"3\"}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[16];
    ASSERT_EQ(json_get_str(&ctx, json, "a", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "1");

    ASSERT_EQ(json_get_str(&ctx, json, "b", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "2");

    ASSERT_EQ(json_get_str(&ctx, json, "c", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "3");
}

TEST(test_get_str_buffer_too_small) {
    const char *json = "{\"key\":\"long_value_here\"}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[3];
    int r = json_get_str(&ctx, json, "key", out, sizeof(out));
    ASSERT_EQ(r, -1); /* value doesn't fit */
}

TEST(test_get_str_empty_value) {
    const char *json = "{\"content\":\"\"}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[16];
    int r = json_get_str(&ctx, json, "content", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "");
}

/* -------------------------------------------------------------------------
 * json_get_nested_str
 * ---------------------------------------------------------------------- */

TEST(test_get_nested_str_found) {
    const char *json = "{\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_nested_str(&ctx, json, "delta", "text", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "Hello");
}

TEST(test_get_nested_str_missing_parent) {
    const char *json = "{\"other\":{\"text\":\"Hello\"}}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_nested_str(&ctx, json, "delta", "text", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

TEST(test_get_nested_str_missing_child) {
    const char *json = "{\"delta\":{\"type\":\"text_delta\"}}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_nested_str(&ctx, json, "delta", "text", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

TEST(test_get_nested_str_claude_event) {
    const char *json = "{\"type\":\"content_block_delta\","
                       "\"delta\":{\"type\":\"text_delta\",\"text\":\"world\"}}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_nested_str(&ctx, json, "delta", "text", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "world");
}

/* -------------------------------------------------------------------------
 * json_get_array_item_str
 * ---------------------------------------------------------------------- */

TEST(test_get_array_item_str_found) {
    const char *json = "{\"choices\":[{\"finish_reason\":\"stop\"}]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_str(&ctx, json, "choices", "finish_reason",
                                    out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "stop");
}

TEST(test_get_array_item_str_missing_array) {
    const char *json = "{\"other\":[{\"key\":\"val\"}]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_str(&ctx, json, "choices", "key", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

TEST(test_get_array_item_str_empty_array) {
    const char *json = "{\"choices\":[]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_str(&ctx, json, "choices", "key", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

/* -------------------------------------------------------------------------
 * json_get_array_item_nested_str
 * ---------------------------------------------------------------------- */

TEST(test_get_array_item_nested_str_openai_format) {
    /* OpenAI/Ollama streaming chunk: choices[0].delta.content */
    const char *json = "{\"choices\":[{\"index\":0,\"delta\":"
                       "{\"content\":\"Hello!\"}}]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_nested_str(&ctx, json, "choices", "delta",
                                           "content", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "Hello!");
}

TEST(test_get_array_item_nested_str_missing) {
    const char *json = "{\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"}}]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_nested_str(&ctx, json, "choices", "delta",
                                           "content", out, sizeof(out));
    ASSERT_EQ(r, -1);
}

TEST(test_get_array_item_nested_str_empty_content) {
    /* Thinking model: content is "" during CoT phase */
    const char *json = "{\"choices\":[{\"delta\":{\"content\":\"\","
                       "\"reasoning\":\"Thinking\"}}]}";
    JsonCtx ctx;
    json_parse_ctx(&ctx, json, strlen(json));

    char out[64];
    int r = json_get_array_item_nested_str(&ctx, json, "choices", "delta",
                                           "content", out, sizeof(out));
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(out, "");
}

int main(void)
{
    fprintf(stderr, "=== test_json ===\n");

    RUN_TEST(test_parse_valid_object);
    RUN_TEST(test_parse_empty_object);
    RUN_TEST(test_parse_invalid_json);
    RUN_TEST(test_parse_empty_string);

    RUN_TEST(test_get_str_found);
    RUN_TEST(test_get_str_missing_key);
    RUN_TEST(test_get_str_multiple_keys);
    RUN_TEST(test_get_str_buffer_too_small);
    RUN_TEST(test_get_str_empty_value);

    RUN_TEST(test_get_nested_str_found);
    RUN_TEST(test_get_nested_str_missing_parent);
    RUN_TEST(test_get_nested_str_missing_child);
    RUN_TEST(test_get_nested_str_claude_event);

    RUN_TEST(test_get_array_item_str_found);
    RUN_TEST(test_get_array_item_str_missing_array);
    RUN_TEST(test_get_array_item_str_empty_array);

    RUN_TEST(test_get_array_item_nested_str_openai_format);
    RUN_TEST(test_get_array_item_nested_str_missing);
    RUN_TEST(test_get_array_item_nested_str_empty_content);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
