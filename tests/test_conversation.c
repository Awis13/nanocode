/*
 * test_conversation.c — unit tests for conversation management (CMP-118)
 *
 * Tests conv_new, conv_free, conv_add, conv_add_tool_use, conv_add_tool_result,
 * conv_to_messages, conv_save, and conv_load.
 */

#include "test.h"
#include "../src/agent/conversation.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Return a temp path for save/load tests. */
static const char *tmp_path(void)
{
    return "/tmp/test_conversation_roundtrip.json";
}

/* -------------------------------------------------------------------------
 * conv_new
 * ---------------------------------------------------------------------- */

TEST(test_conv_new_basic)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);
    ASSERT_NOT_NULL(conv->conv_id);
    ASSERT_EQ(conv->nturn, 0);

    arena_free(a);
}

TEST(test_conv_new_uuid_format)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);
    ASSERT_NOT_NULL(conv->conv_id);

    /* UUID v4 must contain '-' characters. */
    ASSERT_NOT_NULL(strchr(conv->conv_id, '-'));

    /* Standard UUID format: 8-4-4-4-12 = 36 chars including 4 dashes. */
    int dashes = 0;
    for (const char *p = conv->conv_id; *p; p++)
        if (*p == '-')
            dashes++;
    ASSERT_EQ(dashes, 4);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * conv_free safety
 * ---------------------------------------------------------------------- */

TEST(test_conv_free_null)
{
    /* Must not crash. */
    conv_free(NULL);
    ASSERT_TRUE(1);
}

TEST(test_conv_free_clears_pointers)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add(conv, "user", "hello");
    ASSERT_EQ(conv->nturn, 1);

    conv_free(conv);
    ASSERT_EQ(conv->nturn, 0);
    ASSERT_NULL(conv->turns);
    ASSERT_NULL(conv->conv_id);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * conv_add NULL safety
 * ---------------------------------------------------------------------- */

TEST(test_conv_add_null_conv)
{
    /* Must not crash. */
    conv_add(NULL, "user", "hello");
    ASSERT_TRUE(1);
}

TEST(test_conv_add_null_role)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add(conv, NULL, "hello"); /* should not crash */
    ASSERT_EQ(conv->nturn, 0);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * conv_add basic
 * ---------------------------------------------------------------------- */

TEST(test_conv_add_grows)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    const char *roles[]    = { "user", "assistant", "user" };
    const char *contents[] = { "hi",   "hello",     "again" };

    for (int i = 0; i < 3; i++)
        conv_add(conv, roles[i], contents[i]);

    ASSERT_EQ(conv->nturn, 3);
    ASSERT_STR_EQ(conv->turns[0].role,    "user");
    ASSERT_STR_EQ(conv->turns[0].content, "hi");
    ASSERT_STR_EQ(conv->turns[1].role,    "assistant");
    ASSERT_STR_EQ(conv->turns[1].content, "hello");
    ASSERT_STR_EQ(conv->turns[2].role,    "user");
    ASSERT_STR_EQ(conv->turns[2].content, "again");

    arena_free(a);
}

TEST(test_conv_add_many_grows_cap)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    /* Add more than initial cap=8 to exercise doubling. */
    for (int i = 0; i < 20; i++)
        conv_add(conv, "user", "x");

    ASSERT_EQ(conv->nturn, 20);
    ASSERT_TRUE(conv->cap >= 20);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * conv_to_messages
 * ---------------------------------------------------------------------- */

TEST(test_conv_to_messages_null)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    int nmsg = 99;
    Message *msgs = conv_to_messages(NULL, &nmsg, a);
    ASSERT_NULL(msgs);
    ASSERT_EQ(nmsg, 0);

    arena_free(a);
}

TEST(test_conv_to_messages_empty)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    int nmsg = 99;
    Message *msgs = conv_to_messages(conv, &nmsg, a);
    ASSERT_NULL(msgs);
    ASSERT_EQ(nmsg, 0);

    arena_free(a);
}

TEST(test_conv_to_messages_basic)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add(conv, "user",      "what is 2+2?");
    conv_add(conv, "assistant", "4");

    int nmsg = 0;
    Message *msgs = conv_to_messages(conv, &nmsg, a);
    ASSERT_NOT_NULL(msgs);
    ASSERT_EQ(nmsg, 2);
    ASSERT_STR_EQ(msgs[0].role,    "user");
    ASSERT_STR_EQ(msgs[0].content, "what is 2+2?");
    ASSERT_STR_EQ(msgs[1].role,    "assistant");
    ASSERT_STR_EQ(msgs[1].content, "4");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Tool use / result
 * ---------------------------------------------------------------------- */

