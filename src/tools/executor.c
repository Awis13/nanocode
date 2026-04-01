/*
 * executor.c — tool registry, dispatch, and result serialization
 */

#include "executor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declaration — json_escape is defined in the serialization section. */
static size_t json_escape(char *dst, size_t cap, const char *src, size_t srclen);

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
    if (s_count >= TOOL_REGISTRY_MAX) {
        fprintf(stderr, "tool_register: registry full (max %d)\n",
                TOOL_REGISTRY_MAX);
        abort();
    }

    s_registry[s_count].name        = name;
    s_registry[s_count].schema_json = schema_json;
    s_registry[s_count].fn          = fn;
    s_count++;
}

void tool_registry_reset(void)
{
    s_count = 0;
}

int tool_list_names(const char **names, int max_names)
{
    int fill = s_count < max_names ? s_count : max_names;
    for (int i = 0; i < fill; i++)
        names[i] = s_registry[i].name;
    return s_count;
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
    /* Format: {"error":"unknown tool","name":"<name-escaped>"} */
    size_t nlen    = strlen(name);
    size_t esc_len = json_escape(NULL, 0, name, nlen);
    /* overhead: {"error":"unknown tool","name":""} = 42 chars + 1 NUL */
    size_t buflen  = esc_len + 44;
    char  *msg     = arena_alloc(arena, buflen);

    /* Write prefix, escaped name, suffix. */
    const char *prefix = "{\"error\":\"unknown tool\",\"name\":\"";
    size_t plen = strlen(prefix);
    memcpy(msg, prefix, plen);
    size_t pos = plen;
    pos += json_escape(msg + pos, buflen - pos, name, nlen);
    if (pos + 3 <= buflen) {
        msg[pos]     = '"';
        msg[pos + 1] = '}';
        msg[pos + 2] = '\0';
        pos += 2;
    }

    ToolResult r;
    r.error   = 1;
    r.content = msg;
    r.len     = pos;
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

    /* Dry-run escapes to get exact output sizes. */
    size_t id_esclen  = json_escape(NULL, 0, tool_use_id, id_len);
    size_t esc_len    = json_escape(NULL, 0, content, clen);

    /*
     * Wire format (Claude-specific tool_result block):
     *   no error:  {"type":"tool_result","tool_use_id":"<id>","content":"<esc>"}
     *   error:     {"type":"tool_result","tool_use_id":"<id>","is_error":true,"content":"<esc>"}
     */
    static const char pfx[]     = "{\"type\":\"tool_result\",\"tool_use_id\":\"";
    static const char mid_ok[]  = "\",\"content\":\"";
    static const char mid_err[] = "\",\"is_error\":true,\"content\":\"";
    static const char sfx[]     = "\"}";

    const char *mid  = result->error ? mid_err : mid_ok;
    size_t      mlen = result->error ? (sizeof mid_err - 1) : (sizeof mid_ok - 1);
    size_t      total = (sizeof pfx - 1) + id_esclen + mlen + esc_len
                      + (sizeof sfx - 1) + 1; /* +1 NUL */

    char *buf = arena_alloc(arena, total);
    if (!buf)
        return NULL;

    size_t pos = 0;

    /* Prefix. */
    memcpy(buf + pos, pfx, sizeof pfx - 1);
    pos += sizeof pfx - 1;

    /* Escaped tool_use_id. */
    pos += json_escape(buf + pos, total - pos, tool_use_id, id_len);

    /* Middle segment (closes id, opens content). */
    memcpy(buf + pos, mid, mlen);
    pos += mlen;

    /* Escaped content. */
    pos += json_escape(buf + pos, total - pos, content, clen);

    /* Suffix. */
    memcpy(buf + pos, sfx, sizeof sfx - 1);
    pos += sizeof sfx - 1;
    buf[pos] = '\0';

    return buf;
}
