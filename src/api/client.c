/*
 * client.c — async HTTP/1.1 client over plain TCP or BearSSL TLS
 *
 * State machine:
 *   CONNECTING → (if TLS) TLS_HANDSHAKE → SENDING → RECEIVING → DONE
 *
 * TLS I/O model:
 *   BearSSL's "push/pull" wrappers are driven manually: we call the
 *   engine and push/pull raw bytes between it and the socket ourselves,
 *   rather than using the high-level io_context helpers. This gives us
 *   full control over non-blocking behaviour.
 *
 * X.509 validation (CMP-145):
 *   System CA bundle is loaded at TLS connection start via tls_ca_load().
 *   If no bundle is found, falls back to the no-verify vtable with a
 *   DEBUG-level warning.
 *
 * Retry (CMP-145):
 *   On HTTP 429 / 503, retry_should_retry() / retry_next_delay_ms() drive
 *   exponential backoff.  Delays are scheduled with loop_add_timer();
 *   state lives in HttpClient so the Conn is fully released between
 *   attempts.
 */

#include "client.h"
#include "retry.h"
#include "tls_ca.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "bearssl.h"

/* Default connection + response timeout when HttpRequest.timeout_ms == 0. */
#define DEFAULT_TIMEOUT_MS 30000

/* -------------------------------------------------------------------------
 * BearSSL: no-verify X.509 validator
 * Fallback when no system CA bundle is found (dev/test mode).
 * ---------------------------------------------------------------------- */

static void x509_start_chain(const br_x509_class **ctx,
                              const char *server_name)
{
    (void)ctx; (void)server_name;
}

static void x509_start_cert(const br_x509_class **ctx, uint32_t length)
{
    (void)ctx; (void)length;
}

static void x509_append(const br_x509_class **ctx,
                        const unsigned char *buf, size_t len)
{
    (void)ctx; (void)buf; (void)len;
}

static void x509_end_cert(const br_x509_class **ctx)
{
    (void)ctx;
}

static unsigned x509_end_chain(const br_x509_class **ctx)
{
    (void)ctx;
    return 0; /* always accept */
}

static const br_x509_pkey *x509_get_pkey(const br_x509_class *const *ctx,
                                          unsigned *usages)
{
    (void)ctx;
    if (usages)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return NULL;
}

static const br_x509_class noverify_x509_vtable = {
    sizeof(br_x509_class *),
    x509_start_chain,
    x509_start_cert,
    x509_append,
    x509_end_cert,
    x509_end_chain,
    x509_get_pkey,
};

/* -------------------------------------------------------------------------
 * Connection state
 * ---------------------------------------------------------------------- */

typedef enum {
    CS_CONNECTING,
    CS_TLS_HANDSHAKE,
    CS_SENDING,
    CS_RECEIVING_HEADERS,
    CS_RECEIVING_BODY,
    CS_DONE,
    CS_ERROR,
} ConnState;

/* TLS I/O buffers: standard full-duplex sizes per BearSSL docs. */
#define TLS_IO_BUFSIZE BR_SSL_BUFSIZE_BIDI

struct HttpClient; /* forward */

typedef struct {
    struct HttpClient   *client;    /* back-pointer for retry scheduling */
    Loop                *loop;
    int                  fd;
    ConnState            state;

    /* Request being processed */
    HttpRequest          req;

    /* Request serialisation */
    Buf                  req_buf;
    size_t               req_sent;

    /* Response parsing */
    Buf                  resp_buf;
    int                  resp_status;
    int                  headers_done;
    int                  chunked;
    int                  content_length;
    int                  retry_after_s; /* from Retry-After header; -1 absent */

    /* TLS */
    int                      use_tls;
    br_ssl_client_context    ssl_ctx;
    br_x509_minimal_context  x509_minimal; /* real cert validation */
    const br_x509_class     *x509_noverify;
    br_x509_trust_anchor    *trust_anchors;    /* owned; freed on cleanup */
    size_t                   num_trust_anchors;
    unsigned char            tls_buf[TLS_IO_BUFSIZE];

    /* Timeout timer (loop_add_timer id; -1 if none) */
    int                  timeout_timer;

} Conn;