TEST(test_conv_add_tool_use_content)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add_tool_use(conv, "toolu_01", "bash", "{\"command\":\"ls\"}");
    ASSERT_EQ(conv->nturn, 1);
    ASSERT_EQ(conv->turns[0].is_tool, 1);
    ASSERT_STR_EQ(conv->turns[0].role, "assistant");

    /* Content must contain expected fields. */
    const char *c = conv->turns[0].content;
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(strstr(c, "tool_use"));
    ASSERT_NOT_NULL(strstr(c, "toolu_01"));
    ASSERT_NOT_NULL(strstr(c, "bash"));

    arena_free(a);
}

TEST(test_conv_add_tool_result_content)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add_tool_result(conv, "toolu_01", "file1.txt\nfile2.txt");
    ASSERT_EQ(conv->nturn, 1);
    ASSERT_EQ(conv->turns[0].is_tool, 1);
    ASSERT_STR_EQ(conv->turns[0].role, "user");

    const char *c = conv->turns[0].content;
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(strstr(c, "tool_result"));
    ASSERT_NOT_NULL(strstr(c, "toolu_01"));
    /* Newline in content should be escaped as \n. */
    ASSERT_NOT_NULL(strstr(c, "\\n"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Round-trip save/load
 * ---------------------------------------------------------------------- */

TEST(test_roundtrip_basic)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add(conv, "user",      "hello world");
    conv_add(conv, "assistant", "greetings");
    conv_add(conv, "user",      "how are you?");

    int rc = conv_save(conv, tmp_path());
    ASSERT_EQ(rc, 0);

    Arena *b = arena_new(1 << 20);
    ASSERT_NOT_NULL(b);

    Conversation *loaded = conv_load(b, tmp_path());
    ASSERT_NOT_NULL(loaded);
    ASSERT_EQ(loaded->nturn, 3);

    ASSERT_STR_EQ(loaded->turns[0].role,    "user");
    ASSERT_STR_EQ(loaded->turns[0].content, "hello world");
    ASSERT_STR_EQ(loaded->turns[1].role,    "assistant");
    ASSERT_STR_EQ(loaded->turns[1].content, "greetings");
    ASSERT_STR_EQ(loaded->turns[2].role,    "user");
    ASSERT_STR_EQ(loaded->turns[2].content, "how are you?");

    arena_free(a);
    arena_free(b);
}

TEST(test_roundtrip_conv_id_preserved)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    /* Copy conv_id before saving. */
    char saved_id[64];
    strncpy(saved_id, conv->conv_id, sizeof(saved_id) - 1);
    saved_id[sizeof(saved_id) - 1] = '\0';

    conv_add(conv, "user", "test");

    int rc = conv_save(conv, tmp_path());
    ASSERT_EQ(rc, 0);

    Arena *b = arena_new(1 << 20);
    ASSERT_NOT_NULL(b);

    Conversation *loaded = conv_load(b, tmp_path());
    ASSERT_NOT_NULL(loaded);
    ASSERT_NOT_NULL(loaded->conv_id);
    ASSERT_STR_EQ(loaded->conv_id, saved_id);

    arena_free(a);
    arena_free(b);
}

TEST(test_roundtrip_tool_flags)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    conv_add(conv, "user", "run ls");
    conv_add_tool_use(conv, "toolu_42", "bash", "{\"command\":\"ls -la\"}");
    conv_add_tool_result(conv, "toolu_42", "total 8\ndrwxr-xr-x 2 user user");

    ASSERT_EQ(conv->nturn, 3);
    ASSERT_EQ(conv->turns[0].is_tool, 0);
    ASSERT_EQ(conv->turns[1].is_tool, 1);
    ASSERT_EQ(conv->turns[2].is_tool, 1);

    int rc = conv_save(conv, tmp_path());
    ASSERT_EQ(rc, 0);

    Arena *b = arena_new(1 << 20);
    ASSERT_NOT_NULL(b);

    Conversation *loaded = conv_load(b, tmp_path());
    ASSERT_NOT_NULL(loaded);
    ASSERT_EQ(loaded->nturn, 3);

    ASSERT_EQ(loaded->turns[0].is_tool, 0);
    ASSERT_EQ(loaded->turns[1].is_tool, 1);
    ASSERT_EQ(loaded->turns[2].is_tool, 1);

    ASSERT_STR_EQ(loaded->turns[0].role, "user");
    ASSERT_STR_EQ(loaded->turns[1].role, "assistant");
    ASSERT_STR_EQ(loaded->turns[2].role, "user");

    arena_free(a);
    arena_free(b);
}

