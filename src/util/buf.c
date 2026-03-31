/*
 * buf.c — dynamic byte buffer implementation
 */

#include "buf.h"

#include <stdlib.h>
#include <string.h>

#define BUF_INITIAL_CAP 4096

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static int buf_grow(Buf *b, size_t needed)
{
    if (b->cap >= needed)
        return 0;

    size_t new_cap = b->cap ? b->cap : BUF_INITIAL_CAP;
    while (new_cap < needed)
        new_cap *= 2;

    char *p = realloc(b->data, new_cap + 1); /* +1 for NUL terminator */
    if (!p)
        return -1;

    b->data = p;
    b->cap  = new_cap;
    return 0;
}

int buf_append(Buf *b, const char *data, size_t len)
{
    if (buf_grow(b, b->len + len) != 0)
        return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int buf_append_str(Buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

const char *buf_str(Buf *b)
{
    if (buf_grow(b, b->len) != 0)
        return "";
    b->data[b->len] = '\0';
    return b->data;
}

void buf_reset(Buf *b)
{
    b->len = 0;
}

void buf_destroy(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}
