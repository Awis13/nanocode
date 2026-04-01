/*
 * jsonrpc.c — JSON-RPC 2.0 over stdio with Content-Length framing
 *
 * Wire format (same as LSP and MCP over stdio):
 *   Content-Length: N\r\n
 *   \r\n
 *   <N bytes of UTF-8 JSON>
 */

#include "jsonrpc.h"
#include "../util/buf.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Spawn
 * ---------------------------------------------------------------------- */

int jsonrpc_spawn(JsonRpc *rpc, const char *prog, char *const argv[])
{
    int to_child[2];    /* parent writes → child reads (child stdin) */
    int from_child[2];  /* child writes → parent reads (child stdout) */

    rpc->pid      = (pid_t)-1;
    rpc->write_fd = -1;
    rpc->read_fd  = -1;
    rpc->next_id  = 1;

    if (pipe(to_child) < 0)
        return -1;
    if (pipe(from_child) < 0) {
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);   close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: wire up stdin/stdout, close unused ends, exec. */
        dup2(to_child[0],   STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);   close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execvp(prog, argv);
        _exit(127);
    }

    /* Parent: keep write end of to_child and read end of from_child. */
    close(to_child[0]);
    close(from_child[1]);

    rpc->pid      = pid;
    rpc->write_fd = to_child[1];
    rpc->read_fd  = from_child[0];
    rpc->next_id  = 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Request ID
 * ---------------------------------------------------------------------- */

int jsonrpc_next_id(JsonRpc *rpc)
{
    int id = rpc->next_id;
    rpc->next_id = (rpc->next_id < 0x7FFFFFFF) ? rpc->next_id + 1 : 1;
    return id;
}

/* -------------------------------------------------------------------------
 * Write helpers
 * ---------------------------------------------------------------------- */

/*
 * Write exactly `len` bytes to `fd`, retrying on EINTR.
 * Temporarily ignores SIGPIPE so a broken-pipe error manifests as EPIPE
 * (return -1) instead of killing the process.
 */
static int write_all(int fd, const char *buf, size_t len)
{
    struct sigaction sa_ign, sa_old;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ign, &sa_old);

    size_t written = 0;
    int    rc      = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;  /* EPIPE or other error */
            break;
        }
        written += (size_t)n;
    }

    sigaction(SIGPIPE, &sa_old, NULL);
    return rc;
}

/* -------------------------------------------------------------------------
 * Send
 * ---------------------------------------------------------------------- */

int jsonrpc_send(JsonRpc *rpc, const char *method,
                 const char *params_json, int id)
{
    if (rpc->write_fd < 0)
        return -1;

    const char *params = params_json ? params_json : "{}";

    /* Build JSON body into a Buf. */
    Buf body;
    buf_init(&body);

    if (id > 0) {
        /* Request — includes "id". */
        char id_str[32];
        int n = snprintf(id_str, sizeof(id_str), "%d", id);
        if (n < 0 || (size_t)n >= sizeof(id_str))
            goto oom;

        if (buf_append_str(&body, "{\"jsonrpc\":\"2.0\",\"id\":") != 0 ||
            buf_append_str(&body, id_str)                         != 0 ||
            buf_append_str(&body, ",\"method\":\"")               != 0 ||
            buf_append_str(&body, method)                         != 0 ||
            buf_append_str(&body, "\",\"params\":")               != 0 ||
            buf_append_str(&body, params)                         != 0 ||
            buf_append_str(&body, "}")                            != 0)
            goto oom;
    } else {
        /* Notification — no "id". */
        if (buf_append_str(&body, "{\"jsonrpc\":\"2.0\",\"method\":\"") != 0 ||
            buf_append_str(&body, method)                               != 0 ||
            buf_append_str(&body, "\",\"params\":")                     != 0 ||
            buf_append_str(&body, params)                               != 0 ||
            buf_append_str(&body, "}")                                  != 0)
            goto oom;
    }

    /* Build Content-Length header. */
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", body.len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header))
        goto oom;

    /* Write header + body. */
    int rc = 0;
    if (write_all(rpc->write_fd, header, (size_t)hlen) != 0 ||
        write_all(rpc->write_fd, body.data, body.len)  != 0)
        rc = -1;

    buf_destroy(&body);
    return rc;

oom:
    buf_destroy(&body);
    return -1;
}

