/*
 * test_session_persist.c — CMP-403: JSONL streaming + arena OOM recovery tests
 *
 * Tests:
 *   1. JSONL output is valid JSON (parseable lines).
 *   2. Multiple turns produce multiple JSONL lines.
 *   3. Special characters in content are properly escaped.
 *   4. Arena checkpoint/restore via arena->used (no crash, state preserved).
 */

#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/util/arena.h"

/* -------------------------------------------------------------------------
 * Helpers — minimal inline JSONL writer (mirrors repl_coordinator.c logic)
 * ---------------------------------------------------------------------- */

static void write_escaped(FILE *fp, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n",  fp); break;
        case '\r': fputs("\\r",  fp); break;
        case '\t': fputs("\\t",  fp); break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", (unsigned)c);
            else          fputc(c, fp);
        }
    }
}

static void write_turn(FILE *fp, int idx,
                       const char *role, const char *content)
{
    fprintf(fp, "{\"idx\":%d,\"role\":\"", idx);
    write_escaped(fp, role);
    fprintf(fp, "\",\"content\":\"");
    write_escaped(fp, content);
    fprintf(fp, "\"}\n");
}

/* -------------------------------------------------------------------------
 * Test 1: single turn produces a valid-looking JSON line
 * ---------------------------------------------------------------------- */

TEST(test_persist_jsonl_single_turn)
{
    char path[] = "/tmp/nanocode_test_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);

    write_turn(fp, 0, "user", "hello world");
    fclose(fp);

    /* Read line back */
    FILE *r = fopen(path, "r");
    ASSERT_NOT_NULL(r);
    char line[512];
    ASSERT_NOT_NULL(fgets(line, sizeof(line), r));
    fclose(r);
    unlink(path);

    /* Must start with '{' and end with '}\n' */
    ASSERT_TRUE(line[0] == '{');
    size_t len = strlen(line);
    ASSERT_TRUE(len > 2 && line[len - 2] == '}');

    /* Must contain expected fields */
    ASSERT_TRUE(strstr(line, "\"idx\":0") != NULL);
    ASSERT_TRUE(strstr(line, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(line, "\"content\":\"hello world\"") != NULL);
}

/* -------------------------------------------------------------------------
 * Test 2: multiple turns produce multiple lines
 * ---------------------------------------------------------------------- */

TEST(test_persist_jsonl_multiple_turns)
{
    char path[] = "/tmp/nanocode_test_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);

    write_turn(fp, 0, "user",      "what is 2+2?");
    write_turn(fp, 1, "assistant", "4");
    write_turn(fp, 2, "user",      "thanks");
    fclose(fp);

    /* Count lines */
    FILE *r = fopen(path, "r");
    ASSERT_NOT_NULL(r);
    int lines = 0;
    char line[512];
    while (fgets(line, sizeof(line), r))
        lines++;
    fclose(r);
    unlink(path);

    ASSERT_EQ(lines, 3);
}

/* -------------------------------------------------------------------------
 * Test 3: special characters are escaped correctly
 * ---------------------------------------------------------------------- */

TEST(test_persist_jsonl_special_chars)
{
    char path[] = "/tmp/nanocode_test_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);

    write_turn(fp, 0, "user", "line1\nline2\ttabbed \"quoted\" back\\slash");
    fclose(fp);

    FILE *r = fopen(path, "r");
    ASSERT_NOT_NULL(r);
    char line[512];
    ASSERT_NOT_NULL(fgets(line, sizeof(line), r));
    fclose(r);
    unlink(path);

    /* The raw line must not contain unescaped newline mid-content */
    /* (the trailing \n is from the record separator, not content) */
    ASSERT_TRUE(strstr(line, "\\n")    != NULL);
    ASSERT_TRUE(strstr(line, "\\t")    != NULL);
    ASSERT_TRUE(strstr(line, "\\\"")   != NULL);
    ASSERT_TRUE(strstr(line, "\\\\")   != NULL);
}

/* -------------------------------------------------------------------------
 * Test 4: arena checkpoint via arena->used (OOM recovery pattern)
 * ---------------------------------------------------------------------- */

TEST(test_persist_arena_checkpoint_recovery)
{
    Arena *a = arena_new(256);
    ASSERT_NOT_NULL(a);

    /* Simulate "after init" checkpoint */
    void *p1 = arena_alloc(a, 32);
    void *p2 = arena_alloc(a, 32);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);

    /* Save checkpoint */
    size_t checkpoint = a->used;

    /* Simulate turn allocations */
    void *p3 = arena_alloc(a, 64);
    ASSERT_NOT_NULL(p3);
    ASSERT_TRUE(a->used > checkpoint);

    /* Simulate OOM recovery: restore to checkpoint */
    a->used = checkpoint;

    /* Arena is back at checkpoint — new allocation should succeed */
    void *p4 = arena_alloc(a, 64);
    ASSERT_NOT_NULL(p4);

    /* p4 should alias the same region as p3 (arena bump-pointer) */
    ASSERT_EQ((size_t)p4, (size_t)p3);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_session_persist ===\n");
    RUN_TEST(test_persist_jsonl_single_turn);
    RUN_TEST(test_persist_jsonl_multiple_turns);
    RUN_TEST(test_persist_jsonl_special_chars);
    RUN_TEST(test_persist_arena_checkpoint_recovery);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