struct HttpClient {
    Loop *loop;
    Conn *active;

    /* Pending retry state — lives here so Conn can be freed between attempts */
    int          retry_pending;
    int          retry_timer;      /* loop_add_timer id; -1 if none */
    int          retry_attempt;    /* 0-based, incremented after each failure */
    int          retry_after_s;    /* carried from last Retry-After header */
    RetryConfig  retry_cfg;
    HttpRequest  retry_req;        /* shallow copy of caller's request */
};

/* -------------------------------------------------------------------------
 * Conn cleanup helper — releases all resources owned by Conn itself.
 * The Conn struct is freed; pointer becomes invalid after this call.
 * ---------------------------------------------------------------------- */

static void conn_destroy(Conn *c)
{
    if (!c)
        return;
    /* Cancel pending timeout timer */
    if (c->timeout_timer >= 0) {
        loop_cancel_timer(c->loop, c->timeout_timer);
        c->timeout_timer = -1;
    }
    if (c->fd >= 0) {
        loop_remove_fd(c->loop, c->fd);
        close(c->fd);
        c->fd = -1;
    }
    tls_ca_free(c->trust_anchors, c->num_trust_anchors);
    buf_destroy(&c->req_buf);
    buf_destroy(&c->resp_buf);
    free(c);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/*
 * conn_finish: called when a response is fully received or an error
 * occurred.  Decides whether to retry (429/503) or deliver final result.
 *
 * NOTE: conn_finish is called from within conn_io callbacks, so the Conn
 * must not be freed while the call stack still references it.  We set
 * c->state = CS_DONE and defer the free to after conn_io returns.
 * For retries, we save state to HttpClient before conn_io unregisters.
 */
static void conn_finish(Conn *c, int status);
static void conn_error(Conn *c);

static int retry_timer_cb(int id, int events, void *ctx);

static void conn_finish(Conn *c, int status)
{
    c->state = CS_DONE;

    /* Cancel timeout timer — connection resolved before it fired. */
    if (c->timeout_timer >= 0) {
        loop_cancel_timer(c->loop, c->timeout_timer);
        c->timeout_timer = -1;
    }

    HttpClient *cl = c->client;

    /* Check whether we should retry */
    if (retry_should_retry(status)) {
        int delay = retry_next_delay_ms(&cl->retry_cfg,
                                        cl->retry_attempt,
                                        c->retry_after_s);
        if (delay >= 0) {
            fprintf(stderr,
                    "client: [DEBUG] HTTP %d — retry %d after %d ms\n",
                    status, cl->retry_attempt + 1, delay);
            /* Save retry state in client before freeing Conn. */
            cl->retry_attempt++;
            cl->retry_after_s  = c->retry_after_s;
            cl->retry_req      = c->req; /* shallow copy; caller owns strings */
            cl->retry_pending  = 1;
            /* conn_io will close/free the conn after we return (it sees
             * CS_DONE and returns -1, triggering loop_remove_fd).
             * We do NOT call conn_destroy here — that happens in conn_io. */
            cl->retry_timer = loop_add_timer(c->loop, delay,
                                             retry_timer_cb, cl);
            return;
        }
        /* Retries exhausted — fall through to deliver final error status. */
        fprintf(stderr,
                "client: [DEBUG] HTTP %d — retries exhausted, giving up\n",
                status);
    }

    /* Final result (success or non-retryable / exhausted). */
    if (c->req.on_done)
        c->req.on_done(status, c->req.cb_ctx);
    /* conn_io will clean up fd/entry after we return. */
}

static void conn_error(Conn *c)
{
    conn_finish(c, 0);
}

/* -------------------------------------------------------------------------
 * Retry timer callback — fires after backoff delay
 * ---------------------------------------------------------------------- */

static int retry_timer_cb(int id, int events, void *ctx)
{
    HttpClient *cl = ctx;
    (void)id; (void)events;

    cl->retry_pending = 0;
    cl->retry_timer   = -1;

    /* Free any lingering previous Conn (conn_io may have returned before
     * the timer fired but after conn_destroy was deferred). */
    if (cl->active) {
        conn_destroy(cl->active);
        cl->active = NULL;
    }

    /* Reissue the request; retry_attempt is already incremented. */
    http_client_request(cl, &cl->retry_req);
    return 0; /* loop_step removes us automatically (one-shot timer) */
}

/* -------------------------------------------------------------------------
 * Timeout timer callback
 * ---------------------------------------------------------------------- */

static int timeout_timer_cb(int id, int events, void *ctx)
{
    Conn *c = ctx;
    (void)id; (void)events;
    c->timeout_timer = -1; /* already fired */
    fprintf(stderr, "client: connection timed out\n");
    conn_error(c);
    return 0; /* one-shot: loop removes us */
}

/* -------------------------------------------------------------------------
 * HTTP request serialisation
 * ---------------------------------------------------------------------- */

static int build_request(Conn *c)
{
    const HttpRequest *r = &c->req;
    buf_init(&c->req_buf);

    /* Request line */
    if (buf_append_str(&c->req_buf, r->method) < 0 ||
        buf_append_str(&c->req_buf, " ") < 0 ||
        buf_append_str(&c->req_buf, r->path) < 0 ||
        buf_append_str(&c->req_buf, " HTTP/1.1\r\n") < 0)
        return -1;

    /* Host header */
    {
        char host_hdr[512];
        if (r->port != 80 && r->port != 443)
            snprintf(host_hdr, sizeof(host_hdr), "%s:%d", r->host, r->port);
        else
            snprintf(host_hdr, sizeof(host_hdr), "%s", r->host);
        if (buf_append_str(&c->req_buf, "Host: ") < 0 ||
            buf_append_str(&c->req_buf, host_hdr) < 0 ||
            buf_append_str(&c->req_buf, "\r\n") < 0)
            return -1;
    }

    if (buf_append_str(&c->req_buf, "Connection: close\r\n") < 0)
        return -1;

    for (int i = 0; i < r->nheaders; i++) {
        if (buf_append_str(&c->req_buf, r->headers[i].name) < 0  ||
            buf_append_str(&c->req_buf, ": ") < 0                ||
            buf_append_str(&c->req_buf, r->headers[i].value) < 0 ||
            buf_append_str(&c->req_buf, "\r\n") < 0)
            return -1;
    }

    if (r->body && r->body_len > 0) {
        char cl[32];
        snprintf(cl, sizeof(cl), "%zu", r->body_len);
        if (buf_append_str(&c->req_buf, "Content-Length: ") < 0 ||
            buf_append_str(&c->req_buf, cl) < 0 ||
            buf_append_str(&c->req_buf, "\r\n") < 0)
            return -1;
    }

    if (buf_append_str(&c->req_buf, "\r\n") < 0)
        return -1;

    if (r->body && r->body_len > 0) {
        if (buf_append(&c->req_buf, r->body, r->body_len) < 0)
            return -1;
    }

    c->req_sent = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * HTTP response parsing
 * ---------------------------------------------------------------------- */

static int parse_status_line(const char *line, size_t len)
{
    if (len < 12)
        return -1;
    if (memcmp(line, "HTTP/", 5) != 0)
        return -1;
    const char *sp = memchr(line, ' ', len);
    if (!sp)
        return -1;
    return atoi(sp + 1);
}

/*
 * Scan buf for end-of-headers (\r\n\r\n).
 * Returns pointer to body start, or NULL if headers not yet complete.
 * Also parses Transfer-Encoding, Content-Length, and Retry-After.
 */
static const char *find_body_start(const char *buf, size_t len,
                                   int *status_out,
                                   int *chunked_out,
                                   int *content_length_out,
                                   int *retry_after_out)
{
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            end = buf + i + 4;
            break;
        }
    }
    if (!end)
        return NULL;

    /* Parse status line */
    const char *lf = memchr(buf, '\n', (size_t)(end - buf));
    if (!lf)
        return NULL;
    size_t sl_len = (size_t)(lf - buf);
    if (sl_len > 0 && buf[sl_len - 1] == '\r')
        sl_len--;
    *status_out = parse_status_line(buf, sl_len);

    *chunked_out       = 0;
    *content_length_out = -1;
    *retry_after_out   = -1;

    const char *p = lf + 1;
    while (p < end - 1) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            break;
        size_t hlen = (size_t)(eol - p);
        if (hlen > 0 && p[hlen - 1] == '\r')
            hlen--;

        if (hlen > 19 && strncasecmp(p, "transfer-encoding:", 18) == 0) {
            const char *v = p + 18;
            while (*v == ' ') v++;
            if (strncasecmp(v, "chunked", 7) == 0)
                *chunked_out = 1;
        } else if (hlen > 16 && strncasecmp(p, "content-length:", 15) == 0) {
            *content_length_out = atoi(p + 15);
        } else if (hlen > 13 && strncasecmp(p, "retry-after:", 12) == 0) {
            const char *v = p + 12;
            while (*v == ' ') v++;
            int ra = atoi(v);
            if (ra > 0)
                *retry_after_out = ra;
        }

        p = eol + 1;
    }

    return end;
}

