/*
 * mcp.c — MCP client: JSON-RPC 2.0 over STDIO, tool discovery, config
 *
 * JSON-RPC 2.0 messages are newline-delimited.  We spawn the server with
 * fork/execvp, wire up two pipes (one for each direction), and exchange
 * single-line JSON messages.
 *
 * fork/exec is guarded by #ifndef __SANITIZE_ADDRESS__ to prevent hangs
 * under AddressSanitizer on macOS.  Under ASan all public functions return
 * NULL or error gracefully so the test suite stays clean.
 */

#define JSMN_STATIC
#include "jsmn.h"

#include "mcp.h"
#include "../util/buf.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define MCP_READ_BUF    8192   /* sliding read buffer in MCPClient */
#define MCP_RESP_MAX    65536  /* max JSON-RPC response size (64 KB) */
#define MCP_TOKEN_MAX   1024   /* max jsmn tokens per response parse */
#define MCP_TIMEOUT_SEC 5      /* select() timeout waiting for server */
#define MCP_CONFIG_MAX  32768  /* max bytes in mcp.json */

/* -------------------------------------------------------------------------
 * MCPClient definition
 * ---------------------------------------------------------------------- */

struct MCPClient {
    int    stdin_fd;              /* write end of pipe → child stdin */
    int    stdout_fd;             /* read end of pipe ← child stdout */
    pid_t  child_pid;
    int    next_id;               /* monotonic JSON-RPC request ID */
    int    initialized;           /* 1 after successful handshake */
    char   read_buf[MCP_READ_BUF];
    size_t read_len;              /* bytes accumulated in read_buf */
};

/* -------------------------------------------------------------------------
 * Static tool dispatch table
 * ---------------------------------------------------------------------- */

typedef struct {
    MCPClient *client;
    char       tool_name[128];
    char       schema_json[2048]; /* schema kept here for tool_register ptr */
} MCPSlot;

static MCPSlot    s_slots[MCP_MAX_TOTAL_TOOLS];
static int        s_slot_count = 0;

/* Persistent client references prevent premature free. */
static MCPClient *s_clients[MCP_SERVERS_MAX];
static int        s_client_count = 0;

/* Forward declaration. */
static ToolResult mcp_slot_dispatch(int slot, Arena *arena,
                                    const char *args_json);

/* Pre-generate MCP_MAX_TOTAL_TOOLS (32) static handler functions, one per
 * slot.  Each captures its index via the literal token N. */
#define MCP_SLOT_FN(N) \
    static ToolResult mcp_slot_##N(Arena *a, const char *j) { \
        return mcp_slot_dispatch((N), a, j); \
    }

MCP_SLOT_FN( 0) MCP_SLOT_FN( 1) MCP_SLOT_FN( 2) MCP_SLOT_FN( 3)
MCP_SLOT_FN( 4) MCP_SLOT_FN( 5) MCP_SLOT_FN( 6) MCP_SLOT_FN( 7)
MCP_SLOT_FN( 8) MCP_SLOT_FN( 9) MCP_SLOT_FN(10) MCP_SLOT_FN(11)
MCP_SLOT_FN(12) MCP_SLOT_FN(13) MCP_SLOT_FN(14) MCP_SLOT_FN(15)
MCP_SLOT_FN(16) MCP_SLOT_FN(17) MCP_SLOT_FN(18) MCP_SLOT_FN(19)
MCP_SLOT_FN(20) MCP_SLOT_FN(21) MCP_SLOT_FN(22) MCP_SLOT_FN(23)
MCP_SLOT_FN(24) MCP_SLOT_FN(25) MCP_SLOT_FN(26) MCP_SLOT_FN(27)
MCP_SLOT_FN(28) MCP_SLOT_FN(29) MCP_SLOT_FN(30) MCP_SLOT_FN(31)

