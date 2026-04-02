/*
 * daemon.h — Unix domain socket control daemon
 *
 * Binds a SOCK_STREAM AF_UNIX socket and integrates with the event loop
 * to accept remote prompt requests without blocking.
 *
 * Protocol: newline-delimited JSON over the socket.
 *   Request:  {"prompt":"...","cwd":"..."}   (cwd optional)
 *   Response: {"status":"done|error","result":"..."}\n
 *   One request per connection; socket is closed after the reply.
 *   Max request size: 64 KB.
 */

#ifndef DAEMON_H
#define DAEMON_H

#include "loop.h"

typedef struct Daemon Daemon;

/*
 * Callback invoked when a complete request arrives.
 * `prompt`  — NUL-terminated prompt string from the JSON request.
 * `cwd`     — NUL-terminated working directory (may be NULL if omitted).
 * `ctx`     — user context passed to daemon_start().
 * Returns a NUL-terminated result string (caller does NOT free it;
 * the daemon copies it into the response before the callback returns).
 * On error the callback should return NULL; the daemon will reply with
 * {"status":"error","result":"internal error"}.
 */
typedef const char *(*daemon_dispatch_fn)(const char *prompt,
                                          const char *cwd,
                                          void       *ctx);

/*
 * Start the daemon: binds sock_path (AF_UNIX SOCK_STREAM),
 * registers the listen socket with loop, and returns the Daemon handle.
 * Returns NULL on failure (socket bind error, OOM, etc.).
 * Deletes any stale socket file at sock_path before binding.
 */
Daemon *daemon_start(Loop *loop, const char *sock_path,
                     daemon_dispatch_fn dispatch, void *ctx);

/*
 * Stop the daemon: deregisters from the loop, closes open connections,
 * removes the socket file, and frees all resources.
 */
void daemon_stop(Daemon *d);

#endif /* DAEMON_H */