/* -------------------------------------------------------------------------
 * I/O helpers — plain TCP
 * ---------------------------------------------------------------------- */

static ssize_t plain_write(Conn *c, const char *data, size_t len)
{
    return write(c->fd, data, len);
}

static ssize_t plain_read(Conn *c, char *buf, size_t len)
{
    return read(c->fd, buf, len);
}

/* -------------------------------------------------------------------------
 * I/O helpers — BearSSL TLS
 * ---------------------------------------------------------------------- */

static int tls_pump(Conn *c)
{
    br_ssl_engine_context *eng = &c->ssl_ctx.eng;
    int did_something = 0;

    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
        if (!buf || len == 0)
            break;
        ssize_t n = write(c->fd, buf, len);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return -1;
        }
        br_ssl_engine_sendrec_ack(eng, (size_t)n);
        did_something = 1;
    }

    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
        if (!buf || len == 0)
            break;
        ssize_t n = read(c->fd, buf, len);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return -1; /* connection closed or error */
        }
        br_ssl_engine_recvrec_ack(eng, (size_t)n);
        did_something = 1;
    }

    return did_something;
}

static ssize_t tls_write(Conn *c, const char *data, size_t len)
{
    br_ssl_engine_context *eng = &c->ssl_ctx.eng;

    size_t avail;
    unsigned char *app = br_ssl_engine_sendapp_buf(eng, &avail);
    if (!app || avail == 0) {
        tls_pump(c);
        app = br_ssl_engine_sendapp_buf(eng, &avail);
        if (!app || avail == 0)
            return 0;
    }

    size_t to_copy = len < avail ? len : avail;
    memcpy(app, data, to_copy);
    br_ssl_engine_sendapp_ack(eng, to_copy);
    br_ssl_engine_flush(eng, 0);
    tls_pump(c);
    return (ssize_t)to_copy;
}