TEST(test_roundtrip_special_chars)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);

    /* Content with characters that require JSON escaping. */
    conv_add(conv, "user",      "say \"hello\\nworld\"");
    conv_add(conv, "assistant", "line1\nline2\ttabbed");

    int rc = conv_save(conv, tmp_path());
    ASSERT_EQ(rc, 0);

    Arena *b = arena_new(1 << 20);
    ASSERT_NOT_NULL(b);

    Conversation *loaded = conv_load(b, tmp_path());
    ASSERT_NOT_NULL(loaded);
    ASSERT_EQ(loaded->nturn, 2);

    ASSERT_STR_EQ(loaded->turns[0].content, "say \"hello\\nworld\"");
    ASSERT_STR_EQ(loaded->turns[1].content, "line1\nline2\ttabbed");

    arena_free(a);
    arena_free(b);
}

TEST(test_conv_load_nonexistent)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_load(a, "/tmp/this_file_does_not_exist_xyzzy.json");
    ASSERT_NULL(conv);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Version field (CMP-165)
 * ---------------------------------------------------------------------- */

/* conv_save must write "version":1 as the first field. */
TEST(test_version_field_in_saved_file)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);
    conv_add(conv, "user", "version test");

    const char *path = "/tmp/test_conv_version.json";
    int rc = conv_save(conv, path);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    char raw[256] = {0};
    size_t nr = fread(raw, 1, sizeof(raw) - 1, fp);
    fclose(fp);
    ASSERT_TRUE(nr > 0);
    ASSERT_NOT_NULL(strstr(raw, "\"version\":1"));

    arena_free(a);
}

/* Roundtrip still works after adding version field. */
TEST(test_roundtrip_with_version)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_new(a);
    ASSERT_NOT_NULL(conv);
    conv_add(conv, "user",      "version roundtrip");
    conv_add(conv, "assistant", "ok");

    const char *path = "/tmp/test_conv_version_rt.json";
    int rc = conv_save(conv, path);
    ASSERT_EQ(rc, 0);

    Arena *b = arena_new(1 << 20);
    ASSERT_NOT_NULL(b);

    Conversation *loaded = conv_load(b, path);
    ASSERT_NOT_NULL(loaded);
    ASSERT_EQ(loaded->nturn, 2);
    ASSERT_STR_EQ(loaded->turns[0].content, "version roundtrip");
    ASSERT_STR_EQ(loaded->turns[1].content, "ok");

    arena_free(a);
    arena_free(b);
}

/* Legacy files without version field must load gracefully. */
TEST(test_legacy_load_no_version_field)
{
    const char *path = "/tmp/test_conv_legacy.json";
    const char *legacy = "{\"conv_id\":\"legacy-id\","
                         "\"turns\":[{\"role\":\"user\","
                         "\"content\":\"legacy msg\",\"is_tool\":0}]}";

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    fputs(legacy, fp);
    fclose(fp);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_load(a, path);
    ASSERT_NOT_NULL(conv);
    ASSERT_EQ(conv->nturn, 1);
    ASSERT_STR_EQ(conv->turns[0].role,    "user");
    ASSERT_STR_EQ(conv->turns[0].content, "legacy msg");
    ASSERT_STR_EQ(conv->conv_id,          "legacy-id");

    arena_free(a);
}

/* Files with an unsupported version must be rejected. */
TEST(test_unsupported_version_rejected)
{
    const char *path = "/tmp/test_conv_future.json";
    const char *future = "{\"version\":99,\"conv_id\":\"x\","
                         "\"turns\":[]}";

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    fputs(future, fp);
    fclose(fp);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    Conversation *conv = conv_load(a, path);
    ASSERT_NULL(conv);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_conversation ===\n");

    RUN_TEST(test_conv_new_basic);
    RUN_TEST(test_conv_new_uuid_format);

    RUN_TEST(test_conv_free_null);
    RUN_TEST(test_conv_free_clears_pointers);

    RUN_TEST(test_conv_add_null_conv);
    RUN_TEST(test_conv_add_null_role);
    RUN_TEST(test_conv_add_grows);
    RUN_TEST(test_conv_add_many_grows_cap);

    RUN_TEST(test_conv_to_messages_null);
    RUN_TEST(test_conv_to_messages_empty);
    RUN_TEST(test_conv_to_messages_basic);

    RUN_TEST(test_conv_add_tool_use_content);
    RUN_TEST(test_conv_add_tool_result_content);

    RUN_TEST(test_roundtrip_basic);
    RUN_TEST(test_roundtrip_conv_id_preserved);
    RUN_TEST(test_roundtrip_tool_flags);
    RUN_TEST(test_roundtrip_special_chars);
    RUN_TEST(test_conv_load_nonexistent);

    RUN_TEST(test_version_field_in_saved_file);
    RUN_TEST(test_roundtrip_with_version);
    RUN_TEST(test_legacy_load_no_version_field);
    RUN_TEST(test_unsupported_version_rejected);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
