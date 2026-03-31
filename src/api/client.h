/*
 * client.h — async HTTP/1.1 client
 *
 * Supports both plain TCP (e.g. localhost Ollama) and TLS via BearSSL.
 * All I/O is non-blocking and wired through an event loop.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>
#include "../core/loop.h"
#include "../util/buf.h"

/* Maximum number of headers per request/response. */
#define HTTP_MAX_HEADERS 32

/* HTTP method constants. */
#define HTTP_POST "POST"
#define HTTP_GET  "GET"

typedef struct HttpClient HttpClient;

/*
 * Called for each chunk of response body received.
 * `data` is NOT NUL-terminated; `len` bytes are valid.
 * Return 0 to continue, -1 to abort.
 */
typedef int (*http_body_cb)(const char *data, size_t len, void *ctx);

/*
 * Called once when the response is complete (or on error).
 * `status` is the HTTP status code (0 if connection failed).
 */
typedef void (*http_done_cb)(int status, void *ctx);

typedef struct {
    const char *name;
    const char *value;
} HttpHeader;

typedef struct {
    const char  *method;             /* HTTP_POST, HTTP_GET, etc. */
    const char  *host;               /* hostname (e.g. "api.anthropic.com") */
    int          port;               /* 443 for TLS, 11434 for Ollama */
    const char  *path;               /* URL path (e.g. "/v1/messages") */
    int          use_tls;            /* 1 = TLS, 0 = plain TCP */
    HttpHeader   headers[HTTP_MAX_HEADERS];
    int          nheaders;
    const char  *body;               /* request body (may be NULL) */
    size_t       body_len;
    http_body_cb on_body;            /* called for each response chunk */
    http_done_cb on_done;            /* called on completion/error */
    void        *cb_ctx;             /* passed to callbacks */
} HttpRequest;

/* Create a new HTTP client bound to `loop`. Returns NULL on failure. */
HttpClient *http_client_new(Loop *loop);

/* Destroy the client (does NOT free the loop). */
void        http_client_free(HttpClient *c);

/*
 * Begin an async HTTP request. Non-blocking; callbacks fire as data arrives.
 * Returns 0 if the request was started, -1 on immediate error.
 */
int         http_client_request(HttpClient *c, const HttpRequest *req);

#endif /* CLIENT_H */
