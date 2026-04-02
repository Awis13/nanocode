/*
 * tool_protocol.c — provider-agnostic tool call parsing and dispatch
 *
 * Parsing strategy
 * ----------------
 * Claude completed response format:
 *   {"content":[{"type":"tool_use","id":"...","name":"...","input":{...}},...]}
 *
 * OpenAI/Ollama completed response format:
 *   {"choices":[{"message":{"tool_calls":[{"id":"...","function":{"name":"...","arguments":"..."}}]}}]}
 *
 * Both parsers use jsmn directly so we can walk nested arrays.
 *
 * Schema payload format
 * ---------------------
 * Claude:  array of {name, description, input_schema} objects
 *          (the format tool_schemas_json() already emits)
 * OpenAI:  array of {type:"function", function:{...}} wrappers
 *
 * Dispatch loop
 * -------------
 * tool_dispatch_all() runs each ToolCall through the executor and adds the
 * resulting turns to the caller's Conversation so the next provider request
 * sees the complete tool exchange.
 */

#define JSMN_STATIC
#include "../../vendor/jsmn/jsmn.h"

#include "tool_protocol.h"
#include "../tools/executor.h"
#include "../util/arena.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum jsmn tokens when parsing a response body. */
#define TP_TOK_MAX  1024
/* Maximum tool calls we handle in a single response. */
#define TP_MAX_CALLS 16

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

/* Return 1 if token i is a JSMN_STRING matching key. */
static int tok_str_eq(const jsmntok_t *t, int i,
                      const char *json, const char *key)
{
    if (t[i].type != JSMN_STRING)
        return 0;
    int len = t[i].end - t[i].start;
    return len == (int)strlen(key) &&
           memcmp(json + t[i].start, key, (size_t)len) == 0;
}

/*
 * Duplicate json[t[i].start .. t[i].end) into the arena (NUL-terminated).
 * Returns NULL on allocation failure.
 */
static char *arena_dup_tok(Arena *arena, const char *json, const jsmntok_t *t, int i)
{
    int len = t[i].end - t[i].start;
    if (len < 0)
        return NULL;
    char *p = arena_alloc(arena, (size_t)(len + 1));
    if (!p)
        return NULL;
    memcpy(p, json + t[i].start, (size_t)len);
    p[len] = '\0';
    return p;
}

/*
 * Advance token index past a complete JSON value (object, array, primitive,
 * or string) starting at index i.
 * Returns the first token index after the value.
 */
static int skip_value(const jsmntok_t *t, int ntok, int i)
{
    if (i >= ntok)
        return ntok;
    int end_pos = t[i].end;
    i++;
    while (i < ntok && t[i].start < end_pos)
        i++;
    return i;
}

/*
 * Linear scan for key `key` inside the object whose root token is obj_tok.
 * `ntok` is total token count; `end_tok` is exclusive upper bound.
 *
 * Returns the value token index on success, -1 if not found.
 */
static int find_key(const jsmntok_t *t, int ntok,
                    int obj_tok, int end_tok,
                    const char *json, const char *key)
{
    if (obj_tok >= ntok || t[obj_tok].type != JSMN_OBJECT)
        return -1;

    int n = t[obj_tok].size;
    int i = obj_tok + 1;
    for (int k = 0; k < n && i + 1 < end_tok; k++) {
        if (tok_str_eq(t, i, json, key))
            return i + 1; /* value token */
        /* Skip key + value */
        i = skip_value(t, end_tok, i + 1);
    }
    return -1;
}

/*
 * Simple JSON-string unescaping.
 * src points to the raw (already unquoted) string content from a jsmn token.
 * Writes the unescaped result into an arena-allocated buffer.
 * Returns the arena-allocated string, or NULL on failure.
 */
