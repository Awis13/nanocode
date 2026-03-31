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
 */

#include "client.h"

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

/* -------------------------------------------------------------------------
 * BearSSL: no-verify X.509 validator
 * Used when the caller does not provide trust anchors (dev/test mode).
 * In production, replace with br_x509_minimal_context + trust anchors.
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

typedef struct {
    Loop        *loop;
    int          fd;
    ConnState    state;

    /* Request being processed */
    HttpRequest  req;
    char        *req_host;    /* owned copy */
    char        *req_path;    /* owned copy */

    /* Request serialisation */
    Buf          req_buf;
    size_t       req_sent;

    /* Response parsing */
    Buf          resp_buf;    /* raw bytes from socket */
    int          resp_status; /* parsed HTTP status code */
    int          headers_done;
    int          chunked;     /* Transfer-Encoding: chunked */
    int          content_length;

    /* TLS */
    int          use_tls;
    br_ssl_client_context  ssl_ctx;
    br_x509_class         *x509;
    const br_x509_class   *x509_noverify;
    unsigned char          tls_buf[TLS_IO_BUFSIZE];

} Conn;

struct HttpClient {
    Loop *loop;
    /* For now: one in-flight request at a time. */
    Conn *active;
};

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void conn_finish(Conn *c, int status)
{
    c->state = CS_DONE;
    if (c->req.on_done)
        c->req.on_done(status, c->req.cb_ctx);
    loop_remove_fd(c->loop, c->fd);
    close(c->fd);
    c->fd = -1;
}

