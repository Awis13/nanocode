/*
 * test_session.c — unit tests for session event log (CMP-183)
 *
 * Tests session_log_open/close, all write functions, and log rotation.
 *
 * Contract (from CMP-183 acceptance criteria):
 *   - session_log_open() creates the file if it doesn't exist
 *   - Each write function appends a valid NDJSON line
 *   - Rotation renames the current file to "<path>.1" and starts fresh
 *   - Rotation triggers when cumulative bytes_written >= max_bytes
 *   - session_log_close(NULL) is a no-op
 *   - session_log_open() primes bytes_written from existing file size
 */

#include "test.h"
#include "../src/core/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static size_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    return (size_t)st.st_size;
}

static void file_remove(const char *path)
{
    unlink(path);
}

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Read the first `cap-1` bytes of `path` into `buf`. Returns bytes read. */
static size_t file_read(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    size_t n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n;
}

/* --------------------------------------------------------------------------
 * Tests: basic open / close
 * ------------------------------------------------------------------------ */

TEST(test_session_open_creates_file)
{
    const char *path = "/tmp/test_session_open.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);
    ASSERT_TRUE(file_exists(path));
    session_log_close(sl);
    file_remove(path);
}

TEST(test_session_close_null_is_noop)
{
    /* Must not crash */
    session_log_close(NULL);
}

TEST(test_session_open_null_path_returns_null)
{
    SessionLog *sl = session_log_open(NULL, 0);
    ASSERT_NULL(sl);
}

/* --------------------------------------------------------------------------
 * Tests: write functions append NDJSON lines
 * ------------------------------------------------------------------------ */

TEST(test_session_log_start_writes_line)
{
    const char *path = "/tmp/test_session_start.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_start(sl, 42);
    session_log_close(sl);

    char buf[512];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"type\":\"start\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"pid\":42") != NULL);
    /* Line must end with newline */
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

TEST(test_session_log_child_spawn_writes_line)
{
    const char *path = "/tmp/test_session_spawn.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_child_spawn(sl, (pid_t)1234, "echo hello");
    session_log_close(sl);

    char buf[512];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"type\":\"child_spawn\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"pid\":1234") != NULL);
    ASSERT_TRUE(strstr(buf, "echo hello") != NULL);
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

TEST(test_session_log_child_reap_writes_line)
{
    const char *path = "/tmp/test_session_reap.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_child_reap(sl, (pid_t)5678, 0);
    session_log_close(sl);

    char buf[512];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"type\":\"child_reap\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"pid\":5678") != NULL);
    ASSERT_TRUE(strstr(buf, "\"exit\":0") != NULL);
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

TEST(test_session_log_event_writes_line)
{
    const char *path = "/tmp/test_session_event.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_event(sl, "shutdown", "clean exit");
    session_log_close(sl);

    char buf[512];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"type\":\"shutdown\"") != NULL);
    ASSERT_TRUE(strstr(buf, "clean exit") != NULL);
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

TEST(test_session_log_event_null_detail)
{
    const char *path = "/tmp/test_session_event_null.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    /* Must not crash with NULL detail */
    session_log_event(sl, "ping", NULL);
    session_log_close(sl);

    ASSERT_TRUE(file_exists(path));
    file_remove(path);
}

/* --------------------------------------------------------------------------
 * Tests: JSON escaping in child_spawn cmd
 * ------------------------------------------------------------------------ */

TEST(test_session_log_spawn_escapes_quotes)
{
    const char *path = "/tmp/test_session_escape.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_child_spawn(sl, (pid_t)99, "echo \"hello\"");
    session_log_close(sl);

    char buf[512];
    file_read(path, buf, sizeof(buf));
    /* Raw " in cmd must be escaped to \" in the log */
    ASSERT_TRUE(strstr(buf, "\\\"hello\\\"") != NULL);

    file_remove(path);
}

TEST(test_session_log_spawn_escapes_newline)
{
    const char *path = "/tmp/test_session_escape_nl.log";
    file_remove(path);
    SessionLog *sl = session_log_open(path, 0);
    ASSERT_NOT_NULL(sl);

    session_log_child_spawn(sl, (pid_t)99, "echo\nhello");
    session_log_close(sl);

    char buf[512];
    file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "\\n") != NULL);

    file_remove(path);
}

