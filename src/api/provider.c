/*
 * provider.c — AI provider abstraction implementation
 *
 * Builds the JSON request body, fires off an async HTTP request,
 * feeds the response through the SSE parser (PROVIDER_CLAUDE/OPENAI) or
 * an NDJSON line parser (PROVIDER_OLLAMA), and extracts text tokens.
 *
 * Claude stream event format (SSE):
 *   data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"..."}}
 *
 * OpenAI stream event format (SSE):
 *   data: {"choices":[{"delta":{"content":"..."}}]}
 *
 * Ollama native stream format (NDJSON):
 *   {"message":{"content":"..."},"done":false}
 *   {"message":{"content":""},"done":true,"done_reason":"stop"}
 */

#include "provider.h"
#include "client.h"
#include "sse.h"
#include "../util/buf.h"
#include "../util/json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define JSON_TOKEN_MAX 256

/* -------------------------------------------------------------------------
 * Stream context
 * ---------------------------------------------------------------------- */

/* Content block type for Claude streaming — tracks which block is active. */
typedef enum {
    BLOCK_NONE = 0,
    BLOCK_TEXT,
    BLOCK_TOOL_USE,
    BLOCK_THINKING,
} BlockType;

typedef struct {
    ProviderType      type;
    provider_token_cb on_token;
    provider_tool_cb  on_tool;
    provider_done_cb  on_done;
    void             *ctx;
    SseParser        *sse;        /* NULL for PROVIDER_OLLAMA */
    Buf               line_buf;   /* for PROVIDER_OLLAMA NDJSON line assembly */
    int               done;
    /* Claude stop_reason from message_delta event */
    char              stop_reason[32]; /* "end_turn", "max_tokens", "tool_use", "" */
    /* Claude tool_use block tracking */
    BlockType         cur_block;
    char              cur_tool_id[128];
    char              cur_tool_name[128];
    Buf               cur_tool_input; /* accumulates input_json_delta */
} StreamCtx;

/* -------------------------------------------------------------------------
 * SSE event handler — parse JSON and extract the text delta
 * ---------------------------------------------------------------------- */

