/*
 * daemon.c — Unix domain socket control daemon
 *
 * Binds ~/.nanocode/nanocode.sock, accepts JSON prompt requests,
 * dispatches them, and sends back JSON responses.
 * Integrated with loop.h; never blocks.
 */

#include "daemon.h"
#include "../util/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* Maximum concurrent client connections. */
#define MAX_DAEMON_CONNS  16

/* Maximum request payload (64 KB). */
#define MAX_REQUEST_SIZE  (64 * 1024)

/* -------------------------------------------------------------------------
 * JSON helpers (inline, no dependency on util/json.c)
 * ---------------------------------------------------------------------- */

/*
 * Find a top-level JSON string value for `key` in the NUL-terminated
 * buffer `json` of length `len`. Writes unescaped value into out (NUL-term).
 * Returns 0 on success, -1 if not found.
 */
static int simple_json_get_str(const char *json, size_t len,
                                const char *key,
                                char *out, size_t out_cap)
{
    /* Build search pattern: "key":" */
    char pat[128];
    int plen = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pat))
        return -1;

    /* Scan for pattern in json. */
    const char *p = json;
    const char *end = json + len;
    while (p < end) {
        p = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!p)
            return -1;
        if ((size_t)(end - p) < (size_t)plen)
            return -1;
        if (memcmp(p, pat, (size_t)plen) == 0) {
            p += plen;
            /* skip whitespace */
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p >= end || *p != '"')
                return -1;
            p++; /* skip opening quote */
            size_t out_pos = 0;
            while (p < end && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (p >= end) break;
                    char esc = *p;
                    if (esc == 'n')  esc = '\n';
                    else if (esc == 'r') esc = '\r';
                    else if (esc == 't') esc = '\t';
                    if (out_pos + 1 < out_cap)
                        out[out_pos++] = esc;
                } else {
                    if (out_pos + 1 < out_cap)
                        out[out_pos++] = *p;
                }
                p++;
            }
            if (out_pos < out_cap) out[out_pos] = '\0';
            else out[out_cap - 1] = '\0';
            return 0;
        }
        p++; /* skip past this '"' */
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef struct {
    struct Daemon *d;
    int            fd;
    char           buf[MAX_REQUEST_SIZE];
    size_t         len;
    int            active;
} ConnEntry;

/* -------------------------------------------------------------------------
 * Daemon struct
 * ---------------------------------------------------------------------- */

struct Daemon {
    Loop              *loop;
    int                listen_fd;
    char               sock_path[256];
    daemon_dispatch_fn dispatch;
    void              *ctx;
    ConnEntry          conns[MAX_DAEMON_CONNS];
};

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static int daemon_accept_cb(int fd, int events, void *ctx);
static int daemon_client_cb(int fd, int events, void *ctx);
static void conn_close(ConnEntry *c);
static void conn_handle(ConnEntry *c);

/* -------------------------------------------------------------------------
 * Accept callback — registered for the listen socket
 * ---------------------------------------------------------------------- */

static int daemon_accept_cb(int fd, int events, void *ctx)
{
    struct Daemon *d = (struct Daemon *)ctx;
    (void)events;

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            perror("daemon: accept");
            break;
        }

        /* Find a free slot. */
        ConnEntry *slot = NULL;
        for (int i = 0; i < MAX_DAEMON_CONNS; i++) {
            if (!d->conns[i].active) { slot = &d->conns[i]; break; }
        }
        if (!slot) {
            /* Too many connections — reject. */
            close(cfd);
            continue;
        }

        fd_set_nonblocking(cfd);
        slot->d      = d;
        slot->fd     = cfd;
        slot->len    = 0;
        slot->active = 1;

        if (loop_add_fd(d->loop, cfd, LOOP_READ, daemon_client_cb, slot) < 0) {
            close(cfd);
            slot->active = 0;
        }
    }
    return 0; /* keep listen fd in loop */
}

/* -------------------------------------------------------------------------
 * Client read callback
 * ---------------------------------------------------------------------- */

static int daemon_client_cb(int fd, int events, void *ctx)
{
    ConnEntry *c = (ConnEntry *)ctx;
    (void)events;

    for (;;) {
        size_t space = MAX_REQUEST_SIZE - c->len;
        if (space == 0) {
            /* Request too large — close. */
            conn_close(c);
            return -1;
        }

        ssize_t n = read(fd, c->buf + c->len, space);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            conn_close(c);
            return -1;
        }
        if (n == 0) {
            /* EOF — process whatever we have. */
            conn_handle(c);
            return -1;
        }
        c->len += (size_t)n;

        /* Check for newline delimiter. */
        if (memchr(c->buf, '\n', c->len)) {
            conn_handle(c);
            return -1;
        }
    }
    return 0; /* keep fd in loop */
}

/* -------------------------------------------------------------------------
 * Handle a complete request
 * ---------------------------------------------------------------------- */