static ToolHandler s_slot_fns[MCP_MAX_TOTAL_TOOLS] = {
    mcp_slot_0,  mcp_slot_1,  mcp_slot_2,  mcp_slot_3,
    mcp_slot_4,  mcp_slot_5,  mcp_slot_6,  mcp_slot_7,
    mcp_slot_8,  mcp_slot_9,  mcp_slot_10, mcp_slot_11,
    mcp_slot_12, mcp_slot_13, mcp_slot_14, mcp_slot_15,
    mcp_slot_16, mcp_slot_17, mcp_slot_18, mcp_slot_19,
    mcp_slot_20, mcp_slot_21, mcp_slot_22, mcp_slot_23,
    mcp_slot_24, mcp_slot_25, mcp_slot_26, mcp_slot_27,
    mcp_slot_28, mcp_slot_29, mcp_slot_30, mcp_slot_31,
};

/* -------------------------------------------------------------------------
 * Error helpers
 * ---------------------------------------------------------------------- */

static ToolResult make_error(Arena *arena, const char *msg)
{
    size_t len  = strlen(msg);
    char  *copy = arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, msg, len + 1);
    ToolResult r;
    r.error   = 1;
    r.content = copy ? copy : (char *)"";
    r.len     = copy ? len  : 0;
    return r;
}

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

/*
 * Write all `len` bytes from `buf` to `fd`.  Returns 0 on success, -1 on error.
 */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * Send a JSON-RPC message: write `json` followed by '\n' to c->stdin_fd.
 * Returns 0 on success, -1 on write error.
 */
static int rpc_send(MCPClient *c, const char *json)
{
    size_t len = strlen(json);
    if (write_all(c->stdin_fd, json, len) < 0) return -1;
    if (write_all(c->stdin_fd, "\n",   1) < 0) return -1;
    return 0;
}

/*
 * Read one newline-terminated line from c->stdout_fd into `buf` (cap bytes).
 * Uses select() with MCP_TIMEOUT_SEC second timeout.
 * Returns length of line (excluding '\n') on success, -1 on error/timeout/EOF.
 */
static int read_line(MCPClient *c, char *buf, size_t cap, int timeout_sec)
{
    for (;;) {
        /* Scan read_buf for a newline. */
        char *nl = (char *)memchr(c->read_buf, '\n', c->read_len);
        if (nl) {
            size_t line_len = (size_t)(nl - c->read_buf);
            if (line_len + 1 > cap)
                return -1; /* line too long for output buffer */
            memcpy(buf, c->read_buf, line_len);
            buf[line_len] = '\0';
            /* Slide remaining bytes to the front. */
            size_t rest = c->read_len - line_len - 1;
            memmove(c->read_buf, nl + 1, rest);
            c->read_len = rest;
            return (int)line_len;
        }

        /* No newline yet; check for buffer overflow. */
        if (c->read_len >= sizeof(c->read_buf) - 1)
            return -1; /* response larger than read buffer */

        /* Wait for more data. */
        fd_set        rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(c->stdout_fd, &rfds);
        tv.tv_sec  = timeout_sec;
        tv.tv_usec = 0;
        int sel = select(c->stdout_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) return -1; /* timeout or error */

        ssize_t n = read(c->stdout_fd,
                         c->read_buf + c->read_len,
                         sizeof(c->read_buf) - 1 - c->read_len);
        if (n <= 0) return -1; /* EOF or error */
        c->read_len += (size_t)n;
    }
}

/*
 * Check if `json` contains "id":<expected_id> (compact form).
 * Used to match JSON-RPC response IDs quickly.
 */
static int has_id(const char *json, int expected_id)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"id\":%d", expected_id);
    return strstr(json, needle) != NULL;
}

/*
 * Read from c->stdout_fd, discarding notifications (objects that contain
 * "method" but no matching id), until we receive a response for expected_id.
 * Writes the matching response line into resp (up to cap bytes).
 * Returns 0 on success, -1 on failure.
 */
