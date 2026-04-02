/*
 * executor.c — tool registry, dispatch, and result serialization
 */

#define JSMN_STATIC
#include "jsmn.h"

#include "executor.h"
#include "../../include/status_file.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

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
static ExecMode  s_exec_mode = EXEC_MODE_NORMAL;

/* -------------------------------------------------------------------------
 * Status tracker — optional, set via executor_set_status_tracker()
 * ---------------------------------------------------------------------- */

static const char *s_status_path = NULL;
static StatusInfo *s_status_info = NULL;

void executor_set_status_tracker(const char *path, void *info)
{
    s_status_path = path;
    s_status_info = (StatusInfo *)info;
}

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
    s_count    = 0;
    s_exec_mode = EXEC_MODE_NORMAL;
}

void executor_set_mode(ExecMode mode)
{
    s_exec_mode = mode;
}

ExecMode executor_get_mode(void)
{
    return s_exec_mode;
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

    /* Dry-run mode: skip execution, return synthetic result. */
    if (s_exec_mode == EXEC_MODE_DRY_RUN) {
        fprintf(stderr, "[dry-run] would call: %s(%s)\n",
                name, args_json ? args_json : "{}");
        static const char msg[] = "{\"dry_run\":true}";
        size_t mlen = sizeof(msg) - 1;
        char  *buf  = arena_alloc(arena, mlen + 1);
        if (buf) memcpy(buf, msg, mlen + 1);
        ToolResult r = { .error = 0, .content = buf, .len = mlen };
        return r;
    }

    /* Readonly mode: block write/execute tools. */
    if (s_exec_mode == EXEC_MODE_READONLY) {
        static const char *const s_ro_blocked[] = {
            "bash", "write_file", "edit_file", NULL
        };
        for (int j = 0; s_ro_blocked[j]; j++) {
            if (strcmp(name, s_ro_blocked[j]) == 0) {
                static const char msg[] =
                    "{\"error\":\"blocked in readonly mode\"}";
                size_t mlen = sizeof(msg) - 1;
                char  *buf  = arena_alloc(arena, mlen + 1);
                if (buf) memcpy(buf, msg, mlen + 1);
                ToolResult r = { .error = 1, .content = buf, .len = mlen };
                return r;
            }
        }
    }

    /* Plan mode: block write/execute tools. */
    if (s_exec_mode == EXEC_MODE_PLAN) {
        static const char *const s_blocked[] = {
            "bash", "write_file", "edit_file", NULL
        };
        for (int j = 0; s_blocked[j]; j++) {
            if (strcmp(name, s_blocked[j]) == 0) {
                static const char msg[] =
                    "{\"error\":\"tool blocked in plan mode\","
                    "\"hint\":\"use /plan to toggle plan mode off\"}";
                size_t mlen = sizeof(msg) - 1;
                char  *buf  = arena_alloc(arena, mlen + 1);
                if (buf) memcpy(buf, msg, mlen + 1);
                ToolResult r = { .error = 1, .content = buf, .len = mlen };
                return r;
            }
        }
    }

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, name) == 0) {
            ToolResult r = s_registry[i].fn(arena, args_json ? args_json : "{}");
            if (s_status_path && s_status_info) {
                s_status_info->last_action = (char *)(uintptr_t)name;
                s_status_info->tool_calls++;
                status_file_write(s_status_path, s_status_info);
            }
            return r;
        }
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

/* -------------------------------------------------------------------------
 * tool_search meta-tool
 * ---------------------------------------------------------------------- */

/*
 * Parse the "name" string field from a JSON object like {"name":"<value>"}.
 * Writes at most (cap-1) bytes plus NUL into out.
 * Returns 0 on success, -1 if the field is absent or the value is too long.
 */
static int get_name_arg(const char *args_json, char *out, size_t cap)
{
    jsmn_parser p;
    jsmntok_t   toks[32];

    jsmn_init(&p);
    int ntok = jsmn_parse(&p, args_json, strlen(args_json), toks, 32);
    if (ntok < 1) return -1;

    /* Walk key-value pairs in the top-level object. */
    for (int i = 1; i + 1 < ntok; i += 2) {
        const jsmntok_t *key = &toks[i];
        const jsmntok_t *val = &toks[i + 1];
        if (key->type != JSMN_STRING) continue;
        int klen = key->end - key->start;
        if (klen != 4 || memcmp(args_json + key->start, "name", 4) != 0)
            continue;
        int vlen = val->end - val->start;
        if (vlen < 0 || (size_t)vlen >= cap) return -1;
        memcpy(out, args_json + val->start, (size_t)vlen);
        out[vlen] = '\0';
        return 0;
    }
    return -1;
}

