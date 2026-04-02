/*
 * test_status_file.c — unit tests for the atomic status file writer
 *
 * CMP-216-A
 */

#include "test.h"
#include "../include/status_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Helpers ----------------------------------------------------------------- */

static const char *TMP_PATH = "/tmp/nanocode_test_status.json";

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* Tests ------------------------------------------------------------------- */

TEST(test_write_creates_valid_json_file) {
    unlink(TMP_PATH);

    StatusInfo si = {0};
    si.pid        = 1234;
    si.state      = "idle";
    si.task       = "test prompt";
    si.started_at = "2026-01-01T00:00:00Z";
    si.last_action = "bash";
    si.tool_calls  = 3;

    status_file_write(TMP_PATH, &si);

    ASSERT_TRUE(file_exists(TMP_PATH));

    char buf[4096];
    int n = read_file(TMP_PATH, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    /* Basic JSON field presence checks. */
    ASSERT_TRUE(strstr(buf, "\"pid\":")        != NULL);
    ASSERT_TRUE(strstr(buf, "\"state\":")      != NULL);
    ASSERT_TRUE(strstr(buf, "\"idle\"")        != NULL);
    ASSERT_TRUE(strstr(buf, "\"task\":")       != NULL);
    ASSERT_TRUE(strstr(buf, "\"tool_calls\":") != NULL);

    unlink(TMP_PATH);
}

TEST(test_write_is_atomic_no_tmp_after_write) {
    unlink(TMP_PATH);

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", TMP_PATH);
    unlink(tmp_path);

    StatusInfo si = {0};
    si.pid   = 42;
    si.state = "working";

    status_file_write(TMP_PATH, &si);

    /* After write, the .tmp file must not exist (was renamed). */
    ASSERT_FALSE(file_exists(tmp_path));
    /* But the real path must exist. */
    ASSERT_TRUE(file_exists(TMP_PATH));

    unlink(TMP_PATH);
}

TEST(test_remove_deletes_file) {
    /* Create the file first. */
    StatusInfo si = {0};
    si.pid   = 99;
    si.state = "idle";
    status_file_write(TMP_PATH, &si);
    ASSERT_TRUE(file_exists(TMP_PATH));

    status_file_remove(TMP_PATH);
    ASSERT_FALSE(file_exists(TMP_PATH));
}

TEST(test_null_fields_emit_null_not_crash) {
    unlink(TMP_PATH);

    StatusInfo si = {0};
    si.pid         = 5;
    si.state       = NULL;
    si.task        = NULL;
    si.started_at  = NULL;
    si.last_action = NULL;
    si.tool_calls  = 0;

    /* Must not crash. */
    status_file_write(TMP_PATH, &si);

    char buf[4096];
    int n = read_file(TMP_PATH, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    /* NULL fields should become JSON null. */
    ASSERT_TRUE(strstr(buf, "null") != NULL);

    unlink(TMP_PATH);
}

TEST(test_write_null_path_is_noop) {
    /* Must not crash. */
    StatusInfo si = {0};
    si.pid   = 1;
    si.state = "idle";
    status_file_write(NULL, &si);
    /* No assertion needed — just must not crash. */
}

TEST(test_write_null_info_is_noop) {
    status_file_write(TMP_PATH, NULL);
    ASSERT_FALSE(file_exists(TMP_PATH));
}

TEST(test_remove_null_path_is_noop) {
    /* Must not crash. */
    status_file_remove(NULL);
}

int main(void)
{
    fprintf(stderr, "=== test_status_file ===\n");

    RUN_TEST(test_write_creates_valid_json_file);
    RUN_TEST(test_write_is_atomic_no_tmp_after_write);
    RUN_TEST(test_remove_deletes_file);
    RUN_TEST(test_null_fields_emit_null_not_crash);
    RUN_TEST(test_write_null_path_is_noop);
    RUN_TEST(test_write_null_info_is_noop);
    RUN_TEST(test_remove_null_path_is_noop);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