static ssize_t tls_read(Conn *c, char *buf, size_t len)
{
    br_ssl_engine_context *eng = &c->ssl_ctx.eng;

    tls_pump(c);

    size_t avail;
    unsigned char *app = br_ssl_engine_recvapp_buf(eng, &avail);
    if (!app || avail == 0) {
        errno = EAGAIN;
        return -1;
    }

    size_t to_copy = len < avail ? len : avail;
    memcpy(buf, app, to_copy);
    br_ssl_engine_recvapp_ack(eng, to_copy);
    return (ssize_t)to_copy;
}

/* -------------------------------------------------------------------------
 * Event loop callback
 * ---------------------------------------------------------------------- */

static int conn_io(int fd, int events, void *ctx);

static void conn_start_tls(Conn *c);
static void conn_do_tls_handshake(Conn *c);
static void conn_do_send(Conn *c);
static void conn_do_recv(Conn *c);
static void conn_dispatch_body(Conn *c, const char *data, size_t len);

static int conn_io(int fd, int events, void *ctx)
{
    Conn *c = ctx;
    (void)fd;

    switch (c->state) {
    case CS_CONNECTING:
        {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0
                || err) {
                fprintf(stderr, "client: connect failed: %s\n",
                        err ? strerror(err) : strerror(errno));
                conn_error(c);
                return -1;
            }
            if (c->use_tls) {
                conn_start_tls(c);
            } else {
                if (build_request(c) < 0) {
                    conn_error(c);
                    return -1;
                }
                c->state = CS_SENDING;
                loop_mod_fd(c->loop, c->fd, LOOP_WRITE);
            }
        }
        break;

    case CS_TLS_HANDSHAKE:
        if (events & (LOOP_READ | LOOP_WRITE))
            conn_do_tls_handshake(c);
        break;

    case CS_SENDING:
        if (events & LOOP_WRITE)
            conn_do_send(c);
        break;

    case CS_RECEIVING_HEADERS:
    case CS_RECEIVING_BODY:
        if (events & LOOP_READ)
            conn_do_recv(c);
        break;

    case CS_DONE:
    case CS_ERROR:
        return -1;
    }

    if (c->state == CS_DONE || c->state == CS_ERROR) {
        /* Clean up the fd/TLS resources here; HttpClient.active still
         * points to the Conn struct but retry_timer_cb will free it. */
        if (c->timeout_timer >= 0) {
            loop_cancel_timer(c->loop, c->timeout_timer);
            c->timeout_timer = -1;
        }
        tls_ca_free(c->trust_anchors, c->num_trust_anchors);
        c->trust_anchors     = NULL;
        c->num_trust_anchors = 0;
        buf_destroy(&c->req_buf);
        buf_destroy(&c->resp_buf);
        /* fd was already closed in conn_finish's path; if not (error path): */
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1;
        }
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * TLS setup — loads system CAs and configures BearSSL
 * ---------------------------------------------------------------------- */

