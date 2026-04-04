/*
 * retry.h — HTTP retry / exponential backoff (CMP-144)
 *           and max_output_tokens continuation retry (CMP-304)
 *
 * HTTP retry: 429 (Too Many Requests), 503 (Service Unavailable).
 * Backoff: base_delay_ms * 2^attempt, capped at max_delay_ms.
 * Retry-After header value (seconds) overrides computed delay when larger.
 *
 * Continuation retry: when Claude returns stop_reason "max_tokens" the
 * response was truncated.  The caller should append the partial assistant
 * turn and a brief continuation user message, then call provider_stream
 * again.  RETRY_MAX_CONTINUATION caps the number of such rounds.
 */

#ifndef RETRY_H
#define RETRY_H

/* Maximum rounds for max_output_tokens continuation. */
#define RETRY_MAX_CONTINUATION 3

/* Continuation user message appended when max_tokens is hit. */
#define RETRY_CONTINUATION_PROMPT "Continue your response from where you left off."

typedef struct {
    int max_retries;    /* max retry attempts (default 5) */
    int base_delay_ms;  /* starting backoff in ms (default 1000) */
    int max_delay_ms;   /* cap on delay in ms (default 60000 = 60s) */
} RetryConfig;

/* Returns a RetryConfig populated with default values. */
RetryConfig retry_config_default(void);

/*
 * Returns 1 if the HTTP status warrants a retry, 0 otherwise.
 * Retryable: 429, 503.
 */
int retry_should_retry(int http_status);

/*
 * Compute the next retry delay in milliseconds.
 *
 * attempt:       0-based retry index (0 = first retry after first failure)
 * retry_after_s: value from Retry-After response header in seconds;
 *                pass -1 if the header was absent.
 *
 * Returns the delay in ms (>= 0, <= max_delay_ms),
 * or -1 if attempt >= max_retries (caller should stop retrying).
 */
int retry_next_delay_ms(const RetryConfig *cfg, int attempt, int retry_after_s);

/*
 * Returns 1 if the provider stop_reason should trigger a continuation retry.
 * Only "max_tokens" qualifies; "end_turn", "tool_use", NULL, etc. return 0.
 */
int retry_should_continue(const char *stop_reason);

#endif /* RETRY_H */
