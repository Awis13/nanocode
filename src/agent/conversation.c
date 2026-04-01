/*
 * conversation.c — conversation management (CMP-118)
 *
 * Arena-only data path; malloc used only in conv_save/conv_load for temporary
 * I/O buffers, freed before return.
 */

#define JSMN_STATIC
#include "../../vendor/jsmn/jsmn.h"

#include "conversation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Maximum file size accepted by conv_load (1 MB). */
#define CONV_LOAD_MAX_FILE  (1024 * 1024)

/* Maximum jsmn tokens used during conv_load. */
#define CONV_LOAD_MAX_TOKENS 4096

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* arena_strdup: duplicate a NUL-terminated string into the arena. */
static char *arena_strdup(Arena *a, const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *p = arena_alloc(a, len);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    return p;
}

/*
 * Write a JSON-escaped version of `s` to `fp`.
 * Escapes: " \ \n \r \t and control chars < 0x20.
 */
static void fwrite_json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    if (!s) {
        fputc('"', fp);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"') {
            fputs("\\\"", fp);
        } else if (*p == '\\') {
            fputs("\\\\", fp);
        } else if (*p == '\n') {
            fputs("\\n", fp);
        } else if (*p == '\r') {
            fputs("\\r", fp);
        } else if (*p == '\t') {
            fputs("\\t", fp);
        } else if (*p < 0x20) {
            fprintf(fp, "\\u00%02x", (unsigned)*p);
        } else {
            fputc((int)*p, fp);
        }
    }
    fputc('"', fp);
}

/*
 * JSON-unescape `src_len` bytes from `src` into `out` (capacity `cap`).
 * Writes a NUL terminator. Returns length written (excluding NUL),
 * or -1 on error (output too small, bad escape).
 */
static int json_unescape(const char *src, int src_len, char *out, int cap)
{
    int w = 0;
    int i = 0;
    while (i < src_len) {
        if (w >= cap - 1)
            return -1;
        unsigned char c = (unsigned char)src[i++];
        if (c != '\\') {
            out[w++] = (char)c;
            continue;
        }
        /* escape sequence */
        if (i >= src_len)
            return -1;
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
            /* Parse 4 hex digits. Only handle BMP U+0000..U+007F as a byte. */
            if (i + 4 > src_len)
                return -1;
            unsigned val = 0;
            for (int k = 0; k < 4; k++) {
                unsigned char h = (unsigned char)src[i + k];
                unsigned digit;
                if (h >= '0' && h <= '9')      digit = (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') digit = (unsigned)(h - 'a') + 10u;
                else if (h >= 'A' && h <= 'F') digit = (unsigned)(h - 'A') + 10u;
                else return -1;
                val = val * 16u + digit;
            }
            i += 4;
            /* Encode as UTF-8 (basic — only BMP needed for our use). */
            if (val < 0x80u) {
                out[w++] = (char)val;
            } else if (val < 0x800u) {
                if (w + 2 > cap - 1) return -1;
                out[w++] = (char)(0xC0u | (val >> 6));
                out[w++] = (char)(0x80u | (val & 0x3Fu));
            } else {
                if (w + 3 > cap - 1) return -1;
                out[w++] = (char)(0xE0u | (val >> 12));
                out[w++] = (char)(0x80u | ((val >> 6) & 0x3Fu));
                out[w++] = (char)(0x80u | (val & 0x3Fu));
            }
            break;
        }
        default:
            /* Unknown escape — pass through the character as-is. */
            out[w++] = (char)e;
            break;
        }
    }
    out[w] = '\0';
    return w;
}

/* -------------------------------------------------------------------------
 * UUID generation (simple v4-ish using rand())
 * ---------------------------------------------------------------------- */

static void gen_uuid(char out[37])
{
    static int initialized = 0;
    if (!initialized) {
        srand((unsigned)time(NULL));
        initialized = 1;
    }
    /* xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx */
    unsigned r[4];
    for (int i = 0; i < 4; i++)
        r[i] = (unsigned)rand();

    /* Version 4 and variant bits. */
    r[1] = (r[1] & 0xFFFF0FFFu) | 0x00004000u;  /* version 4 */
    r[2] = (r[2] & 0x3FFFFFFFu) | 0x80000000u;  /* variant 10xx */

    snprintf(out, 37,
             "%08x-%04x-%04x-%04x-%04x%08x",
             r[0],
             (r[1] >> 16) & 0xFFFF,
             r[1] & 0xFFFF,
             (r[2] >> 16) & 0xFFFF,
             r[2] & 0xFFFF,
             r[3]);
}

