/*
 * test_daemon.c — unit tests for the Unix socket control daemon
 *
 * CMP-216-A
 */

#include "test.h"
#include "../src/core/loop.h"
#include "../include/daemon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* Test socket path in /tmp to avoid conflicts. */
static const char *SOCK_PATH = "/tmp/nanocode_test_daemon.sock";

/* Cleanup helper. */
static void cleanup_sock(void)
{
    unlink(SOCK_PATH);
}

/* Simple dispatch callback: echoes the prompt back as the result. */
static const char *echo_dispatch(const char *prompt, const char *cwd, void *ctx)
{
    (void)cwd;
    (void)ctx;
    return prompt;
}

/* Dispatch callback that returns a very large payload. */
static const char *large_dispatch(const char *prompt, const char *cwd, void *ctx)
{
    (void)prompt;
    (void)cwd;
    (void)ctx;
    static char big[70000];
    static int init = 0;
    if (!init) {
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        init = 1;
    }
    return big;
}

static ssize_t read_all(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off + 1 < cap) {
        ssize_t n = read(fd, buf + off, cap - 1 - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    return (ssize_t)off;
}

/* -------------------------------------------------------------------------
 * Tests — null / error guards
 * ---------------------------------------------------------------------- */

TEST(test_start_null_loop_returns_null)
{
    Daemon *d = daemon_start(NULL, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NULL(d);
}

TEST(test_start_null_path_returns_null)
{
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);
    Daemon *d = daemon_start(l, NULL, echo_dispatch, NULL);
    ASSERT_NULL(d);
    loop_free(l);
}

TEST(test_start_null_dispatch_returns_null)
{
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);
    Daemon *d = daemon_start(l, SOCK_PATH, NULL, NULL);
    ASSERT_NULL(d);
    loop_free(l);
    cleanup_sock();
}

TEST(test_stop_null_is_noop)
{
    /* Must not crash. */
    daemon_stop(NULL);
}

/* -------------------------------------------------------------------------
 * Tests — lifecycle
 * ---------------------------------------------------------------------- */

TEST(test_start_creates_socket_file)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    struct stat st;
    ASSERT_EQ(stat(SOCK_PATH, &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));

    daemon_stop(d);
    loop_free(l);
    cleanup_sock();
}

TEST(test_start_socket_permissions_0600)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    struct stat st;
    ASSERT_EQ(stat(SOCK_PATH, &st), 0);
    ASSERT_EQ((int)(st.st_mode & 0777), 0600);

    daemon_stop(d);
    loop_free(l);
    cleanup_sock();
}

TEST(test_stop_removes_socket_file)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    daemon_stop(d);
    loop_free(l);

    struct stat st;
    ASSERT_NE(stat(SOCK_PATH, &st), 0); /* file must be gone */
}

/* -------------------------------------------------------------------------
 * Tests — protocol round-trip
 * ---------------------------------------------------------------------- */

TEST(test_round_trip_echo)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    /* Connect as a client. */
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(cfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    int rc = connect(cfd, (struct sockaddr *)&addr, sizeof(addr));
    ASSERT_EQ(rc, 0);

    /* Send a JSON request delimited by newline. */
    const char *req = "{\"prompt\":\"hello daemon\",\"cwd\":\"/tmp\"}\n";
    ssize_t sent = write(cfd, req, strlen(req));
    ASSERT_TRUE(sent > 0);

    /* Let the daemon accept + process: run a few loop steps. */
    for (int i = 0; i < 10; i++)
        loop_step(l, 10);

    /* Read the response. */
    char resp[1024] = {0};
    ssize_t n = read(cfd, resp, sizeof(resp) - 1);
    ASSERT_TRUE(n > 0);
    resp[n] = '\0';

    /* Response must contain status:done and the echoed prompt. */
    ASSERT_TRUE(strstr(resp, "\"status\"") != NULL);
    ASSERT_TRUE(strstr(resp, "done") != NULL);
    ASSERT_TRUE(strstr(resp, "hello daemon") != NULL);

    close(cfd);
    daemon_stop(d);
    loop_free(l);
    cleanup_sock();
}

TEST(test_missing_prompt_returns_error)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, echo_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(cfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    int rc = connect(cfd, (struct sockaddr *)&addr, sizeof(addr));
    ASSERT_EQ(rc, 0);

    /* Send a request without a "prompt" field. */
    const char *req = "{\"cwd\":\"/tmp\"}\n";
    write(cfd, req, strlen(req));

    for (int i = 0; i < 10; i++)
        loop_step(l, 10);

    char resp[1024] = {0};
    ssize_t n = read(cfd, resp, sizeof(resp) - 1);
    ASSERT_TRUE(n > 0);
    resp[n] = '\0';

    ASSERT_TRUE(strstr(resp, "error") != NULL);

    close(cfd);
    daemon_stop(d);
    loop_free(l);
    cleanup_sock();
}

TEST(test_large_response_returns_structured_error)
{
    cleanup_sock();
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    Daemon *d = daemon_start(l, SOCK_PATH, large_dispatch, NULL);
    ASSERT_NOT_NULL(d);

    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(cfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    ASSERT_EQ(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)), 0);

    const char *req = "{\"prompt\":\"large\",\"cwd\":\"/tmp\"}\n";
    ASSERT_TRUE(write(cfd, req, strlen(req)) > 0);

    for (int i = 0; i < 10; i++)
        loop_step(l, 10);

    char *resp = calloc(1, 4096);
    ASSERT_NOT_NULL(resp);
    ssize_t n = read_all(cfd, resp, 4096);
    ASSERT_TRUE(n > 0);

    ASSERT_TRUE(strstr(resp, "\"status\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"error\"") != NULL);
    ASSERT_TRUE(strstr(resp, "response truncated") != NULL);

    free(resp);
    close(cfd);
    daemon_stop(d);
    loop_free(l);
    cleanup_sock();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_daemon ===\n");

    RUN_TEST(test_start_null_loop_returns_null);
    RUN_TEST(test_start_null_path_returns_null);
    RUN_TEST(test_start_null_dispatch_returns_null);
    RUN_TEST(test_stop_null_is_noop);
    RUN_TEST(test_start_creates_socket_file);
    RUN_TEST(test_start_socket_permissions_0600);
    RUN_TEST(test_stop_removes_socket_file);
    RUN_TEST(test_round_trip_echo);
    RUN_TEST(test_missing_prompt_returns_error);
    RUN_TEST(test_large_response_returns_structured_error);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
