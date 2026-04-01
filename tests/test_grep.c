/*
 * test_grep.c — unit tests for the grep tool
 *
 * Handlers are invoked directly (grep_search) without going through the
 * executor registry. A minimal executor stub satisfies link-time dependencies.
 */

#include "test.h"
#include "../src/tools/grep.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Minimal executor stub
 * ---------------------------------------------------------------------- */

void tool_register(const char *name, const char *schema_json, ToolHandler fn)
{
    (void)name; (void)schema_json; (void)fn;
}

ToolResult tool_invoke(Arena *arena, const char *name, const char *args_json)
{
    (void)arena; (void)name; (void)args_json;
    return (ToolResult){ .error = 1, .content = (char *)"stub", .len = 4 };
}

void tool_registry_reset(void) {}

char *tool_result_to_json(Arena *arena, const char *tool_use_id,
                          const ToolResult *result)
{
    (void)arena; (void)tool_use_id; (void)result;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

#define TMP_DIR "/tmp/test_grep_"

/* Write content to a temp file, return path (static buffer). */
static const char *write_tmp(const char *name, const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), TMP_DIR "%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

/* -------------------------------------------------------------------------
 * files_with_matches — basic match
 * ---------------------------------------------------------------------- */

TEST(test_fwm_basic)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("fwm1.txt",
                                  "hello world\n"
                                  "foo bar\n"
                                  "baz qux\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"foo\",\"path\":\"%s\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, path));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * files_with_matches — no match returns empty output
 * ---------------------------------------------------------------------- */

TEST(test_fwm_no_match)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("fwm2.txt", "hello world\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"zzznomatch\",\"path\":\"%s\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_EQ((int)r.len, 0);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — matching lines returned with path:lineno: prefix
 * ---------------------------------------------------------------------- */

TEST(test_content_basic)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("content1.txt",
                                  "line one\n"
                                  "line two has foo\n"
                                  "line three\n"
                                  "another foo here\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"foo\",\"path\":\"%s\","
             "\"output_mode\":\"content\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    /* Both matching lines present */
    ASSERT_NOT_NULL(strstr(r.content, ":2:"));
    ASSERT_NOT_NULL(strstr(r.content, ":4:"));
    /* Non-matching lines absent */
    ASSERT_NULL(strstr(r.content, "line one"));
    ASSERT_NULL(strstr(r.content, "line three"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — case-insensitive (-i)
 * ---------------------------------------------------------------------- */

TEST(test_content_case_insensitive)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("ci.txt",
                                  "Hello World\n"
                                  "HELLO NANOCODE\n"
                                  "no match here\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"hello\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"-i\":true}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "Hello World"));
    ASSERT_NOT_NULL(strstr(r.content, "HELLO NANOCODE"));
    ASSERT_NULL(strstr(r.content, "no match"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — regex pattern
 * ---------------------------------------------------------------------- */

TEST(test_content_regex)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("regex.txt",
                                  "int foo = 1;\n"
                                  "char *bar = NULL;\n"
                                  "double baz = 3.14;\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"^[a-z]+[[:space:]]\",\"path\":\"%s\","
             "\"output_mode\":\"content\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    /* All three lines match (start with lowercase word + space) */
    ASSERT_NOT_NULL(strstr(r.content, "int foo"));
    ASSERT_NOT_NULL(strstr(r.content, "char"));
    ASSERT_NOT_NULL(strstr(r.content, "double"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — after context (-A)
 * ---------------------------------------------------------------------- */

TEST(test_content_after_context)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("after.txt",
                                  "line1\n"
                                  "MATCH\n"
                                  "after1\n"
                                  "after2\n"
                                  "unrelated\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"MATCH\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"-A\":2}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "MATCH"));
    ASSERT_NOT_NULL(strstr(r.content, "after1"));
    ASSERT_NOT_NULL(strstr(r.content, "after2"));
    /* Unrelated line (beyond A=2) should not appear */
    ASSERT_NULL(strstr(r.content, "unrelated"));
    /* line1 is before the match but no -B requested */
    ASSERT_NULL(strstr(r.content, "line1"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — before context (-B)
 * ---------------------------------------------------------------------- */

TEST(test_content_before_context)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("before.txt",
                                  "before2\n"
                                  "before1\n"
                                  "MATCH\n"
                                  "after1\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"MATCH\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"-B\":2}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "MATCH"));
    ASSERT_NOT_NULL(strstr(r.content, "before1"));
    ASSERT_NOT_NULL(strstr(r.content, "before2"));
    ASSERT_NULL(strstr(r.content, "after1"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — -C sets both before and after
 * ---------------------------------------------------------------------- */

TEST(test_content_C_context)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("ctx.txt",
                                  "before\n"
                                  "MATCH\n"
                                  "after\n"
                                  "far\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"MATCH\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"-C\":1}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "before"));
    ASSERT_NOT_NULL(strstr(r.content, "MATCH"));
    ASSERT_NOT_NULL(strstr(r.content, "after"));
    ASSERT_NULL(strstr(r.content, "far"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — -- separator between non-contiguous groups
 * ---------------------------------------------------------------------- */

TEST(test_content_separator)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("sep.txt",
                                  "MATCH1\n"
                                  "gap1\n"
                                  "gap2\n"
                                  "gap3\n"
                                  "MATCH2\n");

    /* No context — matches are non-contiguous → -- separator expected. */
    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"MATCH\",\"path\":\"%s\","
             "\"output_mode\":\"content\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "MATCH1"));
    ASSERT_NOT_NULL(strstr(r.content, "MATCH2"));
    ASSERT_NOT_NULL(strstr(r.content, "--"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * content mode — no duplicate context lines when windows overlap
 * ---------------------------------------------------------------------- */

TEST(test_content_no_duplicate_context)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("dup.txt",
                                  "MATCH1\n"
                                  "shared\n"
                                  "MATCH2\n");

    /* -A 1 on first match overlaps with -B 1 on second; "shared" printed once. */
    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"MATCH\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"-C\":1}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);

    /* Count occurrences of "shared" — should be exactly 1. */
    int count = 0;
    const char *p = r.content;
    while ((p = strstr(p, "shared")) != NULL) {
        count++;
        p++;
    }
    ASSERT_EQ(count, 1);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * count mode — per-file counts
 * ---------------------------------------------------------------------- */

TEST(test_count_mode)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("count.txt",
                                  "foo line\n"
                                  "bar line\n"
                                  "foo again\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"foo\",\"path\":\"%s\","
             "\"output_mode\":\"count\"}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, ":2"));   /* 2 matching lines */

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * head_limit — output is capped
 * ---------------------------------------------------------------------- */

TEST(test_head_limit)
{
    Arena *a = arena_new(2 << 20);

    /* Build a file with 20 matching lines. */
    char content[1024];
    content[0] = '\0';
    for (int i = 0; i < 20; i++) {
        char line[32];
        snprintf(line, sizeof(line), "match line %d\n", i);
        strcat(content, line);
    }
    const char *path = write_tmp("hl.txt", content);

    char args[512];
    snprintf(args, sizeof(args),
             "{\"pattern\":\"match\",\"path\":\"%s\","
             "\"output_mode\":\"content\",\"head_limit\":5}", path);

    ToolResult r = grep_search(a, args);
    ASSERT_EQ(r.error, 0);

    /* Count newlines — should be <= 5 */
    int nl = 0;
    for (size_t i = 0; i < r.len; i++)
        if (r.content[i] == '\n') nl++;
    ASSERT_TRUE(nl <= 5);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * invalid pattern — returns error
 * ---------------------------------------------------------------------- */

TEST(test_invalid_pattern)
{
    Arena *a = arena_new(1 << 20);

    ToolResult r = grep_search(a,
        "{\"pattern\":\"[invalid\",\"path\":\"/tmp\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "invalid pattern"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * missing pattern — returns error
 * ---------------------------------------------------------------------- */

TEST(test_missing_pattern)
{
    Arena *a = arena_new(1 << 20);

    ToolResult r = grep_search(a, "{\"path\":\"/tmp\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "pattern"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * recursive directory walk — finds files in subdirs
 * ---------------------------------------------------------------------- */

TEST(test_recursive_walk)
{
    Arena *a = arena_new(2 << 20);

    /* Use src/util as a known directory with .c files. */
    ToolResult r = grep_search(a,
        "{\"pattern\":\"arena\",\"path\":\"src/util\","
        "\"glob\":\"*.c\",\"output_mode\":\"files_with_matches\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "arena.c"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * glob filter — only .h files
 * ---------------------------------------------------------------------- */

TEST(test_glob_filter)
{
    Arena *a = arena_new(2 << 20);

    ToolResult r = grep_search(a,
        "{\"pattern\":\"Arena\",\"path\":\"src/util\","
        "\"glob\":\"*.h\",\"output_mode\":\"files_with_matches\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "arena.h"));
    /* Should not find .c files */
    ASSERT_NULL(strstr(r.content, "arena.c"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * .git directory is always skipped
 * ---------------------------------------------------------------------- */

TEST(test_git_dir_skipped)
{
    Arena *a = arena_new(2 << 20);

    /* Walk from project root; .git/ should be skipped. */
    ToolResult r = grep_search(a,
        "{\"pattern\":\"HEAD\",\"path\":\".\","
        "\"glob\":\"HEAD\",\"output_mode\":\"files_with_matches\"}");
    ASSERT_EQ(r.error, 0);
    /* .git/HEAD would match the glob but should never be in output. */
    ASSERT_NULL(strstr(r.content, ".git"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * registration smoke test
 * ---------------------------------------------------------------------- */

TEST(test_register_no_crash)
{
    grep_register();
    ASSERT_TRUE(1);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    /* Tests that use relative paths expect to be run from project root. */
    fprintf(stderr, "=== test_grep ===\n");

    RUN_TEST(test_fwm_basic);
    RUN_TEST(test_fwm_no_match);
    RUN_TEST(test_content_basic);
    RUN_TEST(test_content_case_insensitive);
    RUN_TEST(test_content_regex);
    RUN_TEST(test_content_after_context);
    RUN_TEST(test_content_before_context);
    RUN_TEST(test_content_C_context);
    RUN_TEST(test_content_separator);
    RUN_TEST(test_content_no_duplicate_context);
    RUN_TEST(test_count_mode);
    RUN_TEST(test_head_limit);
    RUN_TEST(test_invalid_pattern);
    RUN_TEST(test_missing_pattern);
    RUN_TEST(test_recursive_walk);
    RUN_TEST(test_glob_filter);
    RUN_TEST(test_git_dir_skipped);
    RUN_TEST(test_register_no_crash);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