/* -------------------------------------------------------------------------
 * conv_new / conv_free
 * ---------------------------------------------------------------------- */

Conversation *conv_new(Arena *arena)
{
    if (!arena)
        return NULL;

    Conversation *conv = arena_alloc(arena, sizeof(Conversation));
    if (!conv)
        return NULL;

    conv->arena  = arena;
    conv->turns  = NULL;
    conv->nturn  = 0;
    conv->cap    = 0;

    char uuid[37];
    gen_uuid(uuid);
    conv->conv_id = arena_strdup(arena, uuid);
    if (!conv->conv_id)
        return NULL;

    return conv;
}

void conv_free(Conversation *conv)
{
    if (!conv)
        return;
    /* Arena semantics — just NULLify. */
    conv->turns  = NULL;
    conv->nturn  = 0;
    conv->cap    = 0;
    conv->conv_id = NULL;
}

/* -------------------------------------------------------------------------
 * conv_add (internal growth helper)
 * ---------------------------------------------------------------------- */

/* Ensure the turns array has room for one more entry. Returns 0 on success. */
static int conv_grow(Conversation *conv)
{
    if (conv->nturn < conv->cap)
        return 0;

    int new_cap = (conv->cap == 0) ? 8 : conv->cap * 2;
    Turn *new_turns = arena_alloc(conv->arena, (size_t)new_cap * sizeof(Turn));
    if (!new_turns)
        return -1;

    if (conv->turns && conv->nturn > 0)
        memcpy(new_turns, conv->turns, (size_t)conv->nturn * sizeof(Turn));

    conv->turns = new_turns;
    conv->cap   = new_cap;
    return 0;
}

void conv_add(Conversation *conv, const char *role, const char *content)
{
    if (!conv || !role || !content)
        return;
    if (conv_grow(conv) != 0)
        return;

    char *r = arena_strdup(conv->arena, role);
    char *c = arena_strdup(conv->arena, content);
    if (!r || !c)
        return;

    conv->turns[conv->nturn] = (Turn){ .role = r, .content = c, .is_tool = 0 };
    conv->nturn++;
}

/* -------------------------------------------------------------------------
 * conv_add_tool_use
 * ---------------------------------------------------------------------- */

void conv_add_tool_use(Conversation *conv, const char *id,
                       const char *name, const char *input_json)
{
    if (!conv || !id || !name || !input_json)
        return;
    if (conv_grow(conv) != 0)
        return;

    size_t buf_size = strlen(id) * 2 + strlen(name) * 2
                    + strlen(input_json) + 128;
    char *buf = arena_alloc(conv->arena, buf_size);
    if (!buf)
        return;

    int n = snprintf(buf, buf_size,
                     "[{\"type\":\"tool_use\",\"id\":\"%s\","
                     "\"name\":\"%s\",\"input\":%s}]",
                     id, name, input_json);
    if (n < 0 || (size_t)n >= buf_size)
        return;

    char *r = arena_strdup(conv->arena, "assistant");
    if (!r)
        return;

    conv->turns[conv->nturn] = (Turn){ .role = r, .content = buf, .is_tool = 1 };
    conv->nturn++;
}

/* -------------------------------------------------------------------------
 * conv_add_tool_result
 * ---------------------------------------------------------------------- */

void conv_add_tool_result(Conversation *conv, const char *tool_use_id,
                          const char *content)
{
    if (!conv || !tool_use_id || !content)
        return;
    if (conv_grow(conv) != 0)
        return;

    size_t buf_size = strlen(content) * 6 + strlen(tool_use_id) * 2 + 128;
    char *buf = arena_alloc(conv->arena, buf_size);
    if (!buf)
        return;

    /* Build the prefix. */
    int prefix_len = snprintf(buf, buf_size,
                              "[{\"type\":\"tool_result\",\"tool_use_id\":\"%s\","
                              "\"content\":\"",
                              tool_use_id);
    if (prefix_len < 0 || (size_t)prefix_len >= buf_size)
        return;

    /* JSON-escape the content into the buffer. */
    int w = prefix_len;
    for (const unsigned char *p = (const unsigned char *)content; *p; p++) {
        if ((size_t)(w + 10) >= buf_size)
            return; /* truncated — drop silently */
        if (*p == '"') {
            buf[w++] = '\\'; buf[w++] = '"';
        } else if (*p == '\\') {
            buf[w++] = '\\'; buf[w++] = '\\';
        } else if (*p == '\n') {
            buf[w++] = '\\'; buf[w++] = 'n';
        } else if (*p == '\r') {
            buf[w++] = '\\'; buf[w++] = 'r';
        } else if (*p == '\t') {
            buf[w++] = '\\'; buf[w++] = 't';
        } else if (*p < 0x20) {
            w += snprintf(buf + w, buf_size - (size_t)w, "\\u00%02x", (unsigned)*p);
        } else {
            buf[w++] = (char)*p;
        }
    }

    /* Append closing. */
    int tail = snprintf(buf + w, buf_size - (size_t)w, "\"}]");
    if (tail < 0 || (size_t)(w + tail) >= buf_size)
        return;
    w += tail;
    (void)w;

    char *r = arena_strdup(conv->arena, "user");
    if (!r)
        return;

    conv->turns[conv->nturn] = (Turn){ .role = r, .content = buf, .is_tool = 1 };
    conv->nturn++;
}

