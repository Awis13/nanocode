/*
 * executor.c — tool registry, dispatch, and result serialization
 */

#include "executor.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Registry
 * ---------------------------------------------------------------------- */

typedef struct {
    const char  *name;
    const char  *schema_json;
    ToolHandler  fn;
} ToolEntry;

static ToolEntry s_registry[TOOL_REGISTRY_MAX];
static int       s_count = 0;

void tool_register(const char *name, const char *schema_json, ToolHandler fn)
{
    assert(name != NULL);
    assert(fn   != NULL);
    assert(s_count < TOOL_REGISTRY_MAX);

    s_registry[s_count].name        = name;
    s_registry[s_count].schema_json = schema_json;
    s_registry[s_count].fn          = fn;
    s_count++;
}

void tool_registry_reset(void)
{
    s_count = 0;
}

/* -------------------------------------------------------------------------
 * Dispatch
 * ---------------------------------------------------------------------- */

ToolResult tool_invoke(Arena *arena, const char *name, const char *args_json)
{
    assert(arena != NULL);
    assert(name  != NULL);

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, name) == 0)
            return s_registry[i].fn(arena, args_json ? args_json : "{}");
    }

    /* Unknown tool — return error result with descriptive message. */
    /* Format: {"error":"unknown tool","name":"<name>"} */
    size_t nlen = strlen(name);
    /* max overhead: ~40 chars of fixed JSON + name */
    size_t buflen = nlen + 48;
    char  *msg    = arena_alloc(arena, buflen);

    int written = snprintf(msg, buflen,
                           "{\"error\":\"unknown tool\",\"name\":\"%s\"}",
                           name);

    ToolResult r;
    r.error   = 1;
    r.content = msg;
    r.len     = (written > 0) ? (size_t)written : 0;
    return r;
}

/* -------------------------------------------------------------------------
 * Result serialization
 * ---------------------------------------------------------------------- */

/*
 * Write `src` into `dst` with JSON string escaping.
 * Returns number of bytes written (not counting NUL).
 * If dst is NULL or cap==0, just counts the required bytes (dry-run).
 */
static size_t json_escape(char *dst, size_t cap, const char *src, size_t srclen)
{
    size_t out = 0;

    for (size_t i = 0; i < srclen; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        char        hex[7];
        size_t      elen;

        switch (c) {
        case '"':  esc = "\\\""; elen = 2; break;
        case '\\': esc = "\\\\"; elen = 2; break;
        case '\b': esc = "\\b";  elen = 2; break;
        case '\f': esc = "\\f";  elen = 2; break;
        case '\n': esc = "\\n";  elen = 2; break;
        case '\r': esc = "\\r";  elen = 2; break;
        case '\t': esc = "\\t";  elen = 2; break;
        default:
            if (c < 0x20) {
                /* Control character — emit \uXXXX */
                snprintf(hex, sizeof(hex), "\\u%04x", (unsigned)c);
                esc  = hex;
                elen = 6;
            } else {
                esc  = NULL;
                elen = 1;
            }
            break;
        }

        if (esc) {
            if (dst && out + elen < cap) {
                memcpy(dst + out, esc, elen);
            }
            out += elen;
        } else {
            if (dst && out + 1 < cap) {
                dst[out] = (char)c;
            }
            out += 1;
        }
    }

    if (dst && out < cap)
        dst[out] = '\0';

    return out;
}

char *tool_result_to_json(Arena *arena, const char *tool_use_id,
                          const ToolResult *result)
{
    assert(arena       != NULL);
    assert(tool_use_id != NULL);
    assert(result      != NULL);

    const char *content   = result->content ? result->content : "";
    size_t      clen      = result->content ? result->len     : 0;
    size_t      id_len    = strlen(tool_use_id);

    /* Dry-run escape to get exact escaped length. */
    size_t esc_len = json_escape(NULL, 0, content, clen);

    /*
     * Template (no is_error):
     *   {"type":"tool_result","tool_use_id":"<id>","content":"<esc>"}
     * Template (with is_error):
     *   {"type":"tool_result","tool_use_id":"<id>","is_error":true,"content":"<esc>"}
     */
    const size_t overhead_ok  = 52 + 2; /* fixed chars + quotes around id/content */
    const size_t overhead_err = 67 + 2;
    size_t overhead = result->error ? overhead_err : overhead_ok;
    size_t total = overhead + id_len + esc_len + 1; /* +1 for NUL */

    char *buf = arena_alloc(arena, total);
    if (!buf)
        return NULL;

    /* Write prefix up to content value. */
    int hdr_len;
    if (result->error) {
        hdr_len = snprintf(buf, total,
            "{\"type\":\"tool_result\",\"tool_use_id\":\"%s\","
            "\"is_error\":true,\"content\":\"",
            tool_use_id);
    } else {
        hdr_len = snprintf(buf, total,
            "{\"type\":\"tool_result\",\"tool_use_id\":\"%s\","
            "\"content\":\"",
            tool_use_id);
    }

    if (hdr_len < 0)
        return NULL;

    size_t pos = (size_t)hdr_len;

    /* Write escaped content. */
    size_t written = json_escape(buf + pos, total - pos, content, clen);
    pos += written;

    /* Close the JSON object. */
    if (pos + 2 < total) {
        buf[pos]     = '"';
        buf[pos + 1] = '}';
        buf[pos + 2] = '\0';
    }

    return buf;
}