static void conn_handle(ConnEntry *c)
{
    char prompt[4096] = {0};
    char cwd[4096]    = {0};

    int got_prompt = simple_json_get_str(c->buf, c->len, "prompt",
                                         prompt, sizeof(prompt));
    simple_json_get_str(c->buf, c->len, "cwd", cwd, sizeof(cwd));

    const char *result  = NULL;
    const char *status  = "done";

    if (got_prompt < 0) {
        status = "error";
        result = "missing prompt field";
    } else {
        result = c->d->dispatch(prompt, cwd[0] ? cwd : NULL, c->d->ctx);
        if (!result) {
            status = "error";
            result = "internal error";
        }
    }

    /* Build response: {"status":"...","result":"..."}\n
     * 64 KB buffer reduces silent truncation risk for large dispatch results. */
    char resp[65536];
    size_t cap = sizeof(resp);
    size_t pos = 0;

    if (pos < cap) resp[pos++] = '{';
    const char *sk = "\"status\":";
    size_t sklen = strlen(sk);
    if (pos + sklen < cap) { memcpy(resp + pos, sk, sklen); pos += sklen; }
    pos = json_escape_str(resp, cap, pos, status);
    if (pos < cap) resp[pos++] = ',';
    const char *rk = "\"result\":";
    size_t rklen = strlen(rk);
    if (pos + rklen < cap) { memcpy(resp + pos, rk, rklen); pos += rklen; }
    pos = json_escape_str(resp, cap, pos, result);
    if (pos < cap) resp[pos++] = '}';
    if (pos < cap) resp[pos++] = '\n';
    if (pos < cap) resp[pos]   = '\0'; else resp[cap-1] = '\0';

    /* Guard: if pos exceeded the buffer the JSON is truncated and invalid.
     * Replace with a safe error envelope instead of sending corrupt JSON. */
    if (pos >= cap) {
        static const char trunc_resp[] =
            "{\"status\":\"error\",\"error\":\"response truncated\"}\n";
        pos = sizeof(trunc_resp) - 1;
        memcpy(resp, trunc_resp, pos);
    }

    /* Clamp send length to actual bytes written (pos is guaranteed < cap now). */
    size_t send_len = pos;
    size_t written = 0;
    while (written < send_len) {
        ssize_t w = write(c->fd, resp + written, send_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }

    conn_close(c);
}

/* -------------------------------------------------------------------------
 * Close a connection entry
 * ---------------------------------------------------------------------- */

static void conn_close(ConnEntry *c)
{
    loop_remove_fd(c->d->loop, c->fd);
    close(c->fd);
    c->fd     = -1;
    c->len    = 0;
    c->active = 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Daemon *daemon_start(Loop *loop, const char *sock_path,
                     daemon_dispatch_fn dispatch, void *ctx)
{
    if (!loop || !sock_path || !dispatch)
        return NULL;

    /* Delete stale socket file. */
    unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("daemon: socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fd_set_nonblocking(lfd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "daemon: socket path too long\n");
        close(lfd);
        return NULL;
    }
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("daemon: bind");
        close(lfd);
        return NULL;
    }

    /* Restrict socket to owner only — prevents other users from connecting. */
    if (chmod(sock_path, 0600) < 0) {
        perror("daemon: chmod");
        close(lfd);
        unlink(sock_path);
        return NULL;
    }

    if (listen(lfd, 8) < 0) {
        perror("daemon: listen");
        close(lfd);
        unlink(sock_path);
        return NULL;
    }

    struct Daemon *d = calloc(1, sizeof(*d));
    if (!d) {
        close(lfd);
        unlink(sock_path);
        return NULL;
    }

    d->loop      = loop;
    d->listen_fd = lfd;
    d->dispatch  = dispatch;
    d->ctx       = ctx;
    strncpy(d->sock_path, sock_path, sizeof(d->sock_path) - 1);

    for (int i = 0; i < MAX_DAEMON_CONNS; i++) {
        d->conns[i].fd     = -1;
        d->conns[i].active = 0;
        d->conns[i].d      = d;
    }

    if (loop_add_fd(loop, lfd, LOOP_READ, daemon_accept_cb, d) < 0) {
        fprintf(stderr, "daemon: loop_add_fd failed\n");
        free(d);
        close(lfd);
        unlink(sock_path);
        return NULL;
    }

    return d;
}

void daemon_stop(Daemon *d)
{
    if (!d)
        return;

    /* Close all open client connections. */
    for (int i = 0; i < MAX_DAEMON_CONNS; i++) {
        if (d->conns[i].active) {
            loop_remove_fd(d->loop, d->conns[i].fd);
            close(d->conns[i].fd);
            d->conns[i].active = 0;
        }
    }

    /* Remove listen socket from loop and close. */
    loop_remove_fd(d->loop, d->listen_fd);
    close(d->listen_fd);

    /* Remove socket file. */
    unlink(d->sock_path);

    free(d);
}
