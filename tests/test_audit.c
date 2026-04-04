/*
 * test_audit.c — unit tests for the JSONL audit log (CMP-210)
 *
 * Acceptance criteria:
 *   - audit_open creates file
 *   - tool call writes valid JSONL line
 *   - sandbox deny writes warn level
 *   - rotation renames file at max_size
 *   - null safety (NULL log is a no-op)
 *   - stdout output mode
 */

#include "test.h"
#include "../include/audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static void file_remove(const char *path)
{
    unlink(path);
}

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static size_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    return (size_t)st.st_size;
}

/* Read first cap-1 bytes of path into buf; NUL-terminate. Returns bytes read. */
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
 * Tests: open / close
 * ------------------------------------------------------------------------ */

TEST(test_audit_open_creates_file)
{
    const char *path = "/tmp/test_audit_open.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);
    ASSERT_TRUE(file_exists(path));
    audit_close(al);
    file_remove(path);
}

TEST(test_audit_close_null_is_noop)
{
    /* Must not crash */
    audit_close(NULL);
}

TEST(test_audit_open_null_path_returns_null)
{
    AuditLog *al = audit_open(NULL, 0, 0);
    ASSERT_NULL(al);
}

/* --------------------------------------------------------------------------
 * Tests: tool_call writes valid JSONL line
 * ------------------------------------------------------------------------ */

TEST(test_audit_tool_call_writes_jsonl)
{
    const char *path = "/tmp/test_audit_tool_call.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);

    audit_tool_call(al, "bash", "{\"command\":\"ls\"}",
                    "[\"file1\",\"file2\"]",
                    /*result_ok=*/1, /*duration_ms=*/42,
                    "sess-1", "strict", "/home");
    audit_close(al);

    char buf[2048];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    /* Must contain mandatory fields */
    ASSERT_TRUE(strstr(buf, "\"event\":\"tool_call\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"level\":\"info\"")      != NULL);
    ASSERT_TRUE(strstr(buf, "\"tool\":\"bash\"")       != NULL);
    ASSERT_TRUE(strstr(buf, "\"result_ok\":1")         != NULL);
    ASSERT_TRUE(strstr(buf, "\"duration_ms\":42")      != NULL);
    ASSERT_TRUE(strstr(buf, "\"ts\":")                 != NULL);

    /* Optional fields present when provided */
    ASSERT_TRUE(strstr(buf, "\"sandbox\":\"strict\"")  != NULL);
    ASSERT_TRUE(strstr(buf, "\"session_id\":\"sess-1\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"cwd\":\"/home\"")       != NULL);

    /* Line must end with newline */
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

TEST(test_audit_tool_call_result_size)
{
    const char *path = "/tmp/test_audit_result_size.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);

    const char *result = "hello world"; /* 11 chars */
    audit_tool_call(al, "grep", "{}", result, 1, 0, NULL, NULL, NULL);
    audit_close(al);

    char buf[1024];
    file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "\"result_size\":11") != NULL);

    file_remove(path);
}

TEST(test_audit_tool_call_omits_null_optional_fields)
{
    const char *path = "/tmp/test_audit_null_optional.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);

    /* All optional fields NULL — must not appear in output */
    audit_tool_call(al, "read_file", "{}", "", 1, 5,
                    NULL, NULL, NULL);
    audit_close(al);

    char buf[1024];
    file_read(path, buf, sizeof(buf));
    ASSERT_FALSE(strstr(buf, "\"sandbox\"") != NULL);
    ASSERT_FALSE(strstr(buf, "\"cwd\"")     != NULL);
    ASSERT_FALSE(strstr(buf, "\"session_id\"") != NULL);

    file_remove(path);
}

/* --------------------------------------------------------------------------
 * Tests: sandbox deny writes warn level
 * ------------------------------------------------------------------------ */

