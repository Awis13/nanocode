/*
 * provider.c — AI provider abstraction implementation
 *
 * Builds the JSON request body, fires off an async HTTP request,
 * feeds the response through the SSE parser, and extracts tokens
 * from each JSON event.
 *
 * Claude stream event format:
 *   data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"..."}}
 *
 * OpenAI/Ollama stream event format:
 *   data: {"choices":[{"delta":{"content":"..."}}]}
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

typedef struct {
    ProviderType      type;
    provider_token_cb on_token;
    provider_done_cb  on_done;
    void             *ctx;
    SseParser        *sse;
    int               done;
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
         *  - {"type":"content_block_delta","delta":{"type":"text_delta","text":"..."}}
         *  - {"type":"message_stop"}  (end of stream)
         */
        char type_val[64];
        if (json_get_str(&jctx, data, "type",
                         type_val, sizeof(type_val)) == 0) {
            if (strcmp(type_val, "message_stop") == 0) {
                sc->done = 1;
                return 0;
            }
            if (strcmp(type_val, "content_block_delta") == 0) {
                if (json_get_nested_str(&jctx, data,
                                        "delta", "text",
                                        text, sizeof(text)) == 0) {
                    sc->on_token(text, strlen(text), sc->ctx);
                }
            }
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
 * HTTP callbacks
 * ---------------------------------------------------------------------- */

static int on_http_body(const char *data, size_t len, void *ctx)
{
    StreamCtx *sc = ctx;
    return sse_parser_feed(sc->sse, data, len);
}

static void on_http_done(int status, void *ctx)
{
    StreamCtx *sc = ctx;
    int err = (status == 0 || (status >= 400));
    if (sc->on_done)
        sc->on_done(err, sc->ctx);
    sse_parser_free(sc->sse);
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
                              const Message *msgs, int nmsg)
{
    buf_append_str(b, "{\"model\":\"");
    json_escape(b, model);
    buf_append_str(b, "\",\"max_tokens\":4096,\"stream\":true,\"messages\":[");
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
    buf_append_str(b, "\",\"stream\":true,\"messages\":[");
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
                    provider_done_cb  on_done,
                    void *ctx)
{
    /* Build request body */
    buf_reset(&p->body_buf);
    int r;
    if (p->cfg.type == PROVIDER_CLAUDE)
        r = build_claude_body(&p->body_buf, p->cfg.model, msgs, nmsg);
    else
        r = build_openai_body(&p->body_buf, p->cfg.model, msgs, nmsg);
    if (r < 0)
        return -1;

    /* Set up stream context */
    StreamCtx *sc = calloc(1, sizeof(StreamCtx));
    if (!sc)
        return -1;
    sc->type     = p->cfg.type;
    sc->on_token = on_token;
    sc->on_done  = on_done;
    sc->ctx      = ctx;
    sc->done     = 0;
    sc->sse      = sse_parser_new(on_sse_event, sc);
    if (!sc->sse) {
        free(sc);
        return -1;
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