static char *unescape_json_str(Arena *arena, const char *src, int src_len)
{
    /* Worst case: no escaping, same length + NUL. */
    char *out = arena_alloc(arena, (size_t)(src_len + 1));
    if (!out)
        return NULL;

    int w = 0;
    for (int i = 0; i < src_len; ) {
        unsigned char c = (unsigned char)src[i++];
        if (c != '\\') {
            out[w++] = (char)c;
            continue;
        }
        if (i >= src_len)
            break; /* truncated escape — ignore */
        unsigned char e = (unsigned char)src[i++];
        switch (e) {
        case '"':  out[w++] = '"';  break;
        case '\\': out[w++] = '\\'; break;
        case '/':  out[w++] = '/';  break;
        case 'n':  out[w++] = '\n'; break;
        case 'r':  out[w++] = '\r'; break;
        case 't':  out[w++] = '\t'; break;
        case 'b':  out[w++] = '\b'; break;
        case 'f':  out[w++] = '\f'; break;
        case 'u': {
            /* Handle \uXXXX — emit as UTF-8 (BMP only). */
            if (i + 4 > src_len) break;
            unsigned val = 0;
            for (int k = 0; k < 4; k++) {
                unsigned char h = (unsigned char)src[i + k];
                unsigned d;
                if      (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a') + 10u;
                else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A') + 10u;
                else { val = 0xFFFD; break; } /* replacement char */
                val = val * 16u + d;
            }
            i += 4;
            if (val < 0x80u) {
                out[w++] = (char)val;
            } else if (val < 0x800u) {
                out[w++] = (char)(0xC0u | (val >> 6));
                out[w++] = (char)(0x80u | (val & 0x3Fu));
            } else {
                out[w++] = (char)(0xE0u | (val >> 12));
                out[w++] = (char)(0x80u | ((val >> 6) & 0x3Fu));
                out[w++] = (char)(0x80u | (val & 0x3Fu));
            }
            break;
        }
        default:
            out[w++] = (char)e;
            break;
        }
    }
    out[w] = '\0';
    return out;
}

/* =========================================================================
 * Claude response parser
 *
 * Looks for: content[].{type=="tool_use", id, name, input}
 * ====================================================================== */

static int parse_claude(const char *json, size_t json_len,
                        Arena *arena, ToolCall **calls_out)
{
    jsmntok_t toks[TP_TOK_MAX];
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, json, json_len, toks, TP_TOK_MAX);
    if (ntok < 1 || toks[0].type != JSMN_OBJECT)
        return -1;

    /* Find top-level "content" array. */
    int content_val = find_key(toks, ntok, 0, ntok, json, "content");
    if (content_val < 0 || toks[content_val].type != JSMN_ARRAY)
        return 0; /* no tool calls */

    int n_blocks = toks[content_val].size;

    ToolCall *calls = arena_alloc(arena, (size_t)TP_MAX_CALLS * sizeof(ToolCall));
    if (!calls)
        return -1;

    int ncalls = 0;
    int i = content_val + 1;
    for (int bi = 0; bi < n_blocks && i < ntok && ncalls < TP_MAX_CALLS; bi++) {
        if (toks[i].type != JSMN_OBJECT) {
            i = skip_value(toks, ntok, i);
            continue;
        }
        int obj  = i;
        int oend = skip_value(toks, ntok, i);

        /* Only process tool_use blocks. */
        int type_tok = find_key(toks, ntok, obj, oend, json, "type");
        if (type_tok < 0 || !tok_str_eq(toks, type_tok, json, "tool_use")) {
            i = oend;
            continue;
        }

        int id_tok    = find_key(toks, ntok, obj, oend, json, "id");
        int name_tok  = find_key(toks, ntok, obj, oend, json, "name");
        int input_tok = find_key(toks, ntok, obj, oend, json, "input");

        if (id_tok < 0 || name_tok < 0 || input_tok < 0) {
            i = oend;
            continue;
        }

        calls[ncalls].id   = arena_dup_tok(arena, json, toks, id_tok);
        calls[ncalls].name = arena_dup_tok(arena, json, toks, name_tok);
        /* input is a JSON object — copy verbatim from the token extent. */
        calls[ncalls].input = arena_dup_tok(arena, json, toks, input_tok);

        if (calls[ncalls].id && calls[ncalls].name && calls[ncalls].input)
            ncalls++;

        i = oend;
    }

    *calls_out = calls;
    return ncalls;
}

/* =========================================================================
 * OpenAI / Ollama response parser
 *
 * Looks for:
 *   choices[0].message.tool_calls[].{id, function.{name, arguments}}
 * ====================================================================== */

static int parse_openai(const char *json, size_t json_len,
                        Arena *arena, ToolCall **calls_out)
{
    jsmntok_t toks[TP_TOK_MAX];
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, json, json_len, toks, TP_TOK_MAX);
    if (ntok < 1 || toks[0].type != JSMN_OBJECT)
        return -1;

    /* choices array */
    int choices_val = find_key(toks, ntok, 0, ntok, json, "choices");
    if (choices_val < 0 || toks[choices_val].type != JSMN_ARRAY
            || toks[choices_val].size < 1)
        return 0;

    /* choices[0] */
    int ch0      = choices_val + 1;
    int ch0_end  = skip_value(toks, ntok, ch0);
    if (toks[ch0].type != JSMN_OBJECT)
        return 0;

    /* choices[0].message */
    int msg_val = find_key(toks, ntok, ch0, ch0_end, json, "message");
    if (msg_val < 0 || toks[msg_val].type != JSMN_OBJECT)
        return 0;
    int msg_end = skip_value(toks, ntok, msg_val);

    /* choices[0].message.tool_calls */
    int tc_val = find_key(toks, ntok, msg_val, msg_end, json, "tool_calls");
    if (tc_val < 0 || toks[tc_val].type != JSMN_ARRAY
            || toks[tc_val].size < 1)
        return 0;

    int n_tc = toks[tc_val].size;

    ToolCall *calls = arena_alloc(arena, (size_t)TP_MAX_CALLS * sizeof(ToolCall));
    if (!calls)
        return -1;

    int ncalls = 0;
    int i = tc_val + 1;
    for (int ci = 0; ci < n_tc && i < ntok && ncalls < TP_MAX_CALLS; ci++) {
        if (toks[i].type != JSMN_OBJECT) {
            i = skip_value(toks, ntok, i);
            continue;
        }
        int tc_obj = i;
        int tc_end = skip_value(toks, ntok, i);

        int id_tok = find_key(toks, ntok, tc_obj, tc_end, json, "id");
        int fn_tok = find_key(toks, ntok, tc_obj, tc_end, json, "function");

        if (id_tok < 0 || fn_tok < 0 || toks[fn_tok].type != JSMN_OBJECT) {
            i = tc_end;
            continue;
        }
        int fn_end = skip_value(toks, ntok, fn_tok);

        int name_tok = find_key(toks, ntok, fn_tok, fn_end, json, "name");
        int args_tok = find_key(toks, ntok, fn_tok, fn_end, json, "arguments");

        if (name_tok < 0 || args_tok < 0) {
            i = tc_end;
            continue;
        }

        calls[ncalls].id   = arena_dup_tok(arena, json, toks, id_tok);
        calls[ncalls].name = arena_dup_tok(arena, json, toks, name_tok);

        /*
         * arguments may be:
         *   JSMN_STRING — a JSON-encoded string; unescape to get the JSON object.
         *   JSMN_OBJECT — raw JSON object; copy verbatim (Ollama may send this).
         */
        if (toks[args_tok].type == JSMN_STRING) {
            int src_len = toks[args_tok].end - toks[args_tok].start;
            calls[ncalls].input = unescape_json_str(arena,
                                                    json + toks[args_tok].start,
                                                    src_len);
        } else {
            calls[ncalls].input = arena_dup_tok(arena, json, toks, args_tok);
        }

        if (calls[ncalls].id && calls[ncalls].name && calls[ncalls].input)
            ncalls++;

        i = tc_end;
    }

    *calls_out = calls;
    return ncalls;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