static int on_sse_event(const char *data, size_t len, void *ctx)
{
    StreamCtx *sc = ctx;

    JsonCtx jctx;
    if (json_parse_ctx(&jctx, data, len) < 0)
        return 0; /* malformed JSON — skip */

    char text[4096];

    if (sc->type == PROVIDER_CLAUDE) {
        /*
         * Claude Messages API streaming events:
         *   content_block_start  — starts text, tool_use, or thinking block
         *   content_block_delta  — text_delta, input_json_delta, thinking_delta
         *   content_block_stop   — fires on_tool callback for tool_use blocks
         *   message_stop         — marks stream complete
         */
        char type_val[64];
        if (json_get_str(&jctx, data, "type",
                         type_val, sizeof(type_val)) != 0)
            return 0;

        if (strcmp(type_val, "message_stop") == 0) {
            sc->done = 1;
            return 0;
        }

        if (strcmp(type_val, "message_delta") == 0) {
            /* Capture stop_reason for the caller. */
            json_get_nested_str(&jctx, data, "delta", "stop_reason",
                                sc->stop_reason, sizeof(sc->stop_reason));
            return 0;
        }

        if (strcmp(type_val, "content_block_start") == 0) {
            char block_type[32];
            if (json_get_nested_str(&jctx, data, "content_block", "type",
                                    block_type, sizeof(block_type)) != 0)
                return 0;
            if (strcmp(block_type, "text") == 0) {
                sc->cur_block = BLOCK_TEXT;
            } else if (strcmp(block_type, "tool_use") == 0) {
                sc->cur_block = BLOCK_TOOL_USE;
                json_get_nested_str(&jctx, data, "content_block", "id",
                                    sc->cur_tool_id, sizeof(sc->cur_tool_id));
                json_get_nested_str(&jctx, data, "content_block", "name",
                                    sc->cur_tool_name, sizeof(sc->cur_tool_name));
                buf_reset(&sc->cur_tool_input);
            } else if (strcmp(block_type, "thinking") == 0) {
                sc->cur_block = BLOCK_THINKING;
            }
            return 0;
        }

        if (strcmp(type_val, "content_block_delta") == 0) {
            char delta_type[32];
            if (json_get_nested_str(&jctx, data, "delta", "type",
                                    delta_type, sizeof(delta_type)) != 0)
                return 0;
            if (strcmp(delta_type, "text_delta") == 0) {
                if (sc->on_token &&
                    json_get_nested_str(&jctx, data, "delta", "text",
                                        text, sizeof(text)) == 0) {
                    sc->on_token(text, strlen(text), sc->ctx);
                }
            } else if (strcmp(delta_type, "input_json_delta") == 0) {
                if (sc->cur_block == BLOCK_TOOL_USE &&
                    json_get_nested_str(&jctx, data, "delta", "partial_json",
                                        text, sizeof(text)) == 0) {
                    buf_append_str(&sc->cur_tool_input, text);
                }
            }
            /* thinking_delta — intentionally ignored */
            return 0;
        }

        if (strcmp(type_val, "content_block_stop") == 0) {
            if (sc->cur_block == BLOCK_TOOL_USE && sc->on_tool) {
                /* NUL-terminate the accumulated input */
                buf_append(&sc->cur_tool_input, "", 1);
                sc->on_tool(sc->cur_tool_id, sc->cur_tool_name,
                            sc->cur_tool_input.data
                                ? sc->cur_tool_input.data : "",
                            sc->ctx);
                buf_reset(&sc->cur_tool_input);
            }
            sc->cur_block = BLOCK_NONE;
            return 0;
        }
    } else {
        /*
         * OpenAI / Ollama /v1/chat/completions format:
         *  {"choices":[{"delta":{"content":"..."}}]}
         */
        if (json_get_array_item_nested_str(&jctx, data,
                                           "choices", "delta", "content",
                                           text, sizeof(text)) == 0) {
            if (strlen(text) > 0)
                sc->on_token(text, strlen(text), sc->ctx);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Ollama NDJSON stream handler
 *
 * Each response line is a complete JSON object:
 *   {"message":{"content":"..."},"done":false}
 *   {"message":{"content":""},"done":true}
 * ---------------------------------------------------------------------- */

static void ollama_process_line(StreamCtx *sc, const char *line, size_t len)
{
    if (len == 0)
        return;

    JsonCtx jctx;
    if (json_parse_ctx(&jctx, line, len) < 0)
        return;

    /* Extract done flag first */
    char done_val[8];
    if (json_get_str(&jctx, line, "done", done_val, sizeof(done_val)) == 0) {
        if (strcmp(done_val, "true") == 0) {
            sc->done = 1;
            return;
        }
    }

    /* Extract message.content */
    char text[4096];
    if (json_get_nested_str(&jctx, line, "message", "content",
                            text, sizeof(text)) == 0) {
        if (strlen(text) > 0)
            sc->on_token(text, strlen(text), sc->ctx);
    }
}

static int on_ollama_body(const char *data, size_t len, void *ctx)
{
    StreamCtx *sc = ctx;
    const char *p   = data;
    const char *end = data + len;

    /* Scan for newlines chunk-wise rather than byte-by-byte */
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            /* No newline in remainder — buffer it all */
            if (buf_append(&sc->line_buf, p, (size_t)(end - p)) < 0)
                return -1;
            break;
        }
        /* Append [p, nl) to line_buf, then process the complete line */
        if (nl > p) {
            if (buf_append(&sc->line_buf, p, (size_t)(nl - p)) < 0)
                return -1;
        }
        ollama_process_line(sc, sc->line_buf.data, sc->line_buf.len);
        buf_reset(&sc->line_buf);
        if (sc->done)
            return 0;
        p = nl + 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * HTTP callbacks
 * ---------------------------------------------------------------------- */

static int on_http_body(const char *data, size_t len, void *ctx)
{
    StreamCtx *sc = ctx;
    if (sc->type == PROVIDER_OLLAMA)
        return on_ollama_body(data, len, ctx);
    return sse_parser_feed(sc->sse, data, len);
}

static void on_http_done(int status, void *ctx)
{
    StreamCtx *sc = ctx;
    int err = (status == 0 || (status >= 400));
    /* Pass stop_reason only on success; NULL on error. */
    const char *stop_reason = (!err && sc->stop_reason[0]) ? sc->stop_reason : NULL;
    if (sc->on_done)
        sc->on_done(err, stop_reason, sc->ctx);
    if (sc->sse)
        sse_parser_free(sc->sse);
    buf_destroy(&sc->line_buf);
    buf_destroy(&sc->cur_tool_input);
    free(sc);
}

/* -------------------------------------------------------------------------
 * JSON body builders
 * ---------------------------------------------------------------------- */

/* Simple JSON string escaping: only handle the most common cases. */
static void json_escape(Buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '"':  buf_append_str(b, "\\\""); break;
        case '\\': buf_append_str(b, "\\\\"); break;
        case '\n': buf_append_str(b, "\\n");  break;
        case '\r': buf_append_str(b, "\\r");  break;
        case '\t': buf_append_str(b, "\\t");  break;
        default:
            buf_append(b, s, 1);
        }
    }
}

static int build_claude_body(Buf *b, const char *model,
                              const Message *msgs, int nmsg,
                              int thinking_budget)
{
    buf_append_str(b, "{\"model\":\"");
    json_escape(b, model);

    /* Extended thinking: budget_tokens controls allocation; max_tokens must
     * be at least budget + 1000 to leave room for the visible response. */
    int max_tokens = 4096;
    if (thinking_budget > 0) {
        max_tokens = thinking_budget + 4096;
        buf_append_str(b, "\",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":");
        char num[32];
        snprintf(num, sizeof(num), "%d", thinking_budget);
        buf_append_str(b, num);
        buf_append_str(b, "}");
    } else {
        buf_append_str(b, "\"");
    }

    char max_tokens_str[32];
    snprintf(max_tokens_str, sizeof(max_tokens_str), "%d", max_tokens);
    buf_append_str(b, ",\"max_tokens\":");
    buf_append_str(b, max_tokens_str);
    buf_append_str(b, ",\"stream\":true");

    /* Scan for a system role message and emit it as the top-level field. */
    for (int i = 0; i < nmsg; i++) {
        if (msgs[i].role && strcmp(msgs[i].role, "system") == 0) {
            buf_append_str(b, ",\"system\":\"");
            json_escape(b, msgs[i].content ? msgs[i].content : "");
            buf_append_str(b, "\"");
            break; /* Only the first system message is used. */
        }
    }

    buf_append_str(b, ",\"messages\":[");
    int written = 0;
    for (int i = 0; i < nmsg; i++) {
        if (msgs[i].role && strcmp(msgs[i].role, "system") == 0)
            continue; /* system messages are emitted as top-level field */
        if (written > 0) buf_append_str(b, ",");
        buf_append_str(b, "{\"role\":\"");
        json_escape(b, msgs[i].role ? msgs[i].role : "");
        buf_append_str(b, "\",\"content\":");
        /* Content starting with '[' is already a JSON array — emit verbatim. */
        const char *c = msgs[i].content ? msgs[i].content : "";
        if (c[0] == '[') {
            buf_append_str(b, c);
        } else {
            buf_append_str(b, "\"");
            json_escape(b, c);
            buf_append_str(b, "\"");
        }
        buf_append_str(b, "}");
        written++;
    }
    return buf_append_str(b, "]}");
}

static int build_ollama_body(Buf *b, const char *model,
                              const Message *msgs, int nmsg)
{
    buf_append_str(b, "{\"model\":\"");
    json_escape(b, model);
    /* think:false disables extended reasoning in qwen3/qwen3.5 */
    buf_append_str(b, "\",\"stream\":true,\"think\":false,\"messages\":[");
    for (int i = 0; i < nmsg; i++) {
        if (i > 0) buf_append_str(b, ",");
        buf_append_str(b, "{\"role\":\"");
        json_escape(b, msgs[i].role);
        buf_append_str(b, "\",\"content\":\"");
        json_escape(b, msgs[i].content);
        buf_append_str(b, "\"}");
    }
    return buf_append_str(b, "]}");
}

static int build_openai_body(Buf *b, const char *model,
                              const Message *msgs, int nmsg)
{
    buf_append_str(b, "{\"model\":\"");
    json_escape(b, model);
    /* think:false disables extended reasoning on Ollama's qwen3/qwen3.5 */
    buf_append_str(b, "\",\"stream\":true,\"think\":false,\"messages\":[");
    for (int i = 0; i < nmsg; i++) {
        if (i > 0) buf_append_str(b, ",");
        buf_append_str(b, "{\"role\":\"");
        json_escape(b, msgs[i].role);
        buf_append_str(b, "\",\"content\":\"");
        json_escape(b, msgs[i].content);
        buf_append_str(b, "\"}");
    }
    return buf_append_str(b, "]}");
}

/* -------------------------------------------------------------------------
 * Provider struct
 * ---------------------------------------------------------------------- */

struct Provider {
    Loop          *loop;
    ProviderConfig cfg;
    HttpClient    *http;
    Buf            body_buf;   /* reused across requests */
};

Provider *provider_new(Loop *loop, const ProviderConfig *cfg)
{
    Provider *p = calloc(1, sizeof(Provider));
    if (!p)
        return NULL;
    p->loop = loop;
    p->cfg  = *cfg;
    p->http = http_client_new(loop);
    if (!p->http) {
        free(p);
        return NULL;
    }
    buf_init(&p->body_buf);
    return p;
}

void provider_free(Provider *p)
{
    http_client_free(p->http);
    buf_destroy(&p->body_buf);
    free(p);
}

int provider_stream(Provider *p,
                    const Message *msgs, int nmsg,
                    provider_token_cb on_token,
                    provider_tool_cb  on_tool,
                    provider_done_cb  on_done,
                    void *ctx)
{
    /* Build request body */
    buf_reset(&p->body_buf);
    int r;
    if (p->cfg.type == PROVIDER_CLAUDE)
        r = build_claude_body(&p->body_buf, p->cfg.model, msgs, nmsg,
                              p->cfg.thinking_budget);
    else if (p->cfg.type == PROVIDER_OLLAMA)
        r = build_ollama_body(&p->body_buf, p->cfg.model, msgs, nmsg);
    else
        r = build_openai_body(&p->body_buf, p->cfg.model, msgs, nmsg);
    if (r < 0)
        return -1;

    /* Set up stream context */
    StreamCtx *sc = calloc(1, sizeof(StreamCtx));
    if (!sc)
        return -1;
    sc->type      = p->cfg.type;
    sc->on_token  = on_token;
    sc->on_tool   = on_tool;
    sc->on_done   = on_done;
    sc->ctx       = ctx;
    sc->done      = 0;
    sc->cur_block = BLOCK_NONE;
    buf_init(&sc->line_buf);
    buf_init(&sc->cur_tool_input);

    if (p->cfg.type == PROVIDER_OLLAMA) {
        sc->sse = NULL;
    } else {
        sc->sse = sse_parser_new(on_sse_event, sc);
        if (!sc->sse) {
            buf_destroy(&sc->cur_tool_input);
            free(sc);
            return -1;
        }
    }

    /* Build HTTP request */
    HttpRequest req = {0};
    req.method   = HTTP_POST;
    req.host     = p->cfg.base_url;
    req.port     = p->cfg.port;
    req.use_tls  = p->cfg.use_tls;
    req.on_body  = on_http_body;
    req.on_done  = on_http_done;
    req.cb_ctx   = sc;
    req.body     = p->body_buf.data;
    req.body_len = p->body_buf.len;
    req.nheaders = 0;

    /* Set path and content-type */
    if (p->cfg.type == PROVIDER_CLAUDE)
        req.path = "/v1/messages";
    else if (p->cfg.type == PROVIDER_OLLAMA)
        req.path = "/api/chat";
    else
        req.path = "/v1/chat/completions";

    /* Add headers */
    req.headers[req.nheaders].name  = "Content-Type";
    req.headers[req.nheaders].value = "application/json";
    req.nheaders++;

    if (p->cfg.api_key) {
        if (p->cfg.type == PROVIDER_CLAUDE) {
            req.headers[req.nheaders].name  = "x-api-key";
            req.headers[req.nheaders].value = p->cfg.api_key;
            req.nheaders++;
            req.headers[req.nheaders].name  = "anthropic-version";
            req.headers[req.nheaders].value = "2023-06-01";
            req.nheaders++;
        } else {
            /* Static buffer for the Authorization header value */
            static char auth_hdr[256];
            snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", p->cfg.api_key);
            req.headers[req.nheaders].name  = "Authorization";
            req.headers[req.nheaders].value = auth_hdr;
            req.nheaders++;
        }
    }

    return http_client_request(p->http, &req);
}
