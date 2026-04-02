/*
 * test_session_timeout.c — unit tests for parse_duration() (CMP-212)
 */

#include "test.h"
#include "../src/util/duration.h"

TEST(test_parse_duration_minutes)
{
    ASSERT_EQ(parse_duration("30m"), 1800);
}

TEST(test_parse_duration_hours)
{
    ASSERT_EQ(parse_duration("1h"), 3600);
}

TEST(test_parse_duration_seconds_suffix)
{
    ASSERT_EQ(parse_duration("90s"), 90);
}

TEST(test_parse_duration_bare_int)
{
    ASSERT_EQ(parse_duration("90"), 90);
}

TEST(test_parse_duration_empty)
{
    ASSERT_EQ(parse_duration(""), 0);
}

TEST(test_parse_duration_null)
{
    ASSERT_EQ(parse_duration(NULL), 0);
}

TEST(test_parse_duration_invalid)
{
    ASSERT_EQ(parse_duration("abc"), -1);
}

TEST(test_parse_duration_case_insensitive_M)
{
    ASSERT_EQ(parse_duration("30M"), 1800);
}

TEST(test_parse_duration_case_insensitive_H)
{
    ASSERT_EQ(parse_duration("2H"), 7200);
}

TEST(test_parse_duration_case_insensitive_S)
{
    ASSERT_EQ(parse_duration("10S"), 10);
}

TEST(test_parse_duration_zero)
{
    ASSERT_EQ(parse_duration("0"), 0);
}

TEST(test_parse_duration_invalid_suffix)
{
    ASSERT_EQ(parse_duration("10x"), -1);
}

TEST(test_parse_duration_trailing_garbage)
{
    ASSERT_EQ(parse_duration("10s5"), -1);
}

int main(void)
{
    RUN_TEST(test_parse_duration_minutes);
    RUN_TEST(test_parse_duration_hours);
    RUN_TEST(test_parse_duration_seconds_suffix);
    RUN_TEST(test_parse_duration_bare_int);
    RUN_TEST(test_parse_duration_empty);
    RUN_TEST(test_parse_duration_null);
    RUN_TEST(test_parse_duration_invalid);
    RUN_TEST(test_parse_duration_case_insensitive_M);
    RUN_TEST(test_parse_duration_case_insensitive_H);
    RUN_TEST(test_parse_duration_case_insensitive_S);
    RUN_TEST(test_parse_duration_zero);
    RUN_TEST(test_parse_duration_invalid_suffix);
    RUN_TEST(test_parse_duration_trailing_garbage);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
