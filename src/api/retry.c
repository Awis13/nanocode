/*
 * retry.c — HTTP retry / exponential backoff (CMP-144)
 *           and max_output_tokens continuation retry (CMP-304)
 */

#include "retry.h"
#include <string.h>

RetryConfig retry_config_default(void)
{
    RetryConfig cfg;
    cfg.max_retries   = 5;
    cfg.base_delay_ms = 1000;
    cfg.max_delay_ms  = 60000;
    return cfg;
}

int retry_should_retry(int http_status)
{
    return (http_status == 429 || http_status == 503) ? 1 : 0;
}

int retry_next_delay_ms(const RetryConfig *cfg, int attempt, int retry_after_s)
{
    if (attempt >= cfg->max_retries)
        return -1;

    /* Compute base exponential backoff: base * 2^attempt */
    long delay = cfg->base_delay_ms;
    for (int i = 0; i < attempt; i++) {
        delay *= 2;
        if (delay >= cfg->max_delay_ms) {
            delay = cfg->max_delay_ms;
            break;
        }
    }
    if (delay > cfg->max_delay_ms)
        delay = cfg->max_delay_ms;

    /* Honor Retry-After header when larger than computed backoff */
    if (retry_after_s >= 0) {
        long ra_ms = (long)retry_after_s * 1000;
        if (ra_ms > delay)
            delay = ra_ms;
        if (delay > cfg->max_delay_ms)
            delay = cfg->max_delay_ms;
    }

    return (int)delay;
}

int retry_should_continue(const char *stop_reason)
{
    if (!stop_reason)
        return 0;
    return strcmp(stop_reason, "max_tokens") == 0 ? 1 : 0;
}