static void conn_start_tls(Conn *c)
{
    /* Load system trust anchors */
    size_t num_tas = tls_ca_load(&c->trust_anchors);
    c->num_trust_anchors = num_tas;

    if (num_tas > 0) {
        /* Real certificate validation */
        br_ssl_client_init_full(&c->ssl_ctx, &c->x509_minimal,
                                c->trust_anchors, num_tas);
    } else {
        /* No CA bundle found — fall back to no-verify (dev/test mode) */
        fprintf(stderr,
                "client: WARNING — no CA bundle; TLS certificate "
                "NOT verified\n");
        br_ssl_client_init_full(&c->ssl_ctx, NULL, NULL, 0);
        static const br_x509_class *nv = &noverify_x509_vtable;
        c->x509_noverify = nv;
        br_ssl_engine_set_x509(&c->ssl_ctx.eng,
                               (const br_x509_class **)&c->x509_noverify);
    }

    br_ssl_engine_set_buffer(&c->ssl_ctx.eng,
                             c->tls_buf, TLS_IO_BUFSIZE, 1);
    br_ssl_client_reset(&c->ssl_ctx, c->req.host, 0);

    c->state = CS_TLS_HANDSHAKE;
    loop_mod_fd(c->loop, c->fd, LOOP_READ | LOOP_WRITE);
}

static void conn_do_tls_handshake(Conn *c)
{
    br_ssl_engine_context *eng = &c->ssl_ctx.eng;

    tls_pump(c);

    unsigned st = br_ssl_engine_current_state(eng);

    if (st & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(eng);
        fprintf(stderr, "client: TLS handshake failed: error %d\n", err);
        conn_error(c);
        return;
    }

    if (st & BR_SSL_SENDAPP) {
        if (build_request(c) < 0) {
            conn_error(c);
            return;
        }
        c->state = CS_SENDING;
        loop_mod_fd(c->loop, c->fd, LOOP_WRITE);
    }
}

static void conn_do_send(Conn *c)
{
    const char *data      = c->req_buf.data + c->req_sent;
    size_t      remaining = c->req_buf.len - c->req_sent;

    while (remaining > 0) {
        ssize_t n;
        if (c->use_tls)
            n = tls_write(c, data, remaining);
        else
            n = plain_write(c, data, remaining);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            conn_error(c);
            return;
        }
        if (n == 0)
            return;

        c->req_sent += (size_t)n;
        data        += n;
        remaining   -= (size_t)n;
    }

    c->state = CS_RECEIVING_HEADERS;
    buf_init(&c->resp_buf);
    c->headers_done = 0;
    loop_mod_fd(c->loop, c->fd, LOOP_READ);
}

