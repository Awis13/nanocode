/*
 * test_fileops.c — unit tests for file operation tools
 *
 * Tests invoke the handlers directly (fileops_read, fileops_write, etc.)
 * rather than going through the executor registry.  A minimal executor
 * stub satisfies the link-time dependency on tool_register / tool_invoke.
 */

#include "test.h"
#include "../src/tools/fileops.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Minimal executor stub
 * ---------------------------------------------------------------------- */

void tool_register(const char *name, const char *schema_json, ToolHandler fn,
                   ToolSafety safety)
{
    (void)name; (void)schema_json; (void)fn; (void)safety;
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

#define TMP_DIR    "tests/tmp"
#define TMP_PREFIX TMP_DIR "/test_fileops_"

/* Write `content` to a temp file, return its path (static buffer). */
static const char *write_tmp(const char *suffix, const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), TMP_PREFIX "%s", suffix);
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

/* Read the entire contents of a file into a static buffer. */
static const char *read_tmp(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) { buf[0] = '\0'; return buf; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * edit_file — not found
 * ---------------------------------------------------------------------- */

TEST(test_edit_not_found)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("e1.txt", "hello world\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"no such string\","
             "\"new_string\":\"replacement\"}",
             path);

    ToolResult r = fileops_edit(a, args);
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "not found"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * edit_file — ambiguous (>1 occurrence, replace_all omitted → false)
 * ---------------------------------------------------------------------- */

TEST(test_edit_ambiguous)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("e2.txt", "foo bar foo baz foo\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"foo\","
             "\"new_string\":\"qux\"}",
             path);

    ToolResult r = fileops_edit(a, args);
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "3"));   /* reports count */

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * edit_file — single unique occurrence
 * ---------------------------------------------------------------------- */

TEST(test_edit_single_replace)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("e3.txt", "hello world\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"world\","
             "\"new_string\":\"nanocode\"}",
             path);

    ToolResult r = fileops_edit(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "hello nanocode\n");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * edit_file — replace_all=true replaces every occurrence
 * ---------------------------------------------------------------------- */

TEST(test_edit_replace_all)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("e4.txt", "foo bar foo baz foo\n");

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"foo\","
             "\"new_string\":\"qux\","
             "\"replace_all\":true}",
             path);

    ToolResult r = fileops_edit(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "qux bar qux baz qux\n");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * edit_file — JSON-escaped newline in old_string is decoded properly
 * ---------------------------------------------------------------------- */

TEST(test_edit_escaped_newline)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("e5.txt", "line1\nline2\n");

    /* JSON source: old_string contains a literal newline (\\n in JSON). */
    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"line1\\nline2\","
             "\"new_string\":\"merged\"}",
             path);

    ToolResult r = fileops_edit(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "merged\n");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * write_file — basic round-trip
 * ---------------------------------------------------------------------- */

TEST(test_write_basic)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = TMP_PREFIX "w1.txt";

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"hello fileops\\n\"}",
             path);

    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "hello fileops\n");

    unlink(path);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * write_file — creates parent directories (mkdir -p semantics)
 * ---------------------------------------------------------------------- */

TEST(test_write_creates_dirs)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = TMP_PREFIX "subdir/nested/file.txt";

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"deep\"}",
             path);

    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "deep");

    unlink(path);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * read_file — full file without range
 * ---------------------------------------------------------------------- */

