/*
 * session.h — bounded session event log (CMP-183)
 *
 * Writes NDJSON entries to a rotating log file. When the file exceeds
 * max_bytes, it is renamed to "<path>.1" and a fresh file is started.
 *
 * Thread-safety: none — single-threaded use only, consistent with the
 * rest of nanocode's event-loop architecture.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>
#include <sys/types.h>

/* Default rotation threshold: 8 MB */
#define SESSION_LOG_DEFAULT_MAX_BYTES ((size_t)(8 * 1024 * 1024))

typedef struct SessionLog SessionLog;

/*
 * Open (or create) a session log at `path`.
 * Rotates when the file exceeds `max_bytes` (pass 0 for default).
 * Returns NULL if the file cannot be opened.
 */
SessionLog *session_log_open(const char *path, size_t max_bytes);

/* Flush and close the log. Safe to call with NULL. */
void session_log_close(SessionLog *sl);

/* Write a "start" entry recording the process pid. */
void session_log_start(SessionLog *sl, int pid);

/* Write a "child_spawn" entry when a child process is forked. */
void session_log_child_spawn(SessionLog *sl, pid_t child, const char *cmd);

/* Write a "child_reap" entry when a child process is reaped. */
void session_log_child_reap(SessionLog *sl, pid_t child, int exit_code);

/*
 * Write a generic structured event entry.
 * `type` is a short ASCII label; `detail` is an optional human-readable
 * string (JSON-escaped automatically, truncated at 255 characters).
 */
void session_log_event(SessionLog *sl, const char *type, const char *detail);

#endif /* SESSION_H */
