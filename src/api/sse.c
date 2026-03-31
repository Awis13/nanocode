/*
 * sse.c — SSE stream parser implementation
 *
 * We maintain a line buffer and scan for complete events (terminated by
 * a blank line). Within each event we look for "data: " prefix lines and
 * deliver the payload.
 */

#include "sse.h"

#include <stdlib.h>
#include <string.h>

#define SSE_BUF_INIT  4096
#define SSE_BUF_MAX   (1024 * 1024)   /* 1 MB safety cap */

struct SseParser {
    char        *buf;       /* accumulation buffer */
    size_t       len;       /* bytes currently buffered */
    size_t       cap;       /* buffer capacity */
    sse_event_cb on_event;
    void        *ctx;
};

SseParser *sse_parser_new(sse_event_cb on_event, void *ctx)
{
    SseParser *p = malloc(sizeof(SseParser));
    if (!p)
        return NULL;

    p->buf = malloc(SSE_BUF_INIT);
    if (!p->buf) {
        free(p);
        return NULL;
    }
    p->len      = 0;
    p->cap      = SSE_BUF_INIT;
    p->on_event = on_event;
    p->ctx      = ctx;
    return p;
}

void sse_parser_free(SseParser *p)
{
    free(p->buf);
    free(p);
}

void sse_parser_reset(SseParser *p)
{
    p->len = 0;
}

static int buf_grow(SseParser *p, size_t need)
{
    if (p->cap >= need)
        return 0;
    if (need > SSE_BUF_MAX)
        return -1;
    size_t new_cap = p->cap;
    while (new_cap < need)
        new_cap *= 2;
    char *np = realloc(p->buf, new_cap);
    if (!np)
        return -1;
    p->buf = np;
    p->cap = new_cap;
    return 0;
}

/*
 * Dispatch one complete SSE event block.
 * `block` points to the raw bytes, `block_len` bytes long.
 * We scan for "data: " lines and fire the callback.
 */
static int dispatch_event(SseParser *p, const char *block, size_t block_len)
{
    const char *pos = block;
    const char *end = block + block_len;

    while (pos < end) {
        /* Find end of this line */
        const char *eol = memchr(pos, '\n', (size_t)(end - pos));
        size_t line_len;
        if (eol) {
            line_len = (size_t)(eol - pos);
        } else {
            line_len = (size_t)(end - pos);
            eol = end - 1;
        }

        /* Strip trailing \r */
        if (line_len > 0 && pos[line_len - 1] == '\r')
            line_len--;

        /* Check for "data: " prefix */
        if (line_len >= 6 && memcmp(pos, "data: ", 6) == 0) {
            const char *data = pos + 6;
            size_t data_len  = line_len - 6;

            /* Skip [DONE] sentinel */
            if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0) {
                pos = eol + 1;
                continue;
            }

            int r = p->on_event(data, data_len, p->ctx);
            if (r < 0)
                return -1;
        }

        pos = eol + 1;
    }
    return 0;
}

int sse_parser_feed(SseParser *p, const char *chunk, size_t len)
{
    /* Append chunk to buffer */
    if (buf_grow(p, p->len + len) < 0)
        return -1;
    memcpy(p->buf + p->len, chunk, len);
    p->len += len;

    /* Process complete events (terminated by \n\n or \r\n\r\n) */
    size_t consumed = 0;
    while (consumed < p->len) {
        const char *search = p->buf + consumed;
        size_t remaining   = p->len - consumed;

        /* Look for double-newline event boundary */
        const char *boundary = NULL;
        size_t      skip     = 0;

        for (size_t i = 0; i + 1 < remaining; i++) {
            if (search[i] == '\n' && search[i+1] == '\n') {
                boundary = search + i + 2;
                skip = 0;
                break;
            }
            if (i + 3 < remaining &&
                search[i]   == '\r' && search[i+1] == '\n' &&
                search[i+2] == '\r' && search[i+3] == '\n') {
                boundary = search + i + 4;
                skip = 0;
                break;
            }
        }

        if (!boundary)
            break; /* incomplete event — wait for more data */

        size_t event_len = (size_t)(boundary - search) - 2 - skip;
        /* event_len calculation: boundary points past the \n\n, so the
           event block is from `search` to `boundary - 2`. */
        size_t block_len = (size_t)(boundary - search);
        /* Strip the trailing newlines from the block before dispatch */
        size_t blen = block_len;
        while (blen > 0 && (search[blen-1] == '\n' || search[blen-1] == '\r'))
            blen--;

        (void)event_len; /* suppress unused warning */

        if (blen > 0) {
            int r = dispatch_event(p, search, blen);
            if (r < 0)
                return -1;
        }

        consumed += block_len;
    }

    /* Compact: move unprocessed bytes to front */
    if (consumed > 0 && consumed < p->len) {
        memmove(p->buf, p->buf + consumed, p->len - consumed);
    }
    p->len -= consumed;

    return 0;
}
