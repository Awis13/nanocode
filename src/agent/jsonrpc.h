/*
 * jsonrpc.h — JSON-RPC 2.0 over stdio with Content-Length framing
 *
 * Spawns a subprocess and communicates via its stdin/stdout using the
 * LSP/MCP wire format:
 *   Content-Length: N\r\n\r\n<json-body>
 *
 * Used by both the LSP client (lsp.c) and the MCP client (CMP-150).
 */

#ifndef JSONRPC_H
#define JSONRPC_H

#include <sys/types.h>  /* pid_t */
#include <stddef.h>

/* Maximum size of a single JSON-RPC message body (1 MiB). */
#define JSONRPC_MSG_MAX (1 << 20)

typedef struct {
    pid_t  pid;       /* child process PID, or -1 if not running */
    int    write_fd;  /* parent→child (child's stdin) */
    int    read_fd;   /* child→parent (child's stdout) */
    int    next_id;   /* next request id (auto-incremented) */
} JsonRpc;

/*
 * Spawn `prog` with `argv` (NULL-terminated), connect to its stdin/stdout.
 * Initialises all fields of `rpc`.
 * Returns 0 on success, -1 on error.
 */
int jsonrpc_spawn(JsonRpc *rpc, const char *prog, char *const argv[]);

/*
 * Send a JSON-RPC message.
 *   method       : method name (e.g. "initialize")
 *   params_json  : JSON params string, or NULL for "{}"
 *   id           : request id (> 0) for requests; 0 for notifications
 * Returns 0 on success, -1 on write error (e.g. broken pipe).
 */
int jsonrpc_send(JsonRpc *rpc, const char *method,
                 const char *params_json, int id);

/*
 * Read next complete JSON-RPC message into `buf` (NUL-terminated body).
 *   timeout_ms : milliseconds to wait for first byte;
 *                0 = block indefinitely.
 * Returns body length on success, -1 on timeout/error/EOF or if message
 * exceeds bufsz-1.
 */
int jsonrpc_recv(JsonRpc *rpc, char *buf, size_t bufsz, int timeout_ms);

/*
 * Allocate and return the next request id (1-based, wraps at INT_MAX).
 */
int jsonrpc_next_id(JsonRpc *rpc);

/*
 * Close pipes and reap the child process.
 * Sends SIGKILL if not already dead.
 * Safe to call on an already-closed JsonRpc (no-op).
 */
void jsonrpc_close(JsonRpc *rpc);

#endif /* JSONRPC_H */