int tool_parse_response(ProviderType type, const char *json, size_t json_len,
                        Arena *arena, ToolCall **calls_out)
{
    if (!json || !arena || !calls_out)
        return -1;

    *calls_out = NULL;

    if (type == PROVIDER_CLAUDE)
        return parse_claude(json, json_len, arena, calls_out);

    /* PROVIDER_OPENAI and PROVIDER_OLLAMA share the same format. */
    return parse_openai(json, json_len, arena, calls_out);
}

/* -------------------------------------------------------------------------
 * tool_build_schema_payload
 * ---------------------------------------------------------------------- */

char *tool_build_schema_payload(ProviderType type, Arena *arena)
{
    if (!arena)
        return NULL;

    if (type == PROVIDER_CLAUDE) {
        /* tool_schemas_json() already emits Claude-compatible format. */
        return tool_schemas_json(arena);
    }

    /*
     * OpenAI/Ollama format: wrap each Claude-format schema entry as:
     *   {"type":"function","function":<schema>}
     *
     * We do this by:
     *   1. Getting the raw schema array from tool_schemas_json().
     *   2. Re-parsing it with jsmn to find each top-level array element.
     *   3. Emitting it re-wrapped into a new Buf.
     */
    char *raw = tool_schemas_json(arena);
    if (!raw)
        return NULL;

    size_t raw_len = strlen(raw);

    jsmntok_t toks[TP_TOK_MAX];
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, raw, raw_len, toks, TP_TOK_MAX);
    if (ntok < 1 || toks[0].type != JSMN_ARRAY) {
        /* Fallback: return raw unchanged. */
        return raw;
    }

    Buf b;
    buf_init(&b);
    buf_append_str(&b, "[");

    int n = toks[0].size;
    int i = 1;
    for (int k = 0; k < n && i < ntok; k++) {
        if (k > 0)
            buf_append_str(&b, ",");

        int elem_end = skip_value(toks, ntok, i);
        int elem_len = toks[i].end - toks[i].start;

        buf_append_str(&b, "{\"type\":\"function\",\"function\":");
        buf_append(&b, raw + toks[i].start, (size_t)elem_len);
        buf_append_str(&b, "}");

        i = elem_end;
    }

    buf_append_str(&b, "]");

    /* Copy into arena and free the heap Buf. */
    const char *str = buf_str(&b);
    size_t slen     = b.len;
    char *result    = arena_alloc(arena, slen + 1);
    if (result)
        memcpy(result, str, slen + 1);

    buf_destroy(&b);
    return result;
}

/* -------------------------------------------------------------------------
 * tool_dispatch_all
 * ---------------------------------------------------------------------- */

int tool_dispatch_all(const ToolCall *calls, int ncalls,
                      Conversation *conv, Arena *arena)
{
    if (!calls || ncalls <= 0 || !conv || !arena)
        return 0;

    int dispatched = 0;
    for (int i = 0; i < ncalls; i++) {
        const ToolCall *tc = &calls[i];
        if (!tc->id || !tc->name || !tc->input)
            continue;

        /* 1. Record the tool_use turn (assistant). */
        conv_add_tool_use(conv, tc->id, tc->name, tc->input);

        /* 2. Dispatch through the executor. */
        ToolResult result = tool_invoke(arena, tc->name, tc->input);

        /* 3. Record the tool_result turn (user). */
        const char *content = result.content ? result.content : "";
        conv_add_tool_result(conv, tc->id, content);

        dispatched++;
    }

    return dispatched;
}
