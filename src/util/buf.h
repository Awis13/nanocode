/*
 * buf.h — dynamic byte buffer
 *
 * Growable byte buffer for accumulating HTTP response data, building
 * request strings, etc. Can be backed by a heap or an Arena.
 */

#ifndef BUF_H
#define BUF_H

#include <stddef.h>

typedef struct {
    char  *data;     /* pointer to buffer storage */
    size_t len;      /* bytes currently written */
    size_t cap;      /* total allocated capacity */
} Buf;

/* Initialize an empty buffer (no allocation yet). */
void   buf_init(Buf *b);

/* Pre-allocate at least `capacity` bytes of storage without changing len.
 * Avoids realloc chains when the total size is known upfront (e.g. Content-Length).
 * Returns 0 on success, -1 on OOM. */
int    buf_reserve(Buf *b, size_t capacity);

/* Append `len` bytes from `data` to the buffer. Returns 0 on success, -1 on OOM. */
int    buf_append(Buf *b, const char *data, size_t len);

/* Append a NUL-terminated string. Returns 0 on success, -1 on OOM. */
int    buf_append_str(Buf *b, const char *s);

/* Return NUL-terminated view of buffer contents (adds '\0' without changing len). */
const char *buf_str(Buf *b);

/* Reset length to zero (capacity retained). */
void   buf_reset(Buf *b);

/* Free the buffer's storage. */
void   buf_destroy(Buf *b);

#endif /* BUF_H */
