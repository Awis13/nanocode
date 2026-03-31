/*
 * json.c — jsmn-based JSON extraction helpers
 */

#define JSMN_STATIC
#include "jsmn.h"
#include "json.h"

#include <string.h>
#include <assert.h>

/* Verify our size assumption. jsmntok_t = {int type, int start, int end, int size} */
typedef char _assert_tok_size[sizeof(jsmntok_t) <= 16 ? 1 : -1];

static jsmntok_t *ctx_tokens(JsonCtx *ctx)
{
    return (jsmntok_t *)(void *)ctx->_tok;
}

static const jsmntok_t *ctx_tokens_c(const JsonCtx *ctx)
{
    return (const jsmntok_t *)(const void *)ctx->_tok;
}

int json_parse_ctx(JsonCtx *ctx, const char *json, size_t json_len)
{
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, json_len,
                       ctx_tokens(ctx), JSON_TOKEN_MAX);
    ctx->ntok = (r > 0) ? r : 0;
    return (r < 0) ? -1 : r;
}

/* Return 1 if token `i` matches string `key`. */
static int tok_eq(const jsmntok_t *t, int i, const char *json, const char *key)
{
    size_t klen = strlen(key);
    int    tlen = t[i].end - t[i].start;
    return t[i].type == JSMN_STRING
        && (int)klen == tlen
        && memcmp(json + t[i].start, key, klen) == 0;
}

/* Copy token `i` value into out_buf. Returns 0 on success, -1 if too small. */
static int tok_copy(const jsmntok_t *t, int i, const char *json,
                    char *out_buf, size_t out_cap)
{
    int len = t[i].end - t[i].start;
    if ((size_t)(len + 1) > out_cap)
        return -1;
    memcpy(out_buf, json + t[i].start, (size_t)len);
    out_buf[len] = '\0';
    return 0;
}

int json_get_str(const JsonCtx *ctx, const char *json, const char *key,
                 char *out_buf, size_t out_cap)
{
    const jsmntok_t *t = ctx_tokens_c(ctx);
    int ntok = ctx->ntok;
    if (ntok < 1 || t[0].type != JSMN_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (tok_eq(t, i, json, key) && t[i + 1].type == JSMN_STRING)
            return tok_copy(t, i + 1, json, out_buf, out_cap);
    }
    return -1;
}

int json_get_nested_str(const JsonCtx *ctx, const char *json,
                        const char *parent_key, const char *child_key,
                        char *out_buf, size_t out_cap)
{
    const jsmntok_t *t = ctx_tokens_c(ctx);
    int ntok = ctx->ntok;
    if (ntok < 1 || t[0].type != JSMN_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (tok_eq(t, i, json, parent_key) && t[i + 1].type == JSMN_OBJECT) {
            int obj_size = t[i + 1].size;
            int j = i + 2;
            for (int k = 0; k < obj_size && j + 1 < ntok; k++, j += 2) {
                if (tok_eq(t, j, json, child_key) &&
                    t[j + 1].type == JSMN_STRING)
                    return tok_copy(t, j + 1, json, out_buf, out_cap);
            }
        }
    }
    return -1;
}

int json_get_array_item_str(const JsonCtx *ctx, const char *json,
                            const char *array_key, const char *field,
                            char *out_buf, size_t out_cap)
{
    const jsmntok_t *t = ctx_tokens_c(ctx);
    int ntok = ctx->ntok;
    if (ntok < 1 || t[0].type != JSMN_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (tok_eq(t, i, json, array_key) && t[i + 1].type == JSMN_ARRAY) {
            if (t[i + 1].size < 1)
                return -1;
            int elem = i + 2;
            if (elem >= ntok || t[elem].type != JSMN_OBJECT)
                return -1;
            int obj_size = t[elem].size;
            int j = elem + 1;
            for (int k = 0; k < obj_size && j + 1 < ntok; k++, j += 2) {
                if (tok_eq(t, j, json, field) &&
                    t[j + 1].type == JSMN_STRING)
                    return tok_copy(t, j + 1, json, out_buf, out_cap);
            }
        }
    }
    return -1;
}

int json_get_array_item_nested_str(const JsonCtx *ctx, const char *json,
                                   const char *array_key,
                                   const char *obj_key, const char *field,
                                   char *out_buf, size_t out_cap)
{
    const jsmntok_t *t = ctx_tokens_c(ctx);
    int ntok = ctx->ntok;
    if (ntok < 1 || t[0].type != JSMN_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (tok_eq(t, i, json, array_key) && t[i + 1].type == JSMN_ARRAY) {
            if (t[i + 1].size < 1)
                return -1;
            int elem = i + 2;
            if (elem >= ntok || t[elem].type != JSMN_OBJECT)
                return -1;
            int obj_size = t[elem].size;
            int j = elem + 1;
            for (int k = 0; k < obj_size && j + 1 < ntok; k++) {
                if (tok_eq(t, j, json, obj_key) &&
                    t[j + 1].type == JSMN_OBJECT) {
                    /* recurse into nested obj */
                    int inner_size = t[j + 1].size;
                    int m = j + 2;
                    for (int n = 0; n < inner_size && m + 1 < ntok; n++, m += 2) {
                        if (tok_eq(t, m, json, field) &&
                            t[m + 1].type == JSMN_STRING)
                            return tok_copy(t, m + 1, json, out_buf, out_cap);
                    }
                }
                /* advance past key + value (may be complex — skip by size) */
                j += 2;
            }
        }
    }
    return -1;
}