TEST(test_audit_sandbox_deny_writes_warn)
{
    const char *path = "/tmp/test_audit_sandbox_deny.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);

    audit_sandbox_deny(al, "/etc/passwd", "sess-2", "strict");
    audit_close(al);

    char buf[1024];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    ASSERT_TRUE(strstr(buf, "\"level\":\"warn\"")          != NULL);
    ASSERT_TRUE(strstr(buf, "\"event\":\"sandbox_deny\"")  != NULL);
    ASSERT_TRUE(strstr(buf, "\"denied\":\"/etc/passwd\"")  != NULL);
    ASSERT_TRUE(strstr(buf, "\"sandbox\":\"strict\"")      != NULL);
    ASSERT_TRUE(strstr(buf, "\"session_id\":\"sess-2\"")   != NULL);
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

/* --------------------------------------------------------------------------
 * Tests: null safety — NULL log is always a no-op
 * ------------------------------------------------------------------------ */

TEST(test_audit_null_log_is_noop)
{
    /* None of these must crash */
    audit_tool_call(NULL, "bash", "{}", "out", 1, 0, NULL, NULL, NULL);
    audit_sandbox_deny(NULL, "/etc/passwd", NULL, NULL);
}

/* --------------------------------------------------------------------------
 * Tests: args truncation
 * ------------------------------------------------------------------------ */

TEST(test_audit_args_truncation)
{
    const char *path = "/tmp/test_audit_args_trunc.log";
    file_remove(path);

    AuditLog *al = audit_open(path, 0, 0);
    ASSERT_NOT_NULL(al);

    /* Build args_json longer than 2048 chars */
    char big_args[4096];
    memset(big_args, 'x', sizeof(big_args) - 1);
    big_args[0] = '"';
    big_args[sizeof(big_args) - 2] = '"';
    big_args[sizeof(big_args) - 1] = '\0';

    audit_tool_call(al, "bash", big_args, "", 1, 0, NULL, NULL, NULL);
    audit_close(al);

    /* File must exist and line must be terminated with newline */
    char buf[8192];
    size_t n = file_read(path, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(buf[n - 1] == '\n');

    file_remove(path);
}

/* --------------------------------------------------------------------------
 * Tests: log rotation at max_size
 * ------------------------------------------------------------------------ */

TEST(test_audit_rotation_renames_file)
{
    const char *path = "/tmp/test_audit_rotate.log";
    const char *bak  = "/tmp/test_audit_rotate.log.1";
    file_remove(path);
    file_remove(bak);

    /* max_size=64 bytes, max_files=2: rotation triggers quickly */
    AuditLog *al = audit_open(path, 64, 2);
    ASSERT_NOT_NULL(al);

    for (int i = 0; i < 10; i++)
        audit_sandbox_deny(al, "/etc/passwd", "s", "strict");

    audit_close(al);

    /* Backup file must exist after rotation */
    ASSERT_TRUE(file_exists(bak));

    file_remove(path);
    file_remove(bak);
}

TEST(test_audit_rotation_primary_stays_small)
{
    const char *path = "/tmp/test_audit_rotate2.log";
    const char *bak  = "/tmp/test_audit_rotate2.log.1";
    file_remove(path);
    file_remove(bak);

    AuditLog *al = audit_open(path, 100, 2);
    ASSERT_NOT_NULL(al);

    /* Write enough to force multiple rotations */
    for (int i = 0; i < 30; i++)
        audit_tool_call(al, "bash", "{}", "output", 1, 1, "s", "strict", "/");

    audit_close(al);

    /* Primary file must be smaller than total data written */
    size_t sz = file_size(path);
    ASSERT_TRUE(sz < 30 * 80); /* each line ~80+ bytes */

    file_remove(path);
    file_remove(bak);
}

/* --------------------------------------------------------------------------
 * Tests: stdout output mode
 * ------------------------------------------------------------------------ */

TEST(test_audit_stdout_mode)
{
    /* audit_open_stdout must return a valid handle and not crash when used */
    AuditLog *al = audit_open_stdout();
    ASSERT_NOT_NULL(al);

    /* These must not crash (output goes to stdout — ignored in tests) */
    audit_tool_call(al, "bash", "{\"cmd\":\"ls\"}", "ok", 1, 10,
                    "sess", "permissive", "/tmp");
    audit_sandbox_deny(al, "/etc/shadow", "sess", "strict");

    audit_close(al);
}

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_audit ===\n");
    RUN_TEST(test_audit_open_creates_file);
    RUN_TEST(test_audit_close_null_is_noop);
    RUN_TEST(test_audit_open_null_path_returns_null);
    RUN_TEST(test_audit_tool_call_writes_jsonl);
    RUN_TEST(test_audit_tool_call_result_size);
    RUN_TEST(test_audit_tool_call_omits_null_optional_fields);
    RUN_TEST(test_audit_sandbox_deny_writes_warn);
    RUN_TEST(test_audit_null_log_is_noop);
    RUN_TEST(test_audit_args_truncation);
    RUN_TEST(test_audit_rotation_renames_file);
    RUN_TEST(test_audit_rotation_primary_stays_small);
    RUN_TEST(test_audit_stdout_mode);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
