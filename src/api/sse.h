/*
 * sse.h — Server-Sent Events stream parser
 *
 * Zero-copy where possible: scans for event boundaries in-place and
 * slices the data: field directly from the input buffer.
 *
 * SSE format per spec (simplified for AI API usage):
 *   data: <payload>\n\n
 *
 * The [DONE] sentinel from OpenAI-compatible endpoints is handled:
 * on_event is NOT called for "data: [DONE]" events.
 */

#ifndef SSE_H
#define SSE_H

#include <stddef.h>

typedef struct SseParser SseParser;

/*
 * Callback fired for each complete SSE data event.
 * `data` and `len` describe the raw data: field content (NOT NUL-terminated).
 * Return 0 to continue, -1 to stop parsing.
 */
typedef int (*sse_event_cb)(const char *data, size_t len, void *ctx);

/* Allocate and initialise a new SSE parser. Returns NULL on OOM. */
SseParser *sse_parser_new(sse_event_cb on_event, void *ctx);

/* Free parser resources. */
void       sse_parser_free(SseParser *p);

/*
 * Feed `len` bytes from `chunk` into the parser.
 * May invoke `on_event` zero or more times.
 * Returns 0 on success, -1 if the callback signalled stop.
 */
int        sse_parser_feed(SseParser *p, const char *chunk, size_t len);

/* Reset parser state (e.g. between requests). */
void       sse_parser_reset(SseParser *p);

#endif /* SSE_H */