/* --------------------------------------------------------------------------
 * Tests: null-guard write functions
 * ------------------------------------------------------------------------ */

TEST(test_session_write_null_log_is_noop)
{
    /* None of these should crash */
    session_log_start(NULL, 1);
    session_log_child_spawn(NULL, (pid_t)1, "cmd");
    session_log_child_reap(NULL, (pid_t)1, 0);
    session_log_event(NULL, "type", "detail");
}

/* --------------------------------------------------------------------------
 * Tests: log rotation
 * ------------------------------------------------------------------------ */

TEST(test_session_rotation_renames_old_file)
{
    const char *path  = "/tmp/test_session_rotate.log";
    const char *bak   = "/tmp/test_session_rotate.log.1";
    file_remove(path);
    file_remove(bak);

    /* max_bytes = 64: will rotate after a few writes */
    SessionLog *sl = session_log_open(path, 64);
    ASSERT_NOT_NULL(sl);

    /* Write enough to trigger rotation */
    for (int i = 0; i < 10; i++)
        session_log_event(sl, "tick", "0123456789");

    session_log_close(sl);

    /* Backup file must exist after rotation */
    ASSERT_TRUE(file_exists(bak));

    file_remove(path);
    file_remove(bak);
}

TEST(test_session_rotation_fresh_file_is_smaller)
{
    const char *path = "/tmp/test_session_rotate2.log";
    const char *bak  = "/tmp/test_session_rotate2.log.1";
    file_remove(path);
    file_remove(bak);

    SessionLog *sl = session_log_open(path, 64);
    ASSERT_NOT_NULL(sl);

    for (int i = 0; i < 20; i++)
        session_log_event(sl, "tick", "0123456789abcdef");

    session_log_close(sl);

    /* The primary log must be smaller than the total data written (rotation
     * means not everything landed in the current file). */
    size_t sz = file_size(path);
    ASSERT_TRUE(sz < 20 * 40); /* 20 writes × ~40 bytes each */

    file_remove(path);
    file_remove(bak);
}

TEST(test_session_open_primes_bytes_from_existing_file)
{
    const char *path = "/tmp/test_session_prime.log";
    const char *bak  = "/tmp/test_session_prime.log.1";
    file_remove(path);
    file_remove(bak);

    /* Write some data and close */
    SessionLog *sl = session_log_open(path, 512);
    ASSERT_NOT_NULL(sl);
    session_log_event(sl, "init", "first session");
    session_log_close(sl);

    size_t first_size = file_size(path);
    ASSERT_TRUE(first_size > 0);

    /* Reopen with a tiny threshold that should trigger immediately */
    sl = session_log_open(path, first_size); /* max == current size */
    ASSERT_NOT_NULL(sl);
    session_log_event(sl, "next", "second session");
    session_log_close(sl);

    /* Backup must exist — primed bytes caused rotation on second write */
    ASSERT_TRUE(file_exists(bak));

    file_remove(path);
    file_remove(bak);
}

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_session ===\n");
    RUN_TEST(test_session_open_creates_file);
    RUN_TEST(test_session_close_null_is_noop);
    RUN_TEST(test_session_open_null_path_returns_null);
    RUN_TEST(test_session_log_start_writes_line);
    RUN_TEST(test_session_log_child_spawn_writes_line);
    RUN_TEST(test_session_log_child_reap_writes_line);
    RUN_TEST(test_session_log_event_writes_line);
    RUN_TEST(test_session_log_event_null_detail);
    RUN_TEST(test_session_log_spawn_escapes_quotes);
    RUN_TEST(test_session_log_spawn_escapes_newline);
    RUN_TEST(test_session_write_null_log_is_noop);
    RUN_TEST(test_session_rotation_renames_old_file);
    RUN_TEST(test_session_rotation_fresh_file_is_smaller);
    RUN_TEST(test_session_open_primes_bytes_from_existing_file);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