TEST(test_read_basic)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("r1.txt", "line1\nline2\nline3\n");

    char args[256];
    snprintf(args, sizeof(args), "{\"path\":\"%s\"}", path);

    ToolResult r = fileops_read(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(r.content, "line1\nline2\nline3\n");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * read_file — line range (offset + limit)
 * ---------------------------------------------------------------------- */

TEST(test_read_range)
{
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("r2.txt", "line1\nline2\nline3\nline4\n");

    char args[256];
    /* offset=1, limit=2 → lines 1 and 2 (0-indexed: line2, line3) */
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"offset\":1,\"limit\":2}", path);

    ToolResult r = fileops_read(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(r.content, "line2\nline3\n");

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * read_file — missing file returns error
 * ---------------------------------------------------------------------- */

TEST(test_read_missing_file)
{
    Arena *a = arena_new(1 << 20);

    ToolResult r = fileops_read(a,
        "{\"path\":\"tests/tmp/no_such_file_fileops_xyz.txt\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "cannot open"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * glob — ** pattern finds .c files recursively
 * ---------------------------------------------------------------------- */

TEST(test_glob_recursive_c)
{
    Arena *a = arena_new(2 << 20);

    /* The test binary is run from the project root, so src/util exists. */
    ToolResult r = fileops_glob(a,
        "{\"pattern\":\"*.c\",\"path\":\"src/util\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "arena.c"));
    ASSERT_NOT_NULL(strstr(r.content, "buf.c"));
    ASSERT_NOT_NULL(strstr(r.content, "json.c"));

    arena_free(a);
}

TEST(test_glob_double_star)
{
    Arena *a = arena_new(2 << 20);

    ToolResult r = fileops_glob(a,
        "{\"pattern\":\"**/*.h\",\"path\":\"src/util\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(strstr(r.content, "arena.h"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * glob — pattern with no matches returns empty string
 * ---------------------------------------------------------------------- */

TEST(test_glob_no_match)
{
    Arena *a = arena_new(1 << 20);

    ToolResult r = fileops_glob(a,
        "{\"pattern\":\"**/*.nonexistent\",\"path\":\"src\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_EQ((int)r.len, 0);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * fileops_set_limits — write within size limit → success
 * ---------------------------------------------------------------------- */

TEST(test_write_within_size_limit)
{
    fileops_set_limits(1024 * 1024, 100); /* 1 MB, 100 files */
    Arena      *a    = arena_new(2 << 20);
    const char *path = TMP_PREFIX "lim1.txt";

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"small content\"}", path);

    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 0);

    unlink(path);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * fileops_set_limits — write exceeding max_file_size → error
 * ---------------------------------------------------------------------- */

TEST(test_write_exceeds_file_size)
{
    fileops_set_limits(10, 100); /* 10 bytes max */
    Arena      *a    = arena_new(2 << 20);
    const char *path = TMP_PREFIX "lim2.txt";

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"this is more than ten bytes\"}",
             path);

    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(strstr(r.content, "file too large"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * fileops_set_limits — exceed max_files_created → error on the extra file
 * ---------------------------------------------------------------------- */

TEST(test_write_session_file_limit)
{
    fileops_set_limits(1024 * 1024, 2); /* allow at most 2 new files */
    Arena *a = arena_new(2 << 20);

    const char *p1 = TMP_PREFIX "fc1.txt";
    const char *p2 = TMP_PREFIX "fc2.txt";
    const char *p3 = TMP_PREFIX "fc3.txt";

    /* Clean up any stale files from previous runs so new-file detection is accurate. */
    unlink(p1);
    unlink(p2);
    unlink(p3);

    /* First file — OK */
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"a\"}", p1);
    ToolResult r1 = fileops_write(a, args);
    ASSERT_EQ(r1.error, 0);

    /* Second file — OK */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"b\"}", p2);
    ToolResult r2 = fileops_write(a, args);
    ASSERT_EQ(r2.error, 0);

    /* Third file — should fail: limit is 2 */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"c\"}", p3);
    ToolResult r3 = fileops_write(a, args);
    ASSERT_EQ(r3.error, 1);
    ASSERT_NOT_NULL(strstr(r3.content, "session file limit reached"));

    unlink(p1);
    unlink(p2);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * fileops_set_limits — edit (overwrite) does NOT count against max_files
 * ---------------------------------------------------------------------- */

TEST(test_edit_no_new_file_count)
{
    fileops_set_limits(1024 * 1024, 1); /* only 1 new file allowed */
    Arena      *a    = arena_new(2 << 20);
    const char *path = write_tmp("fce.txt", "original content\n");

    /* Write a new file — consumes the quota */
    const char *p2 = TMP_PREFIX "fce2.txt";
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"new\"}", p2);
    ToolResult rw = fileops_write(a, args);
    ASSERT_EQ(rw.error, 0);

    /* Edit existing file — must not count as a new file */
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\","
             "\"old_string\":\"original\","
             "\"new_string\":\"updated\"}",
             path);
    ToolResult re = fileops_edit(a, args);
    ASSERT_EQ(re.error, 0);

    unlink(p2);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * registration smoke test
 * ---------------------------------------------------------------------- */

TEST(test_register_all_no_crash)
{
    fileops_register_all(); /* stub tool_register is a no-op */
    ASSERT_TRUE(1);
}

/* -------------------------------------------------------------------------
 * fileops_set_confirm_cb — integration tests
 * ---------------------------------------------------------------------- */

/* Callback that always rejects (returns 0). */
static int cb_reject(const char *path, const char *old_content,
                     const char *new_content, char **out_replacement,
                     void *ctx)
{
    (void)path; (void)old_content; (void)new_content;
    (void)out_replacement; (void)ctx;
    return 0;
}

/* Callback that always approves (returns 1). */
static int cb_approve(const char *path, const char *old_content,
                      const char *new_content, char **out_replacement,
                      void *ctx)
{
    (void)path; (void)old_content; (void)new_content;
    (void)out_replacement; (void)ctx;
    return 1;
}

/* Callback that disables itself (returns < 0). */
static int cb_disable(const char *path, const char *old_content,
                      const char *new_content, char **out_replacement,
                      void *ctx)
{
    (void)path; (void)old_content; (void)new_content;
    (void)out_replacement; (void)ctx;
    return -1;
}

/* Callback that replaces content with a fixed string. */
static int cb_replace(const char *path, const char *old_content,
                      const char *new_content, char **out_replacement,
                      void *ctx)
{
    (void)path; (void)old_content; (void)new_content; (void)ctx;
    if (out_replacement) *out_replacement = strdup("replaced_by_callback\n");
    return 1;
}

/* cb=reject: write_file returns error, file not written. */
TEST(test_confirm_cb_reject_write)
{
    fileops_set_limits(0, 0);
    fileops_set_confirm_cb(cb_reject, NULL);

    Arena *a = arena_new(65536);
    const char *path = TMP_PREFIX "confirm_reject";
    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"hello\"}", path);
    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 1); /* rejected → error */
    ASSERT_EQ(access(path, F_OK), -1); /* file must not exist */

    fileops_set_confirm_cb(NULL, NULL);
    arena_free(a);
}

/* cb=approve: write_file succeeds and writes content. */
TEST(test_confirm_cb_approve_write)
{
    fileops_set_limits(0, 0);
    fileops_set_confirm_cb(cb_approve, NULL);

    Arena *a = arena_new(65536);
    const char *path = TMP_PREFIX "confirm_approve";
    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"approved\"}", path);
    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "approved");

    fileops_set_confirm_cb(NULL, NULL);
    unlink(path);
    arena_free(a);
}

/* cb=disable: first write applies; subsequent writes skip the callback. */
TEST(test_confirm_cb_disable_on_negative)
{
    fileops_set_limits(0, 0);
    fileops_set_confirm_cb(cb_disable, NULL);

    Arena *a = arena_new(65536);
    const char *p1 = TMP_PREFIX "confirm_dis1";
    const char *p2 = TMP_PREFIX "confirm_dis2";
    char args[512];

    /* First write: callback returns < 0 → apply + disable. */
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"first\"}", p1);
    ToolResult r1 = fileops_write(a, args);
    ASSERT_EQ(r1.error, 0);
    ASSERT_STR_EQ(read_tmp(p1), "first");

    /* Second write: callback is disabled → write proceeds without prompting. */
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"second\"}", p2);
    ToolResult r2 = fileops_write(a, args);
    ASSERT_EQ(r2.error, 0);
    ASSERT_STR_EQ(read_tmp(p2), "second");

    fileops_set_confirm_cb(NULL, NULL);
    unlink(p1);
    unlink(p2);
    arena_free(a);
}

/* cb that sets out_replacement: fileops writes replacement content. */
TEST(test_confirm_cb_replacement_write)
{
    fileops_set_limits(0, 0);
    fileops_set_confirm_cb(cb_replace, NULL);

    Arena *a = arena_new(65536);
    const char *path = TMP_PREFIX "confirm_replace";
    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"original\"}", path);
    ToolResult r = fileops_write(a, args);
    ASSERT_EQ(r.error, 0);
    ASSERT_STR_EQ(read_tmp(path), "replaced_by_callback\n");

    fileops_set_confirm_cb(NULL, NULL);
    unlink(path);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_fileops ===\n");
    mkdir(TMP_DIR, 0755); /* ensure temp dir exists; ignore EEXIST */

    RUN_TEST(test_edit_not_found);
    RUN_TEST(test_edit_ambiguous);
    RUN_TEST(test_edit_single_replace);
    RUN_TEST(test_edit_replace_all);
    RUN_TEST(test_edit_escaped_newline);

    RUN_TEST(test_write_basic);
    RUN_TEST(test_write_creates_dirs);

    RUN_TEST(test_read_basic);
    RUN_TEST(test_read_range);
    RUN_TEST(test_read_missing_file);

    RUN_TEST(test_glob_recursive_c);
    RUN_TEST(test_glob_double_star);
    RUN_TEST(test_glob_no_match);

    RUN_TEST(test_write_within_size_limit);
    RUN_TEST(test_write_exceeds_file_size);
    RUN_TEST(test_write_session_file_limit);
    RUN_TEST(test_edit_no_new_file_count);

    RUN_TEST(test_register_all_no_crash);

    RUN_TEST(test_confirm_cb_reject_write);
    RUN_TEST(test_confirm_cb_approve_write);
    RUN_TEST(test_confirm_cb_disable_on_negative);
    RUN_TEST(test_confirm_cb_replacement_write);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
