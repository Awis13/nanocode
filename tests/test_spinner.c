/*
 * test_spinner.c — unit tests for the braille breathing spinner
 */

#include "test.h"
#include "tui/spinner.h"
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

/* =========================================================================
 * Tests
 * ====================================================================== */

/* spinner_start initialises fields without writing to fd. */
TEST(test_spinner_start_no_output) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    Spinner s;
    spinner_start(&s, fds[1], 0);
    close(fds[1]);

    char buf[64];
    int n = (int)read(fds[0], buf, sizeof(buf));
    close(fds[0]);
    /* Nothing written at start time — returns -1 (EAGAIN) or 0. */
    ASSERT_TRUE(n <= 0);

    ASSERT_EQ(s.frame, 0);
    ASSERT_EQ(s.shown, 0);
}

/* spinner_stop on a never-shown spinner must not crash or write. */
TEST(test_spinner_stop_before_shown_no_output) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    Spinner s;
    spinner_start(&s, fds[1], 0);
    spinner_stop(&s);
    close(fds[1]);

    char buf[64];
    int n = (int)read(fds[0], buf, sizeof(buf));
    close(fds[0]);
    ASSERT_TRUE(n <= 0);
}

/* spinner_stop erases glyph when spinner has been shown. */
TEST(test_spinner_stop_erases_when_shown) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);

    Spinner s;
    spinner_start(&s, fds[1], 0);

    /* Force shown=1 so spinner_stop emits the erase sequence. */
    s.shown = 1;
    spinner_stop(&s);
    ASSERT_EQ(s.shown, 0);
    close(fds[1]);

    char buf[64];
    int n = (int)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    /* Erase sequence: CR + spaces + CR */
    ASSERT_TRUE(strchr(buf, '\r') != NULL);
}

/* spinner_tick on NULL must not crash. */
TEST(test_spinner_tick_null_safe) {
    spinner_tick(NULL); /* must not crash */
}

/* spinner_stop on NULL must not crash. */
TEST(test_spinner_stop_null_safe) {
    spinner_stop(NULL); /* must not crash */
}

/* spinner_stop resets frame to 0. */
TEST(test_spinner_stop_resets_frame) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    Spinner s;
    spinner_start(&s, fds[1], 0);
    s.frame = 5;
    s.shown = 1;
    spinner_stop(&s);
    close(fds[1]);
    close(fds[0]);
    ASSERT_EQ(s.frame, 0);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    RUN_TEST(test_spinner_start_no_output);
    RUN_TEST(test_spinner_stop_before_shown_no_output);
    RUN_TEST(test_spinner_stop_erases_when_shown);
    RUN_TEST(test_spinner_tick_null_safe);
    RUN_TEST(test_spinner_stop_null_safe);
    RUN_TEST(test_spinner_stop_resets_frame);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
