/*
 * audit.h — structured audit log (CMP-210)
 *
 * Writes JSONL lines for every tool call and sandbox denial.
 *
 * Typical call sequence:
 *   1. config_load()
 *   2. audit_open(path) — if audit.enabled
 *   3. executor_set_audit(log, session_id, sandbox_profile, cwd)
 *   4. sandbox_set_audit(log, session_id)
 *   5. ... normal execution ...
 *   6. audit_close(log)
 *
 * JSONL format (one JSON object per line):
 *   tool_call:
 *     {"ts":"...","level":"info","event":"tool_call","tool":"bash",
 *      "args":{...},"result_size":100,"result_ok":1,
 *      "duration_ms":50,"sandbox":"strict","cwd":"/home","session_id":"abc"}
 *   sandbox_deny:
 *     {"ts":"...","level":"warn","event":"sandbox_deny",
 *      "denied":"/etc/passwd","sandbox":"strict","session_id":"abc"}
 *
 * Rules:
 *   - ISO 8601 timestamps in UTC
 *   - level: info (tool_call), warn (sandbox_deny)
 *   - null/empty optional fields are omitted
 *   - args_json is truncated to 2048 chars if longer
 *   - All public functions are NULL-safe (NULL log is a no-op)
 */

#ifndef AUDIT_H
#define AUDIT_H

/*
 * Opaque audit log handle.
 */
typedef struct AuditLog AuditLog;

/*
 * audit_open — open an audit log file for appending.
 *
 * path      : filesystem path to the log file; created if absent.
 * max_size  : rotate when file exceeds this many bytes (0 = no rotation).
 * max_files : keep this many rotated files (audit.log.1 … .N); 0 = no limit.
 *
 * Returns a handle on success, NULL on error (prints reason to stderr).
 */
AuditLog *audit_open(const char *path, long max_size, int max_files);

/*
 * audit_open_stdout — open an audit log that writes to stdout.
 * Rotation is not performed on stdout handles.
 */
AuditLog *audit_open_stdout(void);

/*
 * audit_close — flush and close the log.
 * NULL is a no-op.
 */
void audit_close(AuditLog *log);

/*
 * audit_tool_call — record a tool invocation.
 *
 * log             : audit log handle (NULL = no-op)
 * tool            : tool name (e.g. "bash")
 * args_json       : raw JSON arguments string; truncated to 2048 chars
 * result_json     : raw JSON result string; only its length is logged
 * result_ok       : 1 if the tool succeeded, 0 on error
 * duration_ms     : wall-clock execution time in milliseconds
 * session_id      : opaque session identifier (may be NULL)
 * sandbox_profile : active sandbox profile name (may be NULL)
 * cwd             : working directory at time of call (may be NULL)
 */
void audit_tool_call(AuditLog *log,
                     const char *tool,
                     const char *args_json,
                     const char *result_json,
                     int         result_ok,
                     long        duration_ms,
                     const char *session_id,
                     const char *sandbox_profile,
                     const char *cwd);

/*
 * audit_sandbox_deny — record a sandbox policy denial.
 *
 * log             : audit log handle (NULL = no-op)
 * denied          : resource that was denied (path, command, etc.)
 * session_id      : opaque session identifier (may be NULL)
 * sandbox_profile : active sandbox profile name (may be NULL)
 */
void audit_sandbox_deny(AuditLog   *log,
                        const char *denied,
                        const char *session_id,
                        const char *sandbox_profile);

#endif /* AUDIT_H */