static int rpc_read_response(MCPClient *c, char *resp, size_t cap,
                              int expected_id)
{
    for (int attempts = 0; attempts < 32; attempts++) {
        int n = read_line(c, resp, cap, MCP_TIMEOUT_SEC);
        if (n < 0) return -1;
        if (n == 0) continue; /* empty line, skip */

        /* If this line has the expected id, it's our response. */
        if (has_id(resp, expected_id))
            return 0;

        /* If it has a "method" field it's a notification — discard and retry. */
        if (strstr(resp, "\"method\"") != NULL)
            continue;

        /* Unknown line; still return it if it looks like a response. */
        if (strstr(resp, "\"result\"") != NULL || strstr(resp, "\"error\"") != NULL)
            return 0;
    }
    return -1; /* ran out of attempts */
}

/* -------------------------------------------------------------------------
 * jsmn helpers (token-tree walking)
 * ---------------------------------------------------------------------- */

/*
 * Return 1 if token i in `json` is a string equal to `key`.
 */
static int tok_str_eq(const jsmntok_t *t, int i, const char *json,
                      const char *key)
{
    int klen = (int)strlen(key);
    int tlen = t[i].end - t[i].start;
    return t[i].type == JSMN_STRING && tlen == klen &&
           memcmp(json + t[i].start, key, (size_t)klen) == 0;
}

/*
 * Skip over the token at index `i` (including all its children for
 * containers) and return the next token index.
 */
static int tok_skip(const jsmntok_t *t, int ntok, int i)
{
    if (i >= ntok) return ntok;
    if (t[i].type == JSMN_OBJECT) {
        int sz = t[i].size;
        i++;
        for (int k = 0; k < sz; k++) {
            i = tok_skip(t, ntok, i); /* key   */
            i = tok_skip(t, ntok, i); /* value */
        }
    } else if (t[i].type == JSMN_ARRAY) {
        int sz = t[i].size;
        i++;
        for (int k = 0; k < sz; k++)
            i = tok_skip(t, ntok, i);
    } else {
        i++;
    }
    return i;
}

/*
 * Find the value token for `key` within the object at obj_idx.
 * Returns the token index of the value on success, -1 if not found.
 */
static int find_key(const jsmntok_t *t, int ntok, int obj_idx,
                    const char *json, const char *key)
{
    if (obj_idx < 0 || obj_idx >= ntok || t[obj_idx].type != JSMN_OBJECT)
        return -1;
    int sz = t[obj_idx].size;
    int i  = obj_idx + 1;
    for (int k = 0; k < sz && i + 1 < ntok; k++) {
        if (tok_str_eq(t, i, json, key))
            return i + 1; /* value immediately follows key */
        i = tok_skip(t, ntok, i); /* skip key   */
        i = tok_skip(t, ntok, i); /* skip value */
    }
    return -1;
}

/*
 * Copy a token's raw string value (unescaped as-is from jsmn) into `dst`.
 * Truncates at dst_cap-1 bytes. Returns number of bytes copied.
 */
static int tok_copy_str(const jsmntok_t *t, int i, const char *json,
                        char *dst, size_t dst_cap)
{
    if (t[i].type != JSMN_STRING) return 0;
    int len = t[i].end - t[i].start;
    if ((size_t)len >= dst_cap) len = (int)dst_cap - 1;
    memcpy(dst, json + t[i].start, (size_t)len);
    dst[len] = '\0';
    return len;
}

/* -------------------------------------------------------------------------
 * JSON string escaping (for building schema_json output)
 * ---------------------------------------------------------------------- */

/*
 * Write `src` (srclen bytes) into `dst` with JSON string-safe escaping.
 * dst_cap must include space for the NUL terminator.
 * Returns number of bytes written (not counting NUL).
 */