/* -------------------------------------------------------------------------
 * Receive helpers
 * ---------------------------------------------------------------------- */

/*
 * Read exactly `len` bytes from `fd`, blocking, retrying on EINTR.
 * Returns 0 on success, -1 on EOF or error.
 */
static int read_exact(int fd, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;  /* EOF */
        got += (size_t)n;
    }
    return 0;
}

/*
 * Read one byte from `fd` with optional timeout.
 * timeout_ms > 0: use poll; 0: block.
 * Returns 1 on success, 0 on timeout, -1 on error/EOF.
 */
static int read_byte_timed(int fd, char *c, int timeout_ms)
{
    if (timeout_ms > 0) {
        struct pollfd pfd;
        pfd.fd     = fd;
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, timeout_ms);
        if (r == 0)
            return 0;  /* timeout */
        if (r < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }
    }
    ssize_t n = read(fd, c, 1);
    if (n == 1)
        return 1;
    if (n == 0)
        return -1;  /* EOF */
    if (errno == EINTR)
        return 0;
    return -1;
}

/*
 * Case-insensitive prefix match for header parsing.
 */
static int ci_prefix(const char *line, const char *prefix)
{
    while (*prefix) {
        char a = *line++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Receive
 * ---------------------------------------------------------------------- */

int jsonrpc_recv(JsonRpc *rpc, char *buf, size_t bufsz, int timeout_ms)
{
    if (rpc->read_fd < 0 || !buf || bufsz < 2)
        return -1;

    /*
     * Read headers line by line until the blank separator.
     * Apply the caller-supplied timeout only for the very first byte;
     * after that we read the rest of the message without further timeout.
     */
    size_t content_len = 0;
    int    have_cl     = 0;
    char   line[256];
    int    linelen     = 0;
    int    first_byte  = 1;

    for (;;) {
        char c;
        int  r;

        if (first_byte) {
            r = read_byte_timed(rpc->read_fd, &c, timeout_ms);
            if (r <= 0)
                return -1;  /* timeout or error */
            first_byte = 0;
        } else {
            ssize_t n = read(rpc->read_fd, &c, 1);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (n == 0)
                return -1;  /* EOF */
        }

        if (c == '\n') {
            /* Strip trailing \r. */
            if (linelen > 0 && line[linelen - 1] == '\r')
                linelen--;
            line[linelen] = '\0';

            if (linelen == 0)
                break;  /* blank line: end of headers */

            if (ci_prefix(line, "Content-Length:")) {
                const char *val = line + 15;
                while (*val == ' ') val++;
                content_len = (size_t)strtoul(val, NULL, 10);
                have_cl = 1;
            }
            linelen = 0;
        } else {
            if (linelen < (int)sizeof(line) - 1)
                line[linelen++] = c;
        }
    }

    if (!have_cl || content_len == 0)
        return -1;
    if (content_len >= bufsz)
        return -1;  /* won't fit */

    /* Read the body. */
    if (read_exact(rpc->read_fd, buf, content_len) != 0)
        return -1;
    buf[content_len] = '\0';
    return (int)content_len;
}

/* -------------------------------------------------------------------------
 * Close
 * ---------------------------------------------------------------------- */

void jsonrpc_close(JsonRpc *rpc)
{
    if (rpc->write_fd >= 0) {
        close(rpc->write_fd);
        rpc->write_fd = -1;
    }
    if (rpc->read_fd >= 0) {
        close(rpc->read_fd);
        rpc->read_fd = -1;
    }
    if (rpc->pid > (pid_t)0) {
        /* Closing the write pipe sends EOF to the child; give it a brief
         * chance to exit, then force-kill. */
        kill(rpc->pid, SIGKILL);
        waitpid(rpc->pid, NULL, 0);
        rpc->pid = (pid_t)-1;
    }
}
