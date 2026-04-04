/*
 * lsp.c — Language Server Protocol client
 *
 * Builds on the shared JSON-RPC stdio layer (jsonrpc.c) to implement
 * the minimal LSP subset needed by nanocode's agent loop:
 *   - initialize / initialized handshake
 *   - textDocument/didOpen + didChange (full-text sync)
 *   - textDocument/publishDiagnostics collection
 *   - shutdown / exit
 */

#define JSMN_STATIC
#include "jsmn.h"

#include "lsp.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Number of jsmn tokens used when parsing diagnostics messages. */
#define LSP_TOK_MAX 512

/* -------------------------------------------------------------------------
 * JSON string escaping
 * ---------------------------------------------------------------------- */

/*
 * Append `len` bytes of `s` as a JSON-escaped (unquoted) string to `b`.
 * Returns 0 on success, -1 on OOM.
 */
static int buf_append_jstr(Buf *b, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char          esc_buf[7];
        const char   *esc  = NULL;
        size_t        elen = 1;

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
                int n = snprintf(esc_buf, sizeof(esc_buf), "\\u%04x",
                                 (unsigned)c);
                if (n < 0) return -1;
                esc  = esc_buf;
                elen = 6;
            }
            break;
        }

        if (esc) {
            if (buf_append(b, esc, elen) != 0)
                return -1;
        } else {
            char ch = (char)c;
            if (buf_append(b, &ch, 1) != 0)
                return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * jsmn helper: skip a token and all its children
 * ---------------------------------------------------------------------- */

/*
 * Return the index of the first token that lies AFTER the token at `idx`
 * (including all descendants).  Uses the token's `end` field.
 */
static int jsmn_skip(const jsmntok_t *t, int ntok, int idx)
{
    if (idx >= ntok)
        return ntok;
    int past = t[idx].end;
    int i    = idx + 1;
    while (i < ntok && t[i].start < past)
        i++;
    return i;
}

/* -------------------------------------------------------------------------
 * jsmn helper: find key in an object, return value index
 * ---------------------------------------------------------------------- */

/*
 * Search the object at `obj_idx` for a STRING key matching `key`.
 * Returns the index of the associated value token, or -1 if not found.
 */
static int find_key(const jsmntok_t *t, int ntok, int obj_idx,
                    const char *json, const char *key)
{
    if (obj_idx >= ntok || t[obj_idx].type != JSMN_OBJECT)
        return -1;

    size_t klen = strlen(key);
    int    past = t[obj_idx].end;
    int    i    = obj_idx + 1;

    while (i < ntok && t[i].start < past) {
        if (t[i].type == JSMN_STRING) {
            int tlen = t[i].end - t[i].start;
            if ((size_t)tlen == klen &&
                memcmp(json + t[i].start, key, klen) == 0) {
                int vi = i + 1;
                return vi < ntok ? vi : -1;
            }
        }
        /* Skip this key and its value. */
        int vi = jsmn_skip(t, ntok, i);   /* past key */
        i      = jsmn_skip(t, ntok, vi);  /* past value */
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * jsmn helper: compare a STRING token to a literal
 * ---------------------------------------------------------------------- */

static int tok_str_eq(const jsmntok_t *t, int idx,
                      const char *json, const char *val)
{
    if (t[idx].type != JSMN_STRING)
        return 0;
    size_t vlen = strlen(val);
    int    tlen = t[idx].end - t[idx].start;
    return (size_t)tlen == vlen &&
           memcmp(json + t[idx].start, val, vlen) == 0;
}

/* -------------------------------------------------------------------------
 * jsmn helper: copy a PRIMITIVE token as a NUL-terminated int string
 * ---------------------------------------------------------------------- */

static int tok_int(const jsmntok_t *t, int idx, const char *json)
{
    if (t[idx].type != JSMN_PRIMITIVE)
        return 0;
    char buf[16];
    int  len = t[idx].end - t[idx].start;
    if (len <= 0 || len >= (int)sizeof(buf))
        return 0;
    memcpy(buf, json + t[idx].start, (size_t)len);
    buf[len] = '\0';
    return atoi(buf);
}

/* -------------------------------------------------------------------------
 * Parse a textDocument/publishDiagnostics notification
 * ---------------------------------------------------------------------- */

/*
 * Parse `json` (length `jlen`) as a publishDiagnostics notification.
 * Fills up to `max_diags` entries in `diags[]`.
 * Returns the number of diagnostics parsed (0 if not a diag notification).
 */
static int parse_diag_msg(const char *json, int jlen,
                          LspDiag *diags, int max_diags)
{
    jsmntok_t tok[LSP_TOK_MAX];
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, json, (size_t)jlen, tok, LSP_TOK_MAX);
    if (ntok <= 0)
        return 0;

    /* Must be a notification with method == textDocument/publishDiagnostics. */
    int method_val = find_key(tok, ntok, 0, json, "method");
    if (method_val < 0)
        return 0;
    if (!tok_str_eq(tok, method_val, json, "textDocument/publishDiagnostics"))
        return 0;

    /* Locate params. */
    int params_idx = find_key(tok, ntok, 0, json, "params");
    if (params_idx < 0 || tok[params_idx].type != JSMN_OBJECT)
        return 0;

    /* Locate params.uri (shared across all diagnostics in this message). */
    int uri_idx = find_key(tok, ntok, params_idx, json, "uri");

    /* Locate params.diagnostics array. */
    int arr_idx = find_key(tok, ntok, params_idx, json, "diagnostics");
    if (arr_idx < 0 || tok[arr_idx].type != JSMN_ARRAY)
        return 0;

    int ndiag    = 0;
    int arr_past = tok[arr_idx].end;
    int i        = arr_idx + 1;

    while (i < ntok && tok[i].start < arr_past && ndiag < max_diags) {
        if (tok[i].type != JSMN_OBJECT) {
            i = jsmn_skip(tok, ntok, i);
            continue;
        }

        LspDiag *d = &diags[ndiag];
        memset(d, 0, sizeof(*d));
        d->severity = 1;  /* default to error */

        /* Copy URI. */
        if (uri_idx >= 0 && tok[uri_idx].type == JSMN_STRING) {
            int ulen = tok[uri_idx].end - tok[uri_idx].start;
            int copy = (ulen < LSP_PATH_MAX - 1) ? ulen : LSP_PATH_MAX - 1;
            memcpy(d->uri, json + tok[uri_idx].start, (size_t)copy);
            d->uri[copy] = '\0';
        }

        /* severity (primitive integer). */
        int sev_idx = find_key(tok, ntok, i, json, "severity");
        if (sev_idx >= 0)
            d->severity = tok_int(tok, sev_idx, json);

        /* range.start.line and range.start.character. */
        int range_idx = find_key(tok, ntok, i, json, "range");
        if (range_idx >= 0 && tok[range_idx].type == JSMN_OBJECT) {
            int start_idx = find_key(tok, ntok, range_idx, json, "start");
            if (start_idx >= 0 && tok[start_idx].type == JSMN_OBJECT) {
                int line_idx = find_key(tok, ntok, start_idx, json, "line");
                int char_idx = find_key(tok, ntok, start_idx, json, "character");
                if (line_idx >= 0)
                    d->line = tok_int(tok, line_idx, json);
                if (char_idx >= 0)
                    d->col  = tok_int(tok, char_idx, json);
            }
        }

        /* message string. */
        int msg_idx = find_key(tok, ntok, i, json, "message");
        if (msg_idx >= 0 && tok[msg_idx].type == JSMN_STRING) {
            int mlen = tok[msg_idx].end - tok[msg_idx].start;
            int copy = (mlen < LSP_DIAG_MSG_MAX - 1) ? mlen : LSP_DIAG_MSG_MAX - 1;
            memcpy(d->message, json + tok[msg_idx].start, (size_t)copy);
            d->message[copy] = '\0';
        }

        if (d->message[0] != '\0')
            ndiag++;

        i = jsmn_skip(tok, ntok, i);
    }

    return ndiag;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

void lsp_init(LspClient *client)
{
    memset(client, 0, sizeof(*client));
    client->rpc.pid      = (pid_t)-1;
    client->rpc.write_fd = -1;
    client->rpc.read_fd  = -1;
    client->rpc.next_id  = 1;
}

/*
 * Split `cmd` on whitespace into argv[].
 * Writes into `buf` (in-place); argv[] points into buf.
 * Returns argc, or 0 on empty input.
 */
static int split_cmd(char *buf, char **argv, int max_argc)
{
    int   argc = 0;
    char *p    = buf;

    while (*p && argc < max_argc - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

int lsp_start(LspClient *client, const char *cmd, const char *root_uri)
{
    /* Split cmd into argv (in-place in a local copy). */
    char  cmd_buf[256];
    char *argv[16];

    int n = snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    if (n < 0 || (size_t)n >= sizeof(cmd_buf))
        return -1;

    if (split_cmd(cmd_buf, argv, 16) == 0)
        return -1;

    /* Spawn the language server. */
    if (jsonrpc_spawn(&client->rpc, argv[0], argv) != 0)
        return -1;

    /* Build initialize params. */
    char params[1024];
    n = snprintf(params, sizeof(params),
        "{"
          "\"processId\":%d,"
          "\"rootUri\":\"%s\","
          "\"capabilities\":{"
            "\"textDocument\":{"
              "\"publishDiagnostics\":{}"
            "}"
          "}"
        "}",
        (int)getpid(), root_uri);
    if (n < 0 || (size_t)n >= sizeof(params)) {
        jsonrpc_close(&client->rpc);
        return -1;
    }

    /* Send initialize request. */
    int id = jsonrpc_next_id(&client->rpc);
    if (jsonrpc_send(&client->rpc, "initialize", params, id) != 0) {
        jsonrpc_close(&client->rpc);
        return -1;
    }

    /* Wait for initialize result (up to 10s). */
    char resp[JSONRPC_MSG_MAX];
    int  rlen = jsonrpc_recv(&client->rpc, resp, sizeof(resp), 10000);
    if (rlen < 0) {
        jsonrpc_close(&client->rpc);
        return -1;
    }

    /* Minimal check: the response should contain "result", not "error". */
    if (strstr(resp, "\"result\"") == NULL) {
        jsonrpc_close(&client->rpc);
        return -1;
    }

    /* Send initialized notification. */
    if (jsonrpc_send(&client->rpc, "initialized", "{}", 0) != 0) {
        jsonrpc_close(&client->rpc);
        return -1;
    }

    client->initialized = 1;
    client->seq         = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * textDocument notifications
 * ---------------------------------------------------------------------- */

int lsp_did_open(LspClient *client, const char *uri,
                 const char *language_id, const char *text)
{
    if (!client->initialized)
        return -1;

    Buf params;
    buf_init(&params);

    if (buf_append_str(&params, "{\"textDocument\":{\"uri\":\"") != 0 ||
        buf_append_jstr(&params, uri, strlen(uri))              != 0 ||
        buf_append_str(&params, "\",\"languageId\":\"")         != 0 ||
        buf_append_str(&params, language_id)                    != 0 ||
        buf_append_str(&params, "\",\"version\":")              != 0)
        goto oom;

    {
        char ver[24];
        int n = snprintf(ver, sizeof(ver), "%d", ++client->seq);
        if (n < 0 || (size_t)n >= sizeof(ver)) goto oom;
        if (buf_append_str(&params, ver) != 0) goto oom;
    }

    if (buf_append_str(&params, ",\"text\":\"")                 != 0 ||
        buf_append_jstr(&params, text, strlen(text))            != 0 ||
        buf_append_str(&params, "\"}}")                         != 0)
        goto oom;

    {
        const char *p = buf_str(&params);
        int rc = jsonrpc_send(&client->rpc, "textDocument/didOpen", p, 0);
        buf_destroy(&params);
        return rc;
    }

oom:
    buf_destroy(&params);
    return -1;
}

int lsp_did_change(LspClient *client, const char *uri, const char *text)
{
    if (!client->initialized)
        return -1;

    Buf params;
    buf_init(&params);

    if (buf_append_str(&params, "{\"textDocument\":{\"uri\":\"") != 0 ||
        buf_append_jstr(&params, uri, strlen(uri))              != 0 ||
        buf_append_str(&params, "\",\"version\":")              != 0)
        goto oom;

    {
        char ver[24];
        int n = snprintf(ver, sizeof(ver), "%d", ++client->seq);
        if (n < 0 || (size_t)n >= sizeof(ver)) goto oom;
        if (buf_append_str(&params, ver) != 0) goto oom;
    }

    if (buf_append_str(&params, "},\"contentChanges\":[{\"text\":\"") != 0 ||
        buf_append_jstr(&params, text, strlen(text))                  != 0 ||
        buf_append_str(&params, "\"}]}")                              != 0)
        goto oom;

    {
        const char *p = buf_str(&params);
        int rc = jsonrpc_send(&client->rpc, "textDocument/didChange", p, 0);
        buf_destroy(&params);
        return rc;
    }

oom:
    buf_destroy(&params);
    return -1;
}

/* -------------------------------------------------------------------------
 * Collect diagnostics
 * ---------------------------------------------------------------------- */

char *lsp_collect_diagnostics(LspClient *client, Arena *arena, int timeout_ms)
{
    if (!client->initialized || client->rpc.read_fd < 0)
        return NULL;

    LspDiag diags[LSP_DIAG_MAX];
    int     ndiag = 0;

    /* Use CLOCK_MONOTONIC to track elapsed time across multiple recv calls. */
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    char msg[JSONRPC_MSG_MAX];

    for (;;) {
        /* Compute remaining budget. */
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        long elapsed_ms = (ts_now.tv_sec  - ts_start.tv_sec)  * 1000L +
                          (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000L;
        int remaining = timeout_ms - (int)elapsed_ms;
        if (remaining <= 0)
            break;

        int r = jsonrpc_recv(&client->rpc, msg, sizeof(msg), remaining);
        if (r < 0)
            break;  /* timeout or error */

        /* Parse this message and collect any diagnostics. */
        LspDiag batch[LSP_DIAG_MAX];
        int nbatch = parse_diag_msg(msg, r, batch, LSP_DIAG_MAX);
        for (int k = 0; k < nbatch && ndiag < LSP_DIAG_MAX; k++)
            diags[ndiag++] = batch[k];
    }

    if (ndiag == 0)
        return NULL;

    /* Format into arena-allocated string. */
    Buf out;
    buf_init(&out);

    for (int i = 0; i < ndiag; i++) {
        LspDiag    *d   = &diags[i];
        const char *sev = d->severity == 1 ? "error"   :
                          d->severity == 2 ? "warning" :
                          d->severity == 3 ? "info"    : "hint";

        /* Strip the "file://" prefix from the URI for human-readable paths. */
        const char *path = d->uri;
        if (strncmp(path, "file://", 7) == 0)
            path += 7;

        /* "[compiler: error at /path/file.c:LINE:COL: message]\n" */
        char line_buf[LSP_PATH_MAX + LSP_DIAG_MSG_MAX + 64];
        int  n = snprintf(line_buf, sizeof(line_buf),
                          "[compiler: %s at %s:%d:%d: %s]\n",
                          sev, path, d->line + 1, d->col + 1, d->message);
        if (n > 0 && (size_t)n < sizeof(line_buf))
            buf_append_str(&out, line_buf);
    }

    const char *str    = buf_str(&out);
    size_t      slen   = out.len;
    char       *result = (char *)arena_alloc(arena, slen + 1);
    if (result) {
        memcpy(result, str, slen);
        result[slen] = '\0';
    }
    buf_destroy(&out);
    return result;
}

/* -------------------------------------------------------------------------
 * Shutdown
 * ---------------------------------------------------------------------- */

void lsp_stop(LspClient *client)
{
    if (client->initialized) {
        /* Graceful: send shutdown request and wait for the response. */
        int  id = jsonrpc_next_id(&client->rpc);
        jsonrpc_send(&client->rpc, "shutdown", "{}", id);

        char resp[4096];
        jsonrpc_recv(&client->rpc, resp, sizeof(resp), 2000);

        /* Then send the exit notification (no response expected). */
        jsonrpc_send(&client->rpc, "exit", NULL, 0);

        client->initialized = 0;
    }
    jsonrpc_close(&client->rpc);
}

/* -------------------------------------------------------------------------
 * Detection helpers
 * ---------------------------------------------------------------------- */

const char *lsp_detect_server(const char *filepath)
{
    const char *ext = strrchr(filepath, '.');
    if (!ext || ext[1] == '\0')
        return NULL;
    ext++;  /* skip '.' */

    if (strcmp(ext, "c")   == 0 || strcmp(ext, "h")   == 0 ||
        strcmp(ext, "cc")  == 0 || strcmp(ext, "cpp") == 0 ||
        strcmp(ext, "cxx") == 0 || strcmp(ext, "hpp") == 0)
        return "clangd";

    if (strcmp(ext, "py") == 0)
        return "pylsp";

    if (strcmp(ext, "ts")  == 0 || strcmp(ext, "tsx") == 0 ||
        strcmp(ext, "js")  == 0 || strcmp(ext, "jsx") == 0)
        return "typescript-language-server --stdio";

    if (strcmp(ext, "rs") == 0)
        return "rust-analyzer";

    if (strcmp(ext, "go") == 0)
        return "gopls";

    return NULL;
}

const char *lsp_detect_language(const char *filepath)
{
    const char *ext = strrchr(filepath, '.');
    if (!ext || ext[1] == '\0')
        return "plaintext";
    ext++;

    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0)
        return "c";
    if (strcmp(ext, "cc")  == 0 || strcmp(ext, "cpp") == 0 ||
        strcmp(ext, "cxx") == 0 || strcmp(ext, "hpp") == 0)
        return "cpp";
    if (strcmp(ext, "py") == 0)
        return "python";
    if (strcmp(ext, "ts") == 0 || strcmp(ext, "tsx") == 0)
        return "typescript";
    if (strcmp(ext, "js") == 0 || strcmp(ext, "jsx") == 0)
        return "javascript";
    if (strcmp(ext, "rs") == 0)
        return "rust";
    if (strcmp(ext, "go") == 0)
        return "go";

    return "plaintext";
}