static void conn_error(Conn *c)
{
    conn_finish(c, 0);
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

    /* Host header — include port for non-standard ports (HTTP/1.1 §14.23) */
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

    /* Connection: close — simplest; no keep-alive state machine needed */
    if (buf_append_str(&c->req_buf, "Connection: close\r\n") < 0)
        return -1;

    /* Caller-supplied headers */
    for (int i = 0; i < r->nheaders; i++) {
        if (buf_append_str(&c->req_buf, r->headers[i].name) < 0  ||
            buf_append_str(&c->req_buf, ": ") < 0                ||
            buf_append_str(&c->req_buf, r->headers[i].value) < 0 ||
            buf_append_str(&c->req_buf, "\r\n") < 0)
            return -1;
    }

    /* Content-Length if there's a body */
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

    /* Body */
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
    /* "HTTP/1.1 200 OK" — we want the 3-digit code */
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
 * Scan `buf` for "\r\n\r\n" (end of headers).
 * Returns pointer to start of body, or NULL if headers not yet complete.
 */
static const char *find_body_start(const char *buf, size_t len,
                                   int *status_out,
                                   int *chunked_out,
                                   int *content_length_out)
{
    /* Find header block end */
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

    /* Scan headers */
    *chunked_out = 0;
    *content_length_out = -1;

    const char *p = lf + 1;
    while (p < end - 1) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            break;
        size_t hlen = (size_t)(eol - p);
        if (hlen > 0 && p[hlen - 1] == '\r')
            hlen--;

        if (hlen > 19 &&
            strncasecmp(p, "transfer-encoding:", 18) == 0) {
            const char *v = p + 18;
            while (*v == ' ') v++;
            if (strncasecmp(v, "chunked", 7) == 0)
                *chunked_out = 1;
        } else if (hlen > 16 &&
                   strncasecmp(p, "content-length:", 15) == 0) {
            *content_length_out = atoi(p + 15);
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

/*
 * Pump the BearSSL engine:
 *  - drain engine → socket (sendrecord)
 *  - fill socket → engine (recvrecord)
 * Returns:
 *   > 0  something was transferred
 *   0    would-block
 *  -1    error
 */
static int tls_pump(Conn *c)
{
    br_ssl_engine_context *eng = &c->ssl_ctx.eng;
    int did_something = 0;

    /* Drain: engine wants us to send raw bytes to the network. */
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

    /* Fill: engine wants raw bytes from the network. */
    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
        if (!buf || len == 0)
            break;
        ssize_t n = read(c->fd, buf, len);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n == 0)
                return -1; /* connection closed */
            return -1;
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
            return 0; /* would block */
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
        return -1; /* would block */
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
        /* Check if async connect completed */
        {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
                fprintf(stderr, "client: connect failed: %s\n",
                        err ? strerror(err) : strerror(errno));
                conn_error(c);
                return -1;
            }
            if (c->use_tls) {
                conn_start_tls(c);
            } else {
                /* Build and start sending the request */
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

    return (c->state == CS_DONE || c->state == CS_ERROR) ? -1 : 0;
}

static void conn_start_tls(Conn *c)
{
    br_ssl_client_init_full(&c->ssl_ctx, NULL, NULL, 0);

    /* Install no-verify X.509 validator */
    static const br_x509_class *nv = &noverify_x509_vtable;
    c->x509_noverify = nv;
    br_ssl_engine_set_x509(&c->ssl_ctx.eng,
                           (const br_x509_class **)&c->x509_noverify);

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

    /* Handshake complete when engine is ready to send application data */
    if (st & BR_SSL_SENDAPP) {
        if (build_request(c) < 0) {
            conn_error(c);
            return;
        }
        c->state = CS_SENDING;
        loop_mod_fd(c->loop, c->fd, LOOP_WRITE);
    }
    /* Otherwise keep pumping */
}

static void conn_do_send(Conn *c)
{
    const char *data = c->req_buf.data + c->req_sent;
    size_t remaining = c->req_buf.len - c->req_sent;

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
            return; /* would block (TLS) */

        c->req_sent += (size_t)n;
        data += n;
        remaining -= (size_t)n;
    }

    /* All sent — switch to receiving */
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
            /* EOF while receiving can be a valid end of HTTP/1.0 response */
            conn_finish(c, c->resp_status);
            return;
        }

        if (n == 0) {
            /* Connection closed gracefully */
            conn_finish(c, c->resp_status);
            return;
        }

        if (c->state == CS_RECEIVING_HEADERS) {
            if (buf_append(&c->resp_buf, tmp, (size_t)n) < 0) {
                conn_error(c);
                return;
            }
            /* Try to find end of headers */
            int status, chunked, clen;
            const char *body = find_body_start(
                c->resp_buf.data, c->resp_buf.len,
                &status, &chunked, &clen);

            if (body) {
                c->resp_status   = status;
                c->chunked       = chunked;
                c->content_length = clen;
                c->state         = CS_RECEIVING_BODY;

                /* Any bytes after headers are already body */
                size_t body_offset = (size_t)(body - c->resp_buf.data);
                size_t body_bytes  = c->resp_buf.len - body_offset;
                if (body_bytes > 0)
                    conn_dispatch_body(c, body, body_bytes);
            }
        } else {
            /* Already in body */
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
    cl->loop = loop;
    return cl;
}

void http_client_free(HttpClient *c)
{
    if (c->active) {
        if (c->active->fd >= 0) {
            loop_remove_fd(c->loop, c->active->fd);
            close(c->active->fd);
        }
        buf_destroy(&c->active->req_buf);
        buf_destroy(&c->active->resp_buf);
        free(c->active);
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

    /* Create non-blocking socket */
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

    /* Begin async connect */
    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    /* Set up connection state */
    Conn *c = calloc(1, sizeof(Conn));
    if (!c) {
        close(fd);
        return -1;
    }
    c->loop    = cl->loop;
    c->fd      = fd;
    c->state   = CS_CONNECTING;
    c->use_tls = req->use_tls;
    c->req     = *req; /* shallow copy — caller must keep strings alive */
    c->resp_status   = 0;
    c->headers_done  = 0;
    c->content_length = -1;
    c->chunked = 0;

    /* Register with event loop — WRITE fires when connect completes */
    if (loop_add_fd(cl->loop, fd, LOOP_WRITE, conn_io, c) < 0) {
        free(c);
        close(fd);
        return -1;
    }

    /* If already connected (unusual for non-blocking but possible on loopback) */
    if (cr == 0) {
        if (req->use_tls) {
            conn_start_tls(c);
        } else {
            if (build_request(c) < 0) {
                free(c);
                return -1;
            }
            c->state = CS_SENDING;
        }
    }

    cl->active = c;
    return 0;
}