static void conn_do_recv(Conn *c)
{
    char tmp[8192];

    for (;;) {
        ssize_t n;
        if (c->use_tls)
            n = tls_read(c, tmp, sizeof(tmp));
        else
            n = plain_read(c, tmp, sizeof(tmp));

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            conn_finish(c, c->resp_status);
            return;
        }

        if (n == 0) {
            conn_finish(c, c->resp_status);
            return;
        }

        if (c->state == CS_RECEIVING_HEADERS) {
            if (buf_append(&c->resp_buf, tmp, (size_t)n) < 0) {
                conn_error(c);
                return;
            }
            int status, chunked, clen, retry_after;
            const char *body = find_body_start(
                c->resp_buf.data, c->resp_buf.len,
                &status, &chunked, &clen, &retry_after);

            if (body) {
                c->resp_status    = status;
                c->chunked        = chunked;
                c->content_length = clen;
                c->retry_after_s  = retry_after;
                c->state          = CS_RECEIVING_BODY;

                size_t body_offset = (size_t)(body - c->resp_buf.data);
                size_t body_bytes  = c->resp_buf.len - body_offset;
                if (body_bytes > 0)
                    conn_dispatch_body(c, body, body_bytes);
            }
        } else {
            conn_dispatch_body(c, tmp, (size_t)n);
        }
    }
}

static void conn_dispatch_body(Conn *c, const char *data, size_t len)
{
    if (c->req.on_body)
        c->req.on_body(data, len, c->req.cb_ctx);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

HttpClient *http_client_new(Loop *loop)
{
    HttpClient *cl = calloc(1, sizeof(HttpClient));
    if (!cl)
        return NULL;
    cl->loop        = loop;
    cl->retry_timer = -1;
    cl->retry_cfg   = retry_config_default();
    return cl;
}

void http_client_free(HttpClient *c)
{
    /* Cancel any pending retry timer */
    if (c->retry_timer >= 0) {
        loop_cancel_timer(c->loop, c->retry_timer);
        c->retry_timer = -1;
    }
    if (c->active) {
        conn_destroy(c->active);
        c->active = NULL;
    }
    free(c);
}

int http_client_request(HttpClient *cl, const HttpRequest *req)
{
    /* Resolve host */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", req->port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(req->host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "client: getaddrinfo failed for %s\n", req->host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (fd_set_nonblocking(fd) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    Conn *c = calloc(1, sizeof(Conn));
    if (!c) {
        close(fd);
        return -1;
    }
    c->client         = cl;
    c->loop           = cl->loop;
    c->fd             = fd;
    c->state          = CS_CONNECTING;
    c->use_tls        = req->use_tls;
    c->req            = *req; /* shallow copy — caller keeps strings alive */
    c->resp_status    = 0;
    c->headers_done   = 0;
    c->content_length = -1;
    c->chunked        = 0;
    c->retry_after_s  = -1;
    c->timeout_timer  = -1;

    /* Set up connection timeout */
    int tms = req->timeout_ms > 0 ? req->timeout_ms : DEFAULT_TIMEOUT_MS;
    c->timeout_timer = loop_add_timer(cl->loop, tms, timeout_timer_cb, c);

    if (loop_add_fd(cl->loop, fd, LOOP_WRITE, conn_io, c) < 0) {
        if (c->timeout_timer >= 0)
            loop_cancel_timer(cl->loop, c->timeout_timer);
        free(c);
        close(fd);
        return -1;
    }

    if (cr == 0) {
        if (req->use_tls) {
            conn_start_tls(c);
        } else {
            if (build_request(c) < 0) {
                conn_destroy(c);
                cl->active = NULL;
                return -1;
            }
            c->state = CS_SENDING;
        }
    }

    cl->active = c;
    return 0;
}