static size_t json_esc(char *dst, size_t dst_cap, const char *src, size_t srclen)
{
    size_t out = 0;
    for (size_t i = 0; i < srclen; i++) {
        unsigned char c   = (unsigned char)src[i];
        const char   *esc = NULL;
        size_t        elen;
        char          hex[7];
        switch (c) {
        case '"':  esc = "\\\""; elen = 2; break;
        case '\\': esc = "\\\\"; elen = 2; break;
        case '\n': esc = "\\n";  elen = 2; break;
        case '\r': esc = "\\r";  elen = 2; break;
        case '\t': esc = "\\t";  elen = 2; break;
        default:
            if (c < 0x20) {
                snprintf(hex, sizeof(hex), "\\u%04x", (unsigned)c);
                esc  = hex;
                elen = 6;
            }
            break;
        }
        if (esc) {
            if (dst && out + elen < dst_cap)
                memcpy(dst + out, esc, elen);
            out += elen;
        } else {
            if (dst && out + 1 < dst_cap)
                dst[out] = (char)c;
            out += 1;
        }
    }
    if (dst && out < dst_cap)
        dst[out] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

MCPClient *mcp_client_new(Arena *arena, const char *server_cmd,
                          char *const argv[])
{
    (void)arena; /* reserved */

    if (!server_cmd || !argv) return NULL;

#ifdef __SANITIZE_ADDRESS__
    /* Suppress fork() under ASan to avoid macOS hangs. */
    (void)server_cmd; (void)argv;
    return NULL;
#else
    int pipe_to_child[2];   /* parent writes, child reads */
    int pipe_from_child[2]; /* child writes, parent reads */

    if (pipe(pipe_to_child) < 0)   return NULL;
    if (pipe(pipe_from_child) < 0) {
        close(pipe_to_child[0]);
        close(pipe_to_child[1]);
        return NULL;
    }

    MCPClient *c = calloc(1, sizeof(*c));
    if (!c) {
        close(pipe_to_child[0]);   close(pipe_to_child[1]);
        close(pipe_from_child[0]); close(pipe_from_child[1]);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(c);
        close(pipe_to_child[0]);   close(pipe_to_child[1]);
        close(pipe_from_child[0]); close(pipe_from_child[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child: wire pipes to stdin/stdout and exec the server. */
        close(pipe_to_child[1]);
        close(pipe_from_child[0]);
        if (dup2(pipe_to_child[0],   STDIN_FILENO)  < 0) _exit(1);
        if (dup2(pipe_from_child[1], STDOUT_FILENO) < 0) _exit(1);
        close(pipe_to_child[0]);
        close(pipe_from_child[1]);
        execvp(server_cmd, argv);
        _exit(127); /* exec failed */
    }

    /* Parent: close the child-side ends. */
    close(pipe_to_child[0]);
    close(pipe_from_child[1]);

    c->stdin_fd   = pipe_to_child[1];
    c->stdout_fd  = pipe_from_child[0];
    c->child_pid  = pid;
    c->next_id    = 1;
    c->initialized = 0;
    c->read_len   = 0;

    return c;
#endif /* __SANITIZE_ADDRESS__ */
}

int mcp_client_init(MCPClient *c)
{
    if (!c) return -1;

#ifdef __SANITIZE_ADDRESS__
    return -1;
#else
    char req[512];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{"
            "\"protocolVersion\":\"2024-11-05\","
            "\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"nanocode\",\"version\":\"0.1.0\"}"
        "}}");

    if (rpc_send(c, req) < 0)
        return -1;

    char *resp = malloc(MCP_RESP_MAX);
    if (!resp) return -1;

    int rc = rpc_read_response(c, resp, MCP_RESP_MAX, 1);
    free(resp);
    if (rc < 0) return -1;

    /* Send the required notifications/initialized notification (no response). */
    const char *notif =
        "{\"jsonrpc\":\"2.0\","
        "\"method\":\"notifications/initialized\","
        "\"params\":{}}";
    rpc_send(c, notif); /* best-effort; ignore failure */

    c->initialized = 1;
    c->next_id = 2;
    return 0;
#endif /* __SANITIZE_ADDRESS__ */
}

ToolList *mcp_list_tools(MCPClient *c, Arena *arena)
{
    if (!c || !arena) return NULL;

#ifdef __SANITIZE_ADDRESS__
    ToolList *tl = arena_alloc(arena, sizeof(ToolList));
    if (tl) { tl->tools = NULL; tl->count = 0; }
    return tl;
#else
    if (!c->initialized) return NULL;

    int req_id = c->next_id++;
    char req[128];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"tools/list\",\"params\":{}}",
        req_id);

    if (rpc_send(c, req) < 0) return NULL;

    char *resp = malloc(MCP_RESP_MAX);
    if (!resp) return NULL;

    if (rpc_read_response(c, resp, MCP_RESP_MAX, req_id) < 0) {
        free(resp);
        return NULL;
    }

    /* Parse response with jsmn. */
    jsmntok_t tokens[MCP_TOKEN_MAX];
    jsmn_parser parser;
    jsmn_init(&parser);
    size_t resp_len = strlen(resp);
    int ntok = jsmn_parse(&parser, resp, resp_len, tokens, MCP_TOKEN_MAX);
    if (ntok < 1) {
        free(resp);
        return NULL;
    }

    /* result.tools array */
    int result_idx = find_key(tokens, ntok, 0, resp, "result");
    if (result_idx < 0 || tokens[result_idx].type != JSMN_OBJECT) {
        free(resp);
        return NULL;
    }
    int tools_idx = find_key(tokens, ntok, result_idx, resp, "tools");
    if (tools_idx < 0 || tokens[tools_idx].type != JSMN_ARRAY) {
        free(resp);
        return NULL;
    }

    int n_tools = tokens[tools_idx].size;
    ToolList *list = arena_alloc(arena, sizeof(ToolList));
    if (!list) { free(resp); return NULL; }
    list->count = 0;
    list->tools = n_tools > 0
        ? arena_alloc(arena, (size_t)n_tools * sizeof(MCPToolEntry))
        : NULL;
    if (n_tools > 0 && !list->tools) { free(resp); return NULL; }

    int ti = tools_idx + 1;
    int limit = n_tools < MCP_MAX_TOTAL_TOOLS ? n_tools : MCP_MAX_TOTAL_TOOLS;

    for (int k = 0; k < limit; k++) {
        if (ti >= ntok || tokens[ti].type != JSMN_OBJECT) break;

        MCPToolEntry *e = &list->tools[list->count];
        e->name[0]        = '\0';
        e->description[0] = '\0';
        e->schema_json[0] = '\0';

        /* name */
        int name_idx = find_key(tokens, ntok, ti, resp, "name");
        if (name_idx >= 0)
            tok_copy_str(tokens, name_idx, resp, e->name, sizeof(e->name));

        /* description */
        int desc_idx = find_key(tokens, ntok, ti, resp, "description");
        if (desc_idx >= 0)
            tok_copy_str(tokens, desc_idx, resp,
                         e->description, sizeof(e->description));

        /* inputSchema: grab the raw JSON substring */
        int schema_idx = find_key(tokens, ntok, ti, resp, "inputSchema");
        if (schema_idx >= 0 && e->name[0] != '\0') {
            /* Raw JSON of inputSchema value. */
            int schema_start = tokens[schema_idx].start;
            int schema_end   = tokens[schema_idx].end;
            /* For containers, end covers the closing bracket; good. */
            int raw_len = schema_end - schema_start;

            /* Escape name and description for embedding in JSON. */
            char esc_name[256], esc_desc[1024];
            json_esc(esc_name, sizeof(esc_name),
                     e->name, strlen(e->name));
            json_esc(esc_desc, sizeof(esc_desc),
                     e->description, strlen(e->description));

            snprintf(e->schema_json, sizeof(e->schema_json),
                "{\"name\":\"%s\",\"description\":\"%s\","
                "\"input_schema\":%.*s}",
                esc_name, esc_desc,
                raw_len, resp + schema_start);
        }

        list->count++;
        ti = tok_skip(tokens, ntok, ti);
    }

    free(resp);
    return list;
#endif /* __SANITIZE_ADDRESS__ */
}

ToolResult mcp_call_tool(MCPClient *c, Arena *arena, const char *name,
                         const char *args_json)
{
    if (!arena) {
        ToolResult r = {1, (char *)"null arena", 10};
        return r;
    }
    if (!c || !name)
        return make_error(arena, "mcp_call_tool: null argument");

#ifdef __SANITIZE_ADDRESS__
    return make_error(arena, "mcp_call_tool: unavailable under ASan");
#else
    if (!c->initialized)
        return make_error(arena, "mcp_call_tool: client not initialized");

    const char *aargs = args_json && args_json[0] ? args_json : "{}";

    /* Build tools/call request. */
    int  req_id = c->next_id++;
    Buf  b;
    buf_init(&b);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":",
        req_id, name);
    buf_append_str(&b, hdr);
    buf_append_str(&b, aargs);
    buf_append_str(&b, "}}");

    const char *req = buf_str(&b);
    if (!req) {
        buf_destroy(&b);
        return make_error(arena, "mcp_call_tool: OOM building request");
    }

    int send_rc = rpc_send(c, req);
    buf_destroy(&b);
    if (send_rc < 0)
        return make_error(arena, "mcp_call_tool: write failed");

    /* Read response. */
    char *resp = malloc(MCP_RESP_MAX);
    if (!resp)
        return make_error(arena, "mcp_call_tool: OOM");

    if (rpc_read_response(c, resp, MCP_RESP_MAX, req_id) < 0) {
        free(resp);
        return make_error(arena, "mcp_call_tool: no response");
    }

    /* Parse response. */
    jsmntok_t tokens[MCP_TOKEN_MAX];
    jsmn_parser parser;
    jsmn_init(&parser);
    size_t resp_len = strlen(resp);
    int ntok = jsmn_parse(&parser, resp, resp_len, tokens, MCP_TOKEN_MAX);

    ToolResult result;
    result.error   = 0;
    result.content = NULL;
    result.len     = 0;

    if (ntok < 1) {
        free(resp);
        return make_error(arena, "mcp_call_tool: parse error");
    }

    /* Check for top-level "error" field (JSON-RPC error). */
    int err_idx = find_key(tokens, ntok, 0, resp, "error");
    if (err_idx >= 0) {
        /* Try to get the error message. */
        int msg_idx = find_key(tokens, ntok, err_idx, resp, "message");
        char errmsg[256] = "mcp tool error";
        if (msg_idx >= 0)
            tok_copy_str(tokens, msg_idx, resp, errmsg, sizeof(errmsg));
        char *copy = arena_alloc(arena, strlen(errmsg) + 1);
        if (copy) memcpy(copy, errmsg, strlen(errmsg) + 1);
        free(resp);
        result.error   = 1;
        result.content = copy ? copy : (char *)"mcp tool error";
        result.len     = copy ? strlen(errmsg) : 14;
        return result;
    }

    /* Extract result.content array — concatenate all "text" type items. */
    int result_idx = find_key(tokens, ntok, 0, resp, "result");
    if (result_idx < 0) {
        free(resp);
        return make_error(arena, "mcp_call_tool: no result field");
    }

    /* Check result.isError */
    int iserr_idx = find_key(tokens, ntok, result_idx, resp, "isError");
    if (iserr_idx >= 0) {
        int is  = tokens[iserr_idx].end - tokens[iserr_idx].start;
        const char *p = resp + tokens[iserr_idx].start;
        if (is >= 4 && memcmp(p, "true", 4) == 0)
            result.error = 1;
    }

    int content_idx = find_key(tokens, ntok, result_idx, resp, "content");
    if (content_idx < 0 || tokens[content_idx].type != JSMN_ARRAY) {
        /* No content array — return empty success. */
        char *empty = arena_alloc(arena, 1);
        if (empty) empty[0] = '\0';
        free(resp);
        result.content = empty ? empty : (char *)"";
        result.len     = 0;
        return result;
    }

    /* Accumulate text content items into a Buf, then copy to arena. */
    Buf out;
    buf_init(&out);

    int ci = content_idx + 1;
    int n_content = tokens[content_idx].size;
    for (int k = 0; k < n_content; k++) {
        if (ci >= ntok || tokens[ci].type != JSMN_OBJECT) break;
        int type_idx = find_key(tokens, ntok, ci, resp, "type");
        if (type_idx >= 0 && tok_str_eq(tokens, type_idx, resp, "text")) {
            int text_idx = find_key(tokens, ntok, ci, resp, "text");
            if (text_idx >= 0 && tokens[text_idx].type == JSMN_STRING) {
                int tlen = tokens[text_idx].end - tokens[text_idx].start;
                if (out.len > 0) buf_append_str(&out, "\n");
                buf_append(&out, resp + tokens[text_idx].start, (size_t)tlen);
            }
        }
        ci = tok_skip(tokens, ntok, ci);
    }

    /* Copy accumulated text into arena. */
    size_t outlen = out.len;
    char  *outbuf = arena_alloc(arena, outlen + 1);
    if (outbuf) {
        if (outlen > 0) memcpy(outbuf, buf_str(&out), outlen);
        outbuf[outlen] = '\0';
    }
    buf_destroy(&out);
    free(resp);

    result.content = outbuf ? outbuf : (char *)"";
    result.len     = outbuf ? outlen : 0;
    return result;
#endif /* __SANITIZE_ADDRESS__ */
}

void mcp_client_free(MCPClient *c)
{
    if (!c) return;
#ifndef __SANITIZE_ADDRESS__
    if (c->stdin_fd  >= 0) { close(c->stdin_fd);  c->stdin_fd  = -1; }
    if (c->stdout_fd >= 0) { close(c->stdout_fd); c->stdout_fd = -1; }
    if (c->child_pid > 0) {
        kill(c->child_pid, SIGTERM);
        /* Brief wait; if the server doesn't exit cleanly, force kill. */
        int   status;
        pid_t waited = waitpid(c->child_pid, &status, WNOHANG);
        if (waited == 0) {
            /* Give it a moment, then SIGKILL. */
            struct timeval tv = {0, 100000}; /* 100 ms */
            select(0, NULL, NULL, NULL, &tv);
            kill(c->child_pid, SIGKILL);
            waitpid(c->child_pid, &status, 0);
        }
        c->child_pid = 0;
    }
#endif
    free(c);
}

/* -------------------------------------------------------------------------
 * Slot dispatch
 * ---------------------------------------------------------------------- */

static ToolResult mcp_slot_dispatch(int slot, Arena *arena,
                                    const char *args_json)
{
    if (slot < 0 || slot >= s_slot_count)
        return make_error(arena, "mcp_slot_dispatch: invalid slot");

    MCPSlot *s = &s_slots[slot];
    return mcp_call_tool(s->client, arena, s->tool_name, args_json);
}

/* -------------------------------------------------------------------------
 * Config loading (mcp.json)
 * ---------------------------------------------------------------------- */

/*
 * Read the file at `path` into a malloc-allocated buffer.
 * Returns the buffer on success (caller must free), NULL on failure.
 * `out_len` is set to the file length.
 */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Buf b;
    buf_init(&b);

    char chunk[1024];
    size_t n;
    size_t total = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0 &&
           total < MCP_CONFIG_MAX) {
        buf_append(&b, chunk, n);
        total += n;
    }
    fclose(fp);

    size_t len  = b.len;
    char  *data = malloc(len + 1);
    if (data) {
        if (len > 0) memcpy(data, buf_str(&b), len);
        data[len] = '\0';
    }
    buf_destroy(&b);

    if (out_len) *out_len = len;
    return data;
}