/* -------------------------------------------------------------------------
 * conv_to_messages
 * ---------------------------------------------------------------------- */

Message *conv_to_messages(const Conversation *conv, int *out_nmsg, Arena *arena)
{
    if (out_nmsg)
        *out_nmsg = 0;
    if (!conv || conv->nturn == 0 || !arena)
        return NULL;

    Message *msgs = arena_alloc(arena, (size_t)conv->nturn * sizeof(Message));
    if (!msgs)
        return NULL;

    for (int i = 0; i < conv->nturn; i++) {
        msgs[i].role    = arena_strdup(arena, conv->turns[i].role);
        msgs[i].content = arena_strdup(arena, conv->turns[i].content);
        if (!msgs[i].role || !msgs[i].content)
            return NULL;
    }

    if (out_nmsg)
        *out_nmsg = conv->nturn;
    return msgs;
}

/* -------------------------------------------------------------------------
 * conv_save
 * ---------------------------------------------------------------------- */

int conv_save(const Conversation *conv, const char *path)
{
    if (!conv || !path)
        return -1;

    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;

    /* {"conv_id":"<id>","turns":[...]} */
    fputs("{\"conv_id\":", fp);
    fwrite_json_string(fp, conv->conv_id);
    fputs(",\"turns\":[", fp);

    for (int i = 0; i < conv->nturn; i++) {
        if (i > 0)
            fputc(',', fp);
        fputs("{\"role\":", fp);
        fwrite_json_string(fp, conv->turns[i].role);
        fputs(",\"content\":", fp);
        fwrite_json_string(fp, conv->turns[i].content);
        fprintf(fp, ",\"is_tool\":%d}", conv->turns[i].is_tool);
    }

    fputs("]}", fp);
    fclose(fp);
    return 0;
}

/* -------------------------------------------------------------------------
 * conv_load
 * ---------------------------------------------------------------------- */

/* Return the string length of token i in the json buffer. */
static int tok_len(const jsmntok_t *t, int i)
{
    return t[i].end - t[i].start;
}

/* Return 1 if token i is a string matching key. */
static int tok_str_eq(const jsmntok_t *t, int i, const char *json, const char *key)
{
    if (t[i].type != JSMN_STRING)
        return 0;
    int len = tok_len(t, i);
    if (len != (int)strlen(key))
        return 0;
    return memcmp(json + t[i].start, key, (size_t)len) == 0;
}

/*
 * Find the token index of key in the object starting at token obj_tok.
 * Returns the value token index, or -1 if not found.
 * `end_tok` is the exclusive upper bound.
 */
static int find_key_in_obj(const jsmntok_t *t, int obj_tok, int end_tok,
                           const char *json, const char *key)
{
    int size = t[obj_tok].size;
    int i = obj_tok + 1;
    for (int k = 0; k < size && i + 1 < end_tok; k++) {
        if (tok_str_eq(t, i, json, key))
            return i + 1; /* value token */
        /* Skip past the value (might be nested — use end position). */
        i += 2;
    }
    return -1;
}

/*
 * Advance token index past a complete value token (including children).
 * Returns the index of the next token after the value.
 */
static int skip_value(const jsmntok_t *t, int ntok, int i)
{
    if (i >= ntok)
        return ntok;
    int end = t[i].end;
    i++;
    while (i < ntok && t[i].start < end)
        i++;
    return i;
}

