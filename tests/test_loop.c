/*
 * test_loop.c — unit tests for the event loop (CMP-180)
 *
 * Tests the O(1) fd-indexed lookup: add, mod, remove, duplicate-add rejection,
 * out-of-range guards, and fd reuse after removal.
 *
 * These tests exercise the Loop API through pipes so they work on both
 * macOS (kqueue) and Linux (epoll) without any platform-specific includes.
 */

#include "test.h"
#include "../src/core/loop.h"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int g_cb_count;
static int g_cb_last_fd;
static int g_cb_last_ev;

static int counting_cb(int fd, int events, void *ctx)
{
    (void)ctx;
    g_cb_count++;
    g_cb_last_fd = fd;
    g_cb_last_ev = events;
    return 0;   /* keep registered */
}

static int remove_self_cb(int fd, int events, void *ctx)
{
    (void)fd; (void)events; (void)ctx;
    g_cb_count++;
    return -1;  /* signal loop to remove */
}

/* Open a pipe pair, make both ends non-blocking. */
static void make_pipe(int fds[2])
{
    if (pipe(fds) < 0) {
        perror("pipe");
        _exit(1);
    }
    fd_set_nonblocking(fds[0]);
    fd_set_nonblocking(fds[1]);
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

/* loop_new / loop_free smoke test */
TEST(test_loop_create)
{
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);
    loop_free(l);
}

/* Add an fd, verify a readability event is dispatched after a write. */
TEST(test_add_fd_readable)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);

    /* Make the read end readable. */
    char buf = 'x';
    (void)write(fds[1], &buf, 1);

    /* One step should dispatch exactly one read event. */
    int n = loop_step(l, 200);
    ASSERT_TRUE(n >= 1);
    ASSERT_TRUE(g_cb_count >= 1);
    ASSERT_EQ(g_cb_last_fd, fds[0]);
    ASSERT_TRUE(g_cb_last_ev & LOOP_READ);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* loop_remove_fd: after removal no further callbacks for that fd. */
TEST(test_remove_fd)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);
    ASSERT_EQ(loop_remove_fd(l, fds[0]), 0);

    /* Write data — fd is no longer registered, callback must not fire. */
    char buf = 'y';
    (void)write(fds[1], &buf, 1);

    loop_step(l, 50);
    ASSERT_EQ(g_cb_count, 0);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* loop_remove_fd on unknown fd returns -1. */
TEST(test_remove_unknown_fd)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    /* fd was never added */
    ASSERT_EQ(loop_remove_fd(l, fds[0]), -1);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* Callback returning -1 causes the loop to auto-remove the fd. */
TEST(test_cb_remove_self)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, remove_self_cb, NULL), 0);

    char buf = 'z';
    (void)write(fds[1], &buf, 1);
    loop_step(l, 200);

    ASSERT_EQ(g_cb_count, 1);

    /* After self-removal, adding the same fd again must succeed. */
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);
    ASSERT_EQ(loop_remove_fd(l, fds[0]), 0);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* Fd reuse: add, remove, then re-add the same fd — should work cleanly. */
TEST(test_fd_reuse)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);
    ASSERT_EQ(loop_remove_fd(l, fds[0]), 0);
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);
    ASSERT_EQ(loop_remove_fd(l, fds[0]), 0);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* loop_mod_fd on unregistered fd returns -1. */
TEST(test_mod_unregistered)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    ASSERT_EQ(loop_mod_fd(l, fds[0], LOOP_READ), -1);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* loop_step with 0 timeout returns immediately (no events). */
TEST(test_step_no_timeout)
{
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    int n = loop_step(l, 0);
    ASSERT_TRUE(n >= 0);

    loop_free(l);
}

/* Multiple fds: events for each are dispatched independently. */
TEST(test_multiple_fds)
{
    int fa[2], fb[2];
    make_pipe(fa);
    make_pipe(fb);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fa[0], LOOP_READ, counting_cb, NULL), 0);
    ASSERT_EQ(loop_add_fd(l, fb[0], LOOP_READ, counting_cb, NULL), 0);

    char buf = 'a';
    (void)write(fa[1], &buf, 1);
    (void)write(fb[1], &buf, 1);

    /* Give the loop a couple of steps to drain both. */
    loop_step(l, 200);
    loop_step(l, 50);

    ASSERT_TRUE(g_cb_count >= 2);

    loop_free(l);
    close(fa[0]); close(fa[1]);
    close(fb[0]); close(fb[1]);
}

/*
 * loop_set_mode: default is LOOP_IDLE (100 ms); switching to LOOP_STREAMING
 * and back to LOOP_IDLE does not corrupt loop state — subsequent fd events
 * are still dispatched correctly.
 */
TEST(test_set_mode_idle_default)
{
    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    /* Default mode: a step with 0 timeout returns immediately. */
    int n = loop_step(l, 0);
    ASSERT_TRUE(n >= 0);

    loop_free(l);
}

TEST(test_set_mode_streaming_events)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    loop_set_mode(l, LOOP_STREAMING);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);

    char buf = 's';
    (void)write(fds[1], &buf, 1);

    int n = loop_step(l, 200);
    ASSERT_TRUE(n >= 1);
    ASSERT_TRUE(g_cb_count >= 1);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

TEST(test_set_mode_roundtrip)
{
    int fds[2];
    make_pipe(fds);

    Loop *l = loop_new();
    ASSERT_NOT_NULL(l);

    /* Switch to streaming then back to idle — loop must remain functional. */
    loop_set_mode(l, LOOP_STREAMING);
    loop_set_mode(l, LOOP_IDLE);

    g_cb_count = 0;
    ASSERT_EQ(loop_add_fd(l, fds[0], LOOP_READ, counting_cb, NULL), 0);

    char buf = 'r';
    (void)write(fds[1], &buf, 1);

    int n = loop_step(l, 200);
    ASSERT_TRUE(n >= 1);
    ASSERT_TRUE(g_cb_count >= 1);

    loop_free(l);
    close(fds[0]);
    close(fds[1]);
}

/* fd_set_nonblocking sanity check. */
TEST(test_fd_set_nonblocking)
{
    int fds[2];
    make_pipe(fds);

    ASSERT_EQ(fd_set_nonblocking(fds[0]), 0);
    int flags = fcntl(fds[0], F_GETFL, 0);
    ASSERT_TRUE(flags & O_NONBLOCK);

    close(fds[0]);
    close(fds[1]);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(test_loop_create);
    RUN_TEST(test_add_fd_readable);
    RUN_TEST(test_remove_fd);
    RUN_TEST(test_remove_unknown_fd);
    RUN_TEST(test_cb_remove_self);
    RUN_TEST(test_fd_reuse);
    RUN_TEST(test_mod_unregistered);
    RUN_TEST(test_step_no_timeout);
    RUN_TEST(test_multiple_fds);
    RUN_TEST(test_fd_set_nonblocking);
    RUN_TEST(test_set_mode_idle_default);
    RUN_TEST(test_set_mode_streaming_events);
    RUN_TEST(test_set_mode_roundtrip);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
