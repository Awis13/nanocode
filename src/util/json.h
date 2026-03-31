/*
 * json.h — thin jsmn wrapper for extracting string values
 *
 * Usage:
 *   JsonCtx ctx;
 *   int r = json_parse_ctx(&ctx, json, len);
 *   if (r > 0) json_get_str(&ctx, json, "key", out, sizeof(out));
 */

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

#define JSON_TOKEN_MAX 256

/*
 * Opaque parse context.
 * sizeof(jsmntok_t) == 16 on all supported platforms (4 × int).
 * Embed inline to keep it stack-allocatable without exposing jsmn types.
 */
typedef struct {
    char _tok[JSON_TOKEN_MAX * 16]; /* jsmntok_t array, type-punned */
    int  ntok;
} JsonCtx;

/*
 * Parse `json` of `json_len` bytes. Fills `ctx`.
 * Returns number of tokens (> 0) on success, -1 on parse error.
 */
int json_parse_ctx(JsonCtx *ctx, const char *json, size_t json_len);

/*
 * Find a top-level string value for `key`.
 * Writes NUL-terminated result into `out_buf` (capacity `out_cap`).
 * Returns 0 on success, -1 if not found.
 */
int json_get_str(const JsonCtx *ctx, const char *json, const char *key,
                 char *out_buf, size_t out_cap);

/*
 * Find nested string: `parent_key` → `child_key`.
 * Returns 0 on success, -1 if not found.
 */
int json_get_nested_str(const JsonCtx *ctx, const char *json,
                        const char *parent_key, const char *child_key,
                        char *out_buf, size_t out_cap);

/*
 * Find first array item's direct field: `array_key[0].field`.
 * Returns 0 on success, -1 if not found.
 */
int json_get_array_item_str(const JsonCtx *ctx, const char *json,
                            const char *array_key, const char *field,
                            char *out_buf, size_t out_cap);

/*
 * Find first array item's nested field: `array_key[0].obj_key.field`.
 * Used for OpenAI SSE "choices[0].delta.content".
 * Returns 0 on success, -1 if not found.
 */
int json_get_array_item_nested_str(const JsonCtx *ctx, const char *json,
                                   const char *array_key,
                                   const char *obj_key, const char *field,
                                   char *out_buf, size_t out_cap);

#endif /* JSON_H */
