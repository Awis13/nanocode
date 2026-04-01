/*
 * test_retry.c — unit tests for HTTP retry / exponential backoff (CMP-144)
 *
 * Tests retry_should_retry() and retry_next_delay_ms() from src/api/retry.c.
 *
 * Contract (from CMP-144 acceptance criteria):
 *   - Retry on HTTP 429 and 503 only
 *   - Backoff sequence: 1s, 2s, 4s, 8s, 16s (5 retries max)
 *   - Delay capped at 60s
 *   - Retry-After header honored when present (capped at max_delay_ms)
 *   - Returns -1 when max_retries exhausted
 *
 * NOTE: Requires src/api/retry.c implementation (CMP-144).
 */

#include "test.h"
#include "../src/api/retry.h"

/* --------------------------------------------------------------------------
 * retry_should_retry
 * ------------------------------------------------------------------------ */

TEST(test_retry_429_is_retryable) {
    ASSERT_TRUE(retry_should_retry(429));
}

TEST(test_retry_503_is_retryable) {
    ASSERT_TRUE(retry_should_retry(503));
}

TEST(test_retry_200_not_retryable) {
    ASSERT_FALSE(retry_should_retry(200));
}

TEST(test_retry_404_not_retryable) {
    ASSERT_FALSE(retry_should_retry(404));
}

TEST(test_retry_500_not_retryable) {
    /* 500 Internal Server Error is not a transient rate-limit — don't retry */
    ASSERT_FALSE(retry_should_retry(500));
}

TEST(test_retry_connection_failure_not_retryable) {
    /* status=0 means connection failed — handled separately, not here */
    ASSERT_FALSE(retry_should_retry(0));
}

/* --------------------------------------------------------------------------
 * retry_next_delay_ms: backoff sequence
 * ------------------------------------------------------------------------ */

TEST(test_retry_backoff_attempt_0) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 0, -1), 1000);
}

TEST(test_retry_backoff_attempt_1) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 1, -1), 2000);
}

TEST(test_retry_backoff_attempt_2) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 2, -1), 4000);
}

TEST(test_retry_backoff_attempt_3) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 3, -1), 8000);
}

TEST(test_retry_backoff_attempt_4) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 4, -1), 16000);
}

TEST(test_retry_exhausted_at_max_retries) {
    RetryConfig cfg = retry_config_default();
    /* attempt 5 is the 6th try — exceeds max_retries=5 */
    ASSERT_EQ(retry_next_delay_ms(&cfg, 5, -1), -1);
}

TEST(test_retry_exhausted_well_beyond_max) {
    RetryConfig cfg = retry_config_default();
    ASSERT_EQ(retry_next_delay_ms(&cfg, 100, -1), -1);
}

TEST(test_retry_backoff_capped_at_60s) {
    RetryConfig cfg = retry_config_default();
    cfg.max_retries = 20; /* allow many retries to reach the cap */
    /* attempt 6: would be 64s — must be capped at 60000ms */
    ASSERT_EQ(retry_next_delay_ms(&cfg, 6, -1), 60000);
    /* further attempts also capped */
    ASSERT_EQ(retry_next_delay_ms(&cfg, 10, -1), 60000);
}

/* --------------------------------------------------------------------------
 * Retry-After header
 * ------------------------------------------------------------------------ */

TEST(test_retry_honors_retry_after_when_larger) {
    RetryConfig cfg = retry_config_default();
    /* attempt 0 would be 1000ms; Retry-After: 30s = 30000ms wins */
    int delay = retry_next_delay_ms(&cfg, 0, 30);
    ASSERT_TRUE(delay >= 30000);
}

TEST(test_retry_ignores_retry_after_when_smaller) {
    RetryConfig cfg = retry_config_default();
    /* attempt 4 = 16000ms; Retry-After: 5s = 5000ms — backoff wins */
    int delay = retry_next_delay_ms(&cfg, 4, 5);
    ASSERT_EQ(delay, 16000);
}

TEST(test_retry_retry_after_capped_at_max_delay) {
    RetryConfig cfg = retry_config_default();
    /* Retry-After: 120s = 120000ms — capped at max_delay_ms=60000ms */
    int delay = retry_next_delay_ms(&cfg, 0, 120);
    ASSERT_EQ(delay, 60000);
}

/* --------------------------------------------------------------------------
 * Custom config
 * ------------------------------------------------------------------------ */

TEST(test_retry_custom_config_sequence) {
    RetryConfig cfg;
    cfg.max_retries   = 3;
    cfg.base_delay_ms = 500;
    cfg.max_delay_ms  = 4000;

    ASSERT_EQ(retry_next_delay_ms(&cfg, 0, -1), 500);
    ASSERT_EQ(retry_next_delay_ms(&cfg, 1, -1), 1000);
    ASSERT_EQ(retry_next_delay_ms(&cfg, 2, -1), 2000);
    /* attempt 3 == max_retries — exhausted */
    ASSERT_EQ(retry_next_delay_ms(&cfg, 3, -1), -1);
}

TEST(test_retry_custom_config_cap) {
    RetryConfig cfg;
    cfg.max_retries   = 10;
    cfg.base_delay_ms = 1000;
    cfg.max_delay_ms  = 5000;

    /* attempt 3 = 8000ms — capped at 5000ms */
    ASSERT_EQ(retry_next_delay_ms(&cfg, 3, -1), 5000);
}

int main(void)
{
    fprintf(stderr, "=== test_retry ===\n");
    RUN_TEST(test_retry_429_is_retryable);
    RUN_TEST(test_retry_503_is_retryable);
    RUN_TEST(test_retry_200_not_retryable);
    RUN_TEST(test_retry_404_not_retryable);
    RUN_TEST(test_retry_500_not_retryable);
    RUN_TEST(test_retry_connection_failure_not_retryable);
    RUN_TEST(test_retry_backoff_attempt_0);
    RUN_TEST(test_retry_backoff_attempt_1);
    RUN_TEST(test_retry_backoff_attempt_2);
    RUN_TEST(test_retry_backoff_attempt_3);
    RUN_TEST(test_retry_backoff_attempt_4);
    RUN_TEST(test_retry_exhausted_at_max_retries);
    RUN_TEST(test_retry_exhausted_well_beyond_max);
    RUN_TEST(test_retry_backoff_capped_at_60s);
    RUN_TEST(test_retry_honors_retry_after_when_larger);
    RUN_TEST(test_retry_ignores_retry_after_when_smaller);
    RUN_TEST(test_retry_retry_after_capped_at_max_delay);
    RUN_TEST(test_retry_custom_config_sequence);
    RUN_TEST(test_retry_custom_config_cap);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