Conversation *conv_load(Arena *arena, const char *path)
{
    if (!arena || !path)
        return NULL;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    /* Read entire file into a malloc'd buffer. */
    char *buf = malloc(CONV_LOAD_MAX_FILE + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t nread = fread(buf, 1, CONV_LOAD_MAX_FILE + 1, fp);
    fclose(fp);

    if (nread > (size_t)CONV_LOAD_MAX_FILE) {
        free(buf);
        return NULL;
    }
    buf[nread] = '\0';

    /* Allocate jsmn token array. */
    jsmntok_t *toks = malloc((size_t)CONV_LOAD_MAX_TOKENS * sizeof(jsmntok_t));
    if (!toks) {
        free(buf);
        return NULL;
    }

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, buf, nread, toks, CONV_LOAD_MAX_TOKENS);
    if (ntok < 1 || toks[0].type != JSMN_OBJECT) {
        free(toks);
        free(buf);
        return NULL;
    }

    /* Create the conversation object. */
    Conversation *conv = arena_alloc(arena, sizeof(Conversation));
    if (!conv) {
        free(toks);
        free(buf);
        return NULL;
    }
    conv->arena  = arena;
    conv->turns  = NULL;
    conv->nturn  = 0;
    conv->cap    = 0;
    conv->conv_id = NULL;

    /* Find conv_id. */
    int id_tok = find_key_in_obj(toks, 0, ntok, buf, "conv_id");
    if (id_tok >= 0 && toks[id_tok].type == JSMN_STRING) {
        int len = tok_len(toks, id_tok);
        char *id_buf = arena_alloc(arena, (size_t)len + 1);
        if (id_buf) {
            memcpy(id_buf, buf + toks[id_tok].start, (size_t)len);
            id_buf[len] = '\0';
            conv->conv_id = id_buf;
        }
    }
    if (!conv->conv_id)
        conv->conv_id = arena_strdup(arena, "unknown");

    /* Find the turns array. */
    int turns_tok = find_key_in_obj(toks, 0, ntok, buf, "turns");
    if (turns_tok < 0 || toks[turns_tok].type != JSMN_ARRAY) {
        free(toks);
        free(buf);
        return conv; /* return empty conversation */
    }

    int nturns = toks[turns_tok].size;

    /* Pre-allocate turns array. */
    if (nturns > 0) {
        conv->turns = arena_alloc(arena, (size_t)nturns * sizeof(Turn));
        if (!conv->turns) {
            free(toks);
            free(buf);
            return NULL;
        }
        conv->cap = nturns;
    }

    /* Iterate over turn objects. */
    int i = turns_tok + 1; /* first element of the array */
    for (int t_idx = 0; t_idx < nturns && i < ntok; t_idx++) {
        if (toks[i].type != JSMN_OBJECT) {
            i = skip_value(toks, ntok, i);
            continue;
        }
        int obj_idx = i;
        int obj_end = i + 1; /* scan forward to find end */
        /* Compute end by looking for next token beyond the object's range. */
        {
            int obj_end_pos = toks[i].end;
            obj_end = i + 1;
            while (obj_end < ntok && toks[obj_end].start < obj_end_pos)
                obj_end++;
        }

        /* Find role. */
        int role_tok = find_key_in_obj(toks, obj_idx, obj_end, buf, "role");
        char *role = NULL;
        if (role_tok >= 0 && toks[role_tok].type == JSMN_STRING) {
            int len = tok_len(toks, role_tok);
            role = arena_alloc(arena, (size_t)len + 1);
            if (role) {
                memcpy(role, buf + toks[role_tok].start, (size_t)len);
                role[len] = '\0';
            }
        }

        /* Find content — unescape it. */
        int content_tok = find_key_in_obj(toks, obj_idx, obj_end, buf, "content");
        char *content = NULL;
        if (content_tok >= 0 && toks[content_tok].type == JSMN_STRING) {
            int src_len = tok_len(toks, content_tok);
            int out_cap = src_len + 1;
            content = arena_alloc(arena, (size_t)out_cap);
            if (content) {
                int wr = json_unescape(buf + toks[content_tok].start,
                                       src_len, content, out_cap);
                if (wr < 0) {
                    /* Fallback: copy raw. */
                    memcpy(content, buf + toks[content_tok].start, (size_t)src_len);
                    content[src_len] = '\0';
                }
            }
        }

        /* Find is_tool (primitive). */
        int is_tool = 0;
        int is_tool_tok = find_key_in_obj(toks, obj_idx, obj_end, buf, "is_tool");
        if (is_tool_tok >= 0 && toks[is_tool_tok].type == JSMN_PRIMITIVE) {
            /* primitive value is '0' or '1' */
            char c = buf[toks[is_tool_tok].start];
            is_tool = (c == '1') ? 1 : 0;
        }

        if (role && content) {
            conv->turns[conv->nturn] = (Turn){
                .role    = role,
                .content = content,
                .is_tool = is_tool
            };
            conv->nturn++;
        }

        i = obj_end;
    }

    free(toks);
    free(buf);
    return conv;
}