/*
 * Parse mcp.json content and register all configured MCP servers.
 * Format:
 *   { "servers": [ {"name":"...", "command":"...", "args":["..."]}, ... ] }
 */
static void parse_and_register(const char *json, size_t json_len)
{
    jsmntok_t tokens[MCP_TOKEN_MAX];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, json, json_len, tokens, MCP_TOKEN_MAX);
    if (ntok < 1) return;

    /* servers array */
    int servers_idx = find_key(tokens, ntok, 0, json, "servers");
    if (servers_idx < 0 || tokens[servers_idx].type != JSMN_ARRAY) return;

    int n_servers = tokens[servers_idx].size;
    int si = servers_idx + 1;

    for (int i = 0; i < n_servers && s_client_count < MCP_SERVERS_MAX; i++) {
        if (si >= ntok || tokens[si].type != JSMN_OBJECT) break;

        /* Extract "command". */
        char cmd[256] = "";
        int cmd_idx = find_key(tokens, ntok, si, json, "command");
        if (cmd_idx < 0) { si = tok_skip(tokens, ntok, si); continue; }
        tok_copy_str(tokens, cmd_idx, json, cmd, sizeof(cmd));
        if (cmd[0] == '\0') { si = tok_skip(tokens, ntok, si); continue; }

        /* Build argv from "command" + "args". */
        char *argv_store[MCP_ARGS_MAX + 1]; /* local storage for arg strings */
        char  argv_bufs[MCP_ARGS_MAX][256];
        int   argc = 0;

        argv_store[argc] = cmd;
        argc++;

        int args_idx = find_key(tokens, ntok, si, json, "args");
        if (args_idx >= 0 && tokens[args_idx].type == JSMN_ARRAY) {
            int n_args = tokens[args_idx].size;
            int ai = args_idx + 1;
            for (int j = 0; j < n_args && argc < MCP_ARGS_MAX; j++) {
                if (ai >= ntok) break;
                if (tokens[ai].type == JSMN_STRING) {
                    tok_copy_str(tokens, ai, json,
                                 argv_bufs[argc - 1], sizeof(argv_bufs[0]));
                    argv_store[argc] = argv_bufs[argc - 1];
                    argc++;
                }
                ai = tok_skip(tokens, ntok, ai);
            }
        }
        argv_store[argc] = NULL;

        /* Spawn and initialise the server. */
        MCPClient *client = mcp_client_new(NULL, cmd, argv_store);
        if (!client) { si = tok_skip(tokens, ntok, si); continue; }
        if (mcp_client_init(client) < 0) {
            mcp_client_free(client);
            si = tok_skip(tokens, ntok, si);
            continue;
        }

        /* Discover tools and register them. */
        Arena *tmp = arena_new(1 << 20); /* 1 MB scratch arena */
        if (!tmp) {
            mcp_client_free(client);
            si = tok_skip(tokens, ntok, si);
            continue;
        }

        ToolList *tl = mcp_list_tools(client, tmp);
        if (tl) {
            for (int t = 0; t < tl->count && s_slot_count < MCP_MAX_TOTAL_TOOLS; t++) {
                MCPToolEntry *e = &tl->tools[t];
                if (e->name[0] == '\0') continue;

                MCPSlot *slot = &s_slots[s_slot_count];
                slot->client = client;
                memcpy(slot->tool_name,  e->name,        sizeof(slot->tool_name));
                memcpy(slot->schema_json, e->schema_json, sizeof(slot->schema_json));

                tool_register(slot->tool_name,
                              slot->schema_json,
                              s_slot_fns[s_slot_count]);
                s_slot_count++;
            }
        }

        arena_free(tmp);

        /* Keep the client alive in the persistent list. */
        s_clients[s_client_count++] = client;
        si = tok_skip(tokens, ntok, si);
    }
}

void mcp_tools_register(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    /* Build path: $HOME/.nanocode/mcp.json */
    char path[512];
    snprintf(path, sizeof(path), "%s/.nanocode/mcp.json", home);

    size_t json_len = 0;
    char  *json     = read_file(path, &json_len);
    if (!json) return; /* file absent — silently do nothing */

    parse_and_register(json, json_len);
    free(json);
}
