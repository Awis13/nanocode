/*
 * test_memory.c — unit tests for cross-session memory tool (CMP-153 P4)
 *
 * Tests run against the real filesystem using a tmp directory overridden
 * via $HOME so the actual user's ~/.nanocode/memory.md is never touched.
 */

#include "test.h"
#include "../src/tools/memory.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Fixture: redirect $HOME to a tmp dir
 * ---------------------------------------------------------------------- */

static char s_tmp_home[256];

static void setup_home(void)
{
    snprintf(s_tmp_home, sizeof(s_tmp_home), "/tmp/nc_test_%d", (int)getpid());
    mkdir(s_tmp_home, 0700);
    setenv("HOME", s_tmp_home, 1);
}

static void cleanup_home(void)
{
    /* Remove .nanocode/memory.md and directory if present. */
    char path[280];
    snprintf(path, sizeof(path), "%s/.nanocode/memory.md", s_tmp_home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.nanocode", s_tmp_home);
    rmdir(path);
    rmdir(s_tmp_home);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Write raw text to the memory file directly (bypassing the tool). */
static void seed_memory(const char *text)
{
    char dir[280], path[280];
    snprintf(dir,  sizeof(dir),  "%s/.nanocode",           s_tmp_home);
    snprintf(path, sizeof(path), "%s/.nanocode/memory.md", s_tmp_home);
    mkdir(dir, 0700);
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* Read memory file directly and return heap string (caller free()s). */
static char *slurp_memory(void)
{
    char path[280];
    snprintf(path, sizeof(path), "%s/.nanocode/memory.md", s_tmp_home);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0)                     { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Invoke memory_write via tool_invoke. */
static ToolResult invoke_write(Arena *a, const char *key, const char *val)
{
    /* Build simple JSON args. */
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"key\":\"%s\",\"content\":\"%s\"}", key, val);
    return tool_invoke(a, "memory_write", buf);
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

TEST(test_memory_write_creates_file) {
    setup_home();
    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    ToolResult r = invoke_write(a, "test-key", "hello world");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "\"ok\":true") != NULL);

    char *mem = slurp_memory();
    ASSERT_NOT_NULL(mem);
    ASSERT_TRUE(strstr(mem, "## test-key") != NULL);
    ASSERT_TRUE(strstr(mem, "hello world") != NULL);
    free(mem);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_write_replaces_existing_key) {
    setup_home();
    seed_memory("## key1\nold content\n\n## key2\nother\n\n");
    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    ToolResult r = invoke_write(a, "key1", "new content");
    ASSERT_EQ(r.error, 0);

    char *mem = slurp_memory();
    ASSERT_NOT_NULL(mem);
    ASSERT_TRUE(strstr(mem, "new content") != NULL);
    ASSERT_TRUE(strstr(mem, "old content") == NULL);
    /* key2 must still be present */
    ASSERT_TRUE(strstr(mem, "## key2")     != NULL);
    ASSERT_TRUE(strstr(mem, "other")       != NULL);
    free(mem);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_write_appends_new_key) {
    setup_home();
    seed_memory("## existing\nsome note\n\n");
    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    ToolResult r = invoke_write(a, "brand-new", "fresh entry");
    ASSERT_EQ(r.error, 0);

    char *mem = slurp_memory();
    ASSERT_NOT_NULL(mem);
    ASSERT_TRUE(strstr(mem, "## existing")  != NULL);
    ASSERT_TRUE(strstr(mem, "## brand-new") != NULL);
    ASSERT_TRUE(strstr(mem, "fresh entry")  != NULL);
    free(mem);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_write_missing_key_returns_error) {
    setup_home();
    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    ToolResult r = tool_invoke(a, "memory_write", "{\"content\":\"no key\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "key") != NULL);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_write_missing_content_returns_error) {
    setup_home();
    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    ToolResult r = tool_invoke(a, "memory_write", "{\"key\":\"k\"}");
    ASSERT_EQ(r.error, 1);
    ASSERT_NOT_NULL(r.content);
    ASSERT_TRUE(strstr(r.content, "content") != NULL);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_load_returns_null_when_no_file) {
    setup_home();

    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *mem = memory_load(a);
    /* No file exists — should return NULL. */
    ASSERT_TRUE(mem == NULL);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_load_returns_file_contents) {
    setup_home();
    seed_memory("## note\nhello from memory\n\n");

    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *mem = memory_load(a);
    ASSERT_NOT_NULL(mem);
    ASSERT_TRUE(strstr(mem, "hello from memory") != NULL);

    arena_free(a);
    cleanup_home();
}

TEST(test_memory_write_trims_over_200_lines) {
    setup_home();

    /*
     * Seed file with two sections of 99 content lines each.
     * Per-section line count (counting '\n'): 1 heading + 99 content + 1 blank = 101.
     * Total seeded: 202 lines.  Writing "section-c" appends 3 more lines (205 total),
     * which triggers trim (> MEMORY_MAX_LINES=200).
     */
    {
        char dir[280], path[280];
        snprintf(dir,  sizeof(dir),  "%s/.nanocode",           s_tmp_home);
        snprintf(path, sizeof(path), "%s/.nanocode/memory.md", s_tmp_home);
        mkdir(dir, 0700);
        FILE *f = fopen(path, "w");
        ASSERT_NOT_NULL(f);
        /* Section A: 99 content lines */
        fputs("## section-a\n", f);
        for (int i = 0; i < 99; i++) fprintf(f, "line %d\n", i);
        fputs("\n", f);
        /* Section B: 99 content lines */
        fputs("## section-b\n", f);
        for (int i = 0; i < 99; i++) fprintf(f, "line %d\n", i);
        fputs("\n", f);
        fclose(f);
    }

    tool_registry_reset();
    memory_tool_register();

    Arena *a = arena_new(8192);
    ASSERT_NOT_NULL(a);

    /* Writing a new key pushes us over 200 lines → oldest section trimmed. */
    ToolResult r = invoke_write(a, "section-c", "new entry");
    ASSERT_EQ(r.error, 0);

    /* section-a (oldest) should be gone; section-c (newest) should be there. */
    char *mem = slurp_memory();
    ASSERT_NOT_NULL(mem);
    ASSERT_TRUE(strstr(mem, "## section-a") == NULL);
    ASSERT_TRUE(strstr(mem, "## section-c") != NULL);
    free(mem);

    arena_free(a);
    cleanup_home();
}

int main(void)
{
    fprintf(stderr, "=== test_memory ===\n");

    RUN_TEST(test_memory_write_creates_file);
    RUN_TEST(test_memory_write_replaces_existing_key);
    RUN_TEST(test_memory_write_appends_new_key);
    RUN_TEST(test_memory_write_missing_key_returns_error);
    RUN_TEST(test_memory_write_missing_content_returns_error);
    RUN_TEST(test_memory_load_returns_null_when_no_file);
    RUN_TEST(test_memory_load_returns_file_contents);
    RUN_TEST(test_memory_write_trims_over_200_lines);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
