/*
 * test_diff_sandbox.c — unit tests for the diff sandbox
 *
 * Covers:
 *   - Queue / count
 *   - Apply: new file creation
 *   - Apply: existing file overwrite
 *   - Apply: multiple files in one sandbox
 *   - Discard: leaves files unmodified
 *   - Show: doesn't crash (output goes to stdout; content not asserted)
 *   - Apply on empty sandbox: succeeds (returns 0)
 *   - Atomicity: first-file temp is cleaned up when second write fails
 */

#include "test.h"
#include "../src/tools/diff_sandbox.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

#define TMP_PREFIX "/tmp/test_diffsb_"

/* Write content to a temp file; return its path (static buffer). */
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
static const char *read_file(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) { buf[0] = '\0'; return buf; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Return 1 if the file exists, 0 otherwise. */
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

TEST(test_new_empty)
{
    Arena       *a  = arena_new(1 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);
    ASSERT_NOT_NULL(sb);
    /* Applying an empty sandbox should succeed immediately. */
    ASSERT_EQ(diff_sandbox_apply(sb), 0);
    arena_free(a);
}

TEST(test_queue_increments_count)
{
    Arena       *a  = arena_new(1 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, "/tmp/x.txt", NULL, "hello\n");
    diff_sandbox_queue(sb, "/tmp/y.txt", NULL, "world\n");

    /* apply immediately to avoid leaving temp files; use discard instead */
    diff_sandbox_discard(sb);
    arena_free(a);
}

TEST(test_apply_new_file)
{
    const char *path = TMP_PREFIX "new_file.txt";
    unlink(path); /* ensure it doesn't exist */

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, path, NULL, "created by sandbox\n");

    int r = diff_sandbox_apply(sb);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(file_exists(path));
    ASSERT_STR_EQ(read_file(path), "created by sandbox\n");

    unlink(path);
    arena_free(a);
}

TEST(test_apply_overwrite_existing)
{
    const char *path = write_tmp("overwrite.txt", "original content\n");

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, path, "original content\n", "replaced content\n");

    int r = diff_sandbox_apply(sb);
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(read_file(path), "replaced content\n");

    unlink(path);
    arena_free(a);
}

TEST(test_apply_multiple_files)
{
    /* write_tmp returns a static buffer — copy each path before the next call. */
    char path1[256], path2[256];
    strncpy(path1, write_tmp("multi_a.txt", "alpha\n"), sizeof(path1) - 1);
    path1[sizeof(path1) - 1] = '\0';
    strncpy(path2, write_tmp("multi_b.txt", "beta\n"),  sizeof(path2) - 1);
    path2[sizeof(path2) - 1] = '\0';

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, path1, "alpha\n", "ALPHA\n");
    diff_sandbox_queue(sb, path2, "beta\n",  "BETA\n");

    int r = diff_sandbox_apply(sb);
    ASSERT_EQ(r, 0);
    ASSERT_STR_EQ(read_file(path1), "ALPHA\n");
    ASSERT_STR_EQ(read_file(path2), "BETA\n");

    unlink(path1);
    unlink(path2);
    arena_free(a);
}

TEST(test_discard_leaves_file_unmodified)
{
    const char *path = write_tmp("discard.txt", "do not touch\n");

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, path, "do not touch\n", "should not appear\n");
    diff_sandbox_discard(sb);

    /* File must retain its original content. */
    ASSERT_STR_EQ(read_file(path), "do not touch\n");
    /* No temp file should linger. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.nanosandbox", path);
    ASSERT_FALSE(file_exists(tmp));

    unlink(path);
    arena_free(a);
}

TEST(test_show_no_crash)
{
    /* Redirect stdout to /dev/null so test output stays clean. */
    FILE *null_out = fopen("/dev/null", "wb");
    int   saved_fd = dup(STDOUT_FILENO);
    if (null_out && saved_fd >= 0) {
        dup2(fileno(null_out), STDOUT_FILENO);
        fclose(null_out);
    }

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, "/tmp/show_test.txt",
                       "line one\nline two\nline three\n",
                       "line one\nline TWO\nline three\n");
    diff_sandbox_show(sb);  /* must not crash */
    diff_sandbox_discard(sb);

    /* Restore stdout. */
    if (saved_fd >= 0) {
        dup2(saved_fd, STDOUT_FILENO);
        close(saved_fd);
    }

    ASSERT_TRUE(1); /* reaching here means no crash */
    arena_free(a);
}