static ToolResult tool_search_handler(Arena *arena, const char *args_json)
{
    char name[128];
    if (get_name_arg(args_json, name, sizeof(name)) != 0) {
        static const char msg[] = "{\"error\":\"missing or invalid \\\"name\\\" argument\"}";
        size_t mlen = sizeof(msg) - 1;
        char  *buf  = arena_alloc(arena, mlen + 1);
        if (buf) memcpy(buf, msg, mlen + 1);
        ToolResult r = { .error = 1, .content = buf, .len = mlen };
        return r;
    }

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, name) == 0) {
            const char *schema = s_registry[i].schema_json
                                 ? s_registry[i].schema_json : "{}";
            size_t slen = strlen(schema);
            char  *buf  = arena_alloc(arena, slen + 1);
            if (buf) memcpy(buf, schema, slen + 1);
            ToolResult r = { .error = 0, .content = buf, .len = slen };
            return r;
        }
    }

    /* Not found — build error message. */
    size_t nlen    = strlen(name);
    size_t esc_len = json_escape(NULL, 0, name, nlen);
    /* {"error":"tool not found","name":""} = 38 chars overhead + esc + NUL */
    size_t buflen  = esc_len + 40;
    char  *msg     = arena_alloc(arena, buflen);

    const char *prefix = "{\"error\":\"tool not found\",\"name\":\"";
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

    ToolResult r = { .error = 1, .content = msg, .len = pos };
    return r;
}

static const char s_tool_search_schema[] =
    "{"
    "\"name\":\"tool_search\","
    "\"description\":\"Fetch the full JSON schema for a registered tool by name.\","
    "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Tool name to look up\"}"
        "},"
        "\"required\":[\"name\"]"
    "}"
    "}";

void tool_search_register(void)
{
    tool_register("tool_search", s_tool_search_schema, tool_search_handler);
}

/* -------------------------------------------------------------------------
 * tool_names_json / tool_schemas_json
 * ---------------------------------------------------------------------- */

char *tool_names_json(Arena *arena)
{
    assert(arena != NULL);

    /* Compute required buffer size. */
    /* Format: [{"name":"t1"},{"name":"t2"},...] */
    /* Each entry: {"name":"<name>"} = 11 + len(name) chars, plus comma. */
    /* Bracket pair: 2.  Final NUL: 1. */
    size_t total = 3; /* "[]" + NUL */
    int    count = 0;

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, "tool_search") == 0) continue;
        /* {"name":"<name>"} = 11 + strlen(name), plus comma separator */
        total += 11 + strlen(s_registry[i].name) + 1; /* +1 for comma */
        count++;
    }

    char *buf = arena_alloc(arena, total);
    if (!buf) return NULL;

    char *p = buf;
    *p++ = '[';

    int written = 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, "tool_search") == 0) continue;
        if (written > 0) *p++ = ',';
        size_t nlen = strlen(s_registry[i].name);
        memcpy(p, "{\"name\":\"", 9); p += 9;
        memcpy(p, s_registry[i].name, nlen); p += nlen;
        memcpy(p, "\"}", 2); p += 2;
        written++;
    }

    *p++ = ']';
    *p   = '\0';

    (void)count;
    return buf;
}

char *tool_schemas_json(Arena *arena)
{
    assert(arena != NULL);

    /* Compute required buffer size. */
    /* Format: [<schema1>,<schema2>,...] */
    size_t total = 3; /* "[]" + NUL */

    for (int i = 0; i < s_count; i++) {
        const char *schema = s_registry[i].schema_json
                             ? s_registry[i].schema_json : "{}";
        total += strlen(schema) + 1; /* +1 for comma */
    }

    char *buf = arena_alloc(arena, total);
    if (!buf) return NULL;

    char *p = buf;
    *p++ = '[';

    for (int i = 0; i < s_count; i++) {
        if (i > 0) *p++ = ',';
        const char *schema = s_registry[i].schema_json
                             ? s_registry[i].schema_json : "{}";
        size_t slen = strlen(schema);
        memcpy(p, schema, slen);
        p += slen;
    }

    *p++ = ']';
    *p   = '\0';

    return buf;
}
