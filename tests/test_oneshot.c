/*
 * test_oneshot.c — unit tests for one-shot CLI mode helpers
 *
 * Tests: flag parsing (-c, --command, --auto-apply), auto-apply predicate,
 * and exit-code logic.
 */

#include "test.h"
#include "../include/oneshot.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * oneshot_parse_arg tests
 * ---------------------------------------------------------------------- */

TEST(test_parse_short_c)
{
    char *argv[] = { "nanocode", "-c", "what is 2+2" };
    int argc = 3, idx = 1;
    OneShotFlags f = {0};
    char err[64];

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, err, sizeof(err));
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(f.enabled, 1);
    ASSERT_STR_EQ(f.command, "what is 2+2");
    ASSERT_EQ(f.auto_apply, 0);
    ASSERT_EQ(idx, 2);  /* advanced past the instruction */
}

TEST(test_parse_long_command)
{
    char *argv[] = { "nanocode", "--command", "list files" };
    int argc = 3, idx = 1;
    OneShotFlags f = {0};

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, NULL, 0);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(f.enabled, 1);
    ASSERT_STR_EQ(f.command, "list files");
    ASSERT_EQ(idx, 2);
}

TEST(test_parse_auto_apply)
{
    char *argv[] = { "nanocode", "--auto-apply" };
    int argc = 2, idx = 1;
    OneShotFlags f = {0};

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, NULL, 0);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(f.auto_apply, 1);
    ASSERT_EQ(f.enabled, 0);   /* --auto-apply alone does not enable one-shot */
    ASSERT_EQ(idx, 1);         /* no extra arg consumed */
}

TEST(test_parse_unrecognized)
{
    char *argv[] = { "nanocode", "--other-flag" };
    int argc = 2, idx = 1;
    OneShotFlags f = {0};

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, NULL, 0);
    ASSERT_EQ(rc, 0);           /* not handled — caller should continue */
    ASSERT_EQ(f.enabled, 0);
    ASSERT_EQ(idx, 1);          /* index not advanced */
}

TEST(test_parse_c_missing_arg)
{
    char *argv[] = { "nanocode", "-c" };
    int argc = 2, idx = 1;
    OneShotFlags f = {0};
    char err[128];
    err[0] = '\0';

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(err[0] != '\0');    /* error message filled */
    ASSERT_EQ(f.enabled, 0);
}

TEST(test_parse_command_missing_arg)
{
    char *argv[] = { "nanocode", "--command" };
    int argc = 2, idx = 1;
    OneShotFlags f = {0};
    char err[128];
    err[0] = '\0';

    int rc = oneshot_parse_arg(argc, argv, &idx, &f, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(err[0] != '\0');
}

TEST(test_parse_combined_flags)
{
    /* Simulate: nanocode --auto-apply -c "do something" */
    char *argv[] = { "nanocode", "--auto-apply", "-c", "do something" };
    int argc = 4;
    OneShotFlags f = {0};

    /* Parse --auto-apply at index 1 */
    int idx = 1;
    int rc = oneshot_parse_arg(argc, argv, &idx, &f, NULL, 0);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(f.auto_apply, 1);

    /* Parse -c at index 2 */
    idx = 2;
    rc = oneshot_parse_arg(argc, argv, &idx, &f, NULL, 0);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(f.enabled, 1);
    ASSERT_STR_EQ(f.command, "do something");
}

/* -------------------------------------------------------------------------
 * oneshot_should_auto_apply tests
 * ---------------------------------------------------------------------- */

TEST(test_auto_apply_off)
{
    OneShotFlags f = {0};
    ASSERT_EQ(oneshot_should_auto_apply(&f), 0);
}

TEST(test_auto_apply_on)
{
    OneShotFlags f = {0};
    f.auto_apply = 1;
    ASSERT_EQ(oneshot_should_auto_apply(&f), 1);
}

TEST(test_auto_apply_null)
{
    ASSERT_EQ(oneshot_should_auto_apply(NULL), 0);
}

/* -------------------------------------------------------------------------
 * oneshot_exit_code tests
 * ---------------------------------------------------------------------- */

TEST(test_exit_code_success)
{
    ASSERT_EQ(oneshot_exit_code(0, 0), 0);
}

TEST(test_exit_code_stream_error)
{
    ASSERT_EQ(oneshot_exit_code(1, 0), 1);
}

TEST(test_exit_code_timeout)
{
    ASSERT_EQ(oneshot_exit_code(0, 1), 1);
}

TEST(test_exit_code_both_errors)
{
    ASSERT_EQ(oneshot_exit_code(1, 1), 1);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(test_parse_short_c);
    RUN_TEST(test_parse_long_command);
    RUN_TEST(test_parse_auto_apply);
    RUN_TEST(test_parse_unrecognized);
    RUN_TEST(test_parse_c_missing_arg);
    RUN_TEST(test_parse_command_missing_arg);
    RUN_TEST(test_parse_combined_flags);
    RUN_TEST(test_auto_apply_off);
    RUN_TEST(test_auto_apply_on);
    RUN_TEST(test_auto_apply_null);
    RUN_TEST(test_exit_code_success);
    RUN_TEST(test_exit_code_stream_error);
    RUN_TEST(test_exit_code_timeout);
    RUN_TEST(test_exit_code_both_errors);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