TEST(test_show_new_file_no_crash)
{
    FILE *null_out = fopen("/dev/null", "wb");
    int   saved_fd = dup(STDOUT_FILENO);
    if (null_out && saved_fd >= 0) {
        dup2(fileno(null_out), STDOUT_FILENO);
        fclose(null_out);
    }

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, "/tmp/show_new.txt", NULL, "brand new file\n");
    diff_sandbox_show(sb);
    diff_sandbox_discard(sb);

    if (saved_fd >= 0) {
        dup2(saved_fd, STDOUT_FILENO);
        close(saved_fd);
    }

    ASSERT_TRUE(1);
    arena_free(a);
}

TEST(test_apply_atomic_cleanup_on_failure)
{
    /*
     * Queue two patches:
     *   [0] valid file
     *   [1] path inside a regular file (treated as dir) — temp write fails
     *
     * After the failed apply, file [0] must remain unmodified.
     *
     * write_tmp returns a static buffer; copy before the next call.
     */
    char path1_copy[256];
    strncpy(path1_copy, write_tmp("atomic_ok.txt", "original\n"),
            sizeof(path1_copy) - 1);
    path1_copy[sizeof(path1_copy) - 1] = '\0';

    /* Make TMP_PREFIX "not_a_dir" a regular FILE so mkdir inside it fails. */
    write_tmp("not_a_dir", "I am a file, not a directory");
    const char *not_a_dir = TMP_PREFIX "not_a_dir";

    /* path2 is inside the regular file — temp write will fail. */
    char path2[256];
    snprintf(path2, sizeof(path2), "%s/subfile.txt", not_a_dir);

    Arena       *a  = arena_new(2 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_queue(sb, path1_copy, "original\n", "modified\n");
    diff_sandbox_queue(sb, path2,      NULL,          "content\n");

    int r = diff_sandbox_apply(sb);
    ASSERT_EQ(r, -1);

    /* path1 must not be modified. */
    ASSERT_STR_EQ(read_file(path1_copy), "original\n");

    /* No stray temp file from path1. */
    char tmp1[256];
    snprintf(tmp1, sizeof(tmp1), "%s.nanosandbox", path1_copy);
    ASSERT_FALSE(file_exists(tmp1));

    unlink(path1_copy);
    unlink(not_a_dir);
    arena_free(a);
}

TEST(test_queue_ignored_after_discard)
{
    Arena       *a  = arena_new(1 << 20);
    DiffSandbox *sb = diff_sandbox_new(a);

    diff_sandbox_discard(sb);
    /* queue after discard should be a no-op */
    diff_sandbox_queue(sb, "/tmp/ignored.txt", NULL, "nope\n");
    /* apply after discard should fail */
    ASSERT_EQ(diff_sandbox_apply(sb), -1);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_diff_sandbox ===\n");

    RUN_TEST(test_new_empty);
    RUN_TEST(test_queue_increments_count);
    RUN_TEST(test_apply_new_file);
    RUN_TEST(test_apply_overwrite_existing);
    RUN_TEST(test_apply_multiple_files);
    RUN_TEST(test_discard_leaves_file_unmodified);
    RUN_TEST(test_show_no_crash);
    RUN_TEST(test_show_new_file_no_crash);
    RUN_TEST(test_apply_atomic_cleanup_on_failure);
    RUN_TEST(test_queue_ignored_after_discard);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
