/*
 * audit.c — structured JSONL audit log (CMP-210)
 *
 * Writes one JSON object per line for tool calls and sandbox denials.
 * Log rotation: rename audit.log -> audit.log.1 -> ... -> audit.log.N.
 * File size is checked via fseek/ftell before each write.
 *
 * All stack buffers are 4 KB per line.  No arena allocation needed.
 * The public API is NULL-safe: passing NULL for the log handle is a no-op.
 */

#include "../../include/audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Maximum length for the args_json excerpt embedded in the JSONL line. */
#define AUDIT_ARGS_MAX   2048
/* Stack buffer size for one JSONL line. */
#define AUDIT_LINE_MAX   4096
/* Maximum path length stored in the struct. */
#define AUDIT_PATH_MAX   512

/* -------------------------------------------------------------------------
 * AuditLog struct
 * ---------------------------------------------------------------------- */

struct AuditLog {
    FILE *fp;
    int   is_stdout;                 /* 1 = stdout; no rotation, no close */
    char  path[AUDIT_PATH_MAX];
    long  max_size;                  /* 0 = no rotation */
    int   max_files;                 /* 0 = keep forever */
};

/* -------------------------------------------------------------------------
 * Internal: JSON string escaping
 *
 * Writes `src` into `dst` (capacity `cap`, must be >= 1) with JSON escaping.
 * Returns bytes written, not including the NUL terminator.
 * Stops when fewer than 7 bytes remain in `dst` (worst-case \\u00XX).
 * ---------------------------------------------------------------------- */

static size_t json_escape(char *dst, size_t cap, const char *src)
{
    size_t w = 0;
    if (!src || cap < 1)
        return 0;
    for (const unsigned char *p = (const unsigned char *)src;
         *p && w + 7 < cap; p++) {
        if      (*p == '"')  { dst[w++] = '\\'; dst[w++] = '"';  }
        else if (*p == '\\') { dst[w++] = '\\'; dst[w++] = '\\'; }
        else if (*p == '\n') { dst[w++] = '\\'; dst[w++] = 'n';  }
        else if (*p == '\r') { dst[w++] = '\\'; dst[w++] = 'r';  }
        else if (*p == '\t') { dst[w++] = '\\'; dst[w++] = 't';  }
        else if (*p < 0x20)  {
            w += (size_t)snprintf(dst + w, 7, "\\u%04x", (unsigned)*p);
        } else {
            dst[w++] = (char)*p;
        }
    }
    dst[w] = '\0';
    return w;
}

/* -------------------------------------------------------------------------
 * Internal: ISO 8601 UTC timestamp
 *
 * Writes "YYYY-MM-DDTHH:MM:SSZ" into buf[bufsz].
 * ---------------------------------------------------------------------- */

static void utc_timestamp(char *buf, size_t bufsz)
{
    time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    strftime(buf, bufsz, "%Y-%m-%dT%H:%M:%SZ", &t);
}

/* -------------------------------------------------------------------------
 * Internal: log rotation
 *
 * Rotates path: .log.{N-1} is deleted, each .log.{k} renamed to .log.{k+1},
 * then .log renamed to .log.1.  The primary file is then reopened fresh.
 * Errors are silently ignored — logging is best-effort.
 * ---------------------------------------------------------------------- */

static void do_rotate(struct AuditLog *log)
{
    if (!log || log->is_stdout || !log->path[0])
        return;

    /* Close current file. */
    if (log->fp) {
        fclose(log->fp);
        log->fp = NULL;
    }

    /* Delete the oldest file if max_files is set. */
    if (log->max_files > 0) {
        char old[AUDIT_PATH_MAX + 16];
        snprintf(old, sizeof(old), "%s.%d", log->path, log->max_files);
        remove(old);
    }

    /* Shift existing rotated files: .log.{k} -> .log.{k+1} */
    int limit = log->max_files > 0 ? log->max_files - 1 : 99;
    for (int k = limit; k >= 1; k--) {
        char src[AUDIT_PATH_MAX + 16];
        char dst[AUDIT_PATH_MAX + 16];
        snprintf(src, sizeof(src), "%s.%d", log->path, k);
        snprintf(dst, sizeof(dst), "%s.%d", log->path, k + 1);
        rename(src, dst);
    }

    /* Rename primary to .log.1 */
    {
        char bak[AUDIT_PATH_MAX + 16];
        snprintf(bak, sizeof(bak), "%s.1", log->path);
        rename(log->path, bak);
    }

    /* Reopen primary file (fresh). */
    log->fp = fopen(log->path, "a");
}

/* -------------------------------------------------------------------------
 * Internal: write a line (with rotation check)
 * ---------------------------------------------------------------------- */

static void audit_write_line(struct AuditLog *log, const char *line, size_t len)
{
    if (!log || !log->fp || !line || len == 0)
        return;

    /* Check file size before write (spec requirement). */
    if (!log->is_stdout && log->max_size > 0) {
        if (fseek(log->fp, 0, SEEK_END) == 0) {
            long sz = ftell(log->fp);
            if (sz >= log->max_size)
                do_rotate(log);
        }
    }

    if (!log->fp)
        return;

    fwrite(line, 1, len, log->fp);
    fflush(log->fp);
}

/* =========================================================================
 * Public: audit_open
 * ====================================================================== */

AuditLog *audit_open(const char *path, long max_size, int max_files)
{
    if (!path || !path[0]) {
        fprintf(stderr, "audit: audit_open called with NULL/empty path\n");
        return NULL;
    }

    struct AuditLog *log = calloc(1, sizeof(struct AuditLog));
    if (!log) {
        fprintf(stderr, "audit: out of memory\n");
        return NULL;
    }

    strncpy(log->path, path, AUDIT_PATH_MAX - 1);
    log->path[AUDIT_PATH_MAX - 1] = '\0';
    log->max_size  = max_size;
    log->max_files = max_files;
    log->is_stdout = 0;

    log->fp = fopen(path, "a");
    if (!log->fp) {
        fprintf(stderr, "audit: cannot open \"%s\" for appending\n", path);
        free(log);
        return NULL;
    }

    return log;
}

/* =========================================================================
 * Public: audit_open_stdout
 * ====================================================================== */

AuditLog *audit_open_stdout(void)
{
    struct AuditLog *log = calloc(1, sizeof(struct AuditLog));
    if (!log)
        return NULL;

    log->fp        = stdout;
    log->is_stdout = 1;
    log->max_size  = 0;
    log->max_files = 0;
    return log;
}

/* =========================================================================
 * Public: audit_close
 * ====================================================================== */

void audit_close(AuditLog *log)
{
    if (!log)
        return;
    if (log->fp && !log->is_stdout)
        fclose(log->fp);
    free(log);
}

/* =========================================================================
 * Public: audit_tool_call
 * ====================================================================== */

void audit_tool_call(AuditLog   *log,
                     const char *tool,
                     const char *args_json,
                     const char *result_json,
                     int         result_ok,
                     long        duration_ms,
                     const char *session_id,
                     const char *sandbox_profile,
                     const char *cwd)
{
    if (!log)
        return;

    char   line[AUDIT_LINE_MAX];
    int    pos = 0;
    int    cap = (int)sizeof(line);
    char   ts[32];
    char   tool_esc[256];
    char   args_buf[AUDIT_ARGS_MAX + 4]; /* +4 for "..." + NUL */
    size_t result_size = result_json ? strlen(result_json) : 0;

    utc_timestamp(ts, sizeof(ts));

    /* Escape tool name. */
    json_escape(tool_esc, sizeof(tool_esc), tool ? tool : "");

    /* Prepare args: embed raw JSON, truncated to AUDIT_ARGS_MAX. */
    const char *aj = (args_json && args_json[0]) ? args_json : "{}";
    size_t      ajlen = strlen(aj);
    if (ajlen > AUDIT_ARGS_MAX) {
        memcpy(args_buf, aj, AUDIT_ARGS_MAX);
        memcpy(args_buf + AUDIT_ARGS_MAX, "...", 3);
        args_buf[AUDIT_ARGS_MAX + 3] = '\0';
    } else {
        memcpy(args_buf, aj, ajlen + 1);
    }

    /* Build mandatory fields. */
    pos += snprintf(line + pos, (size_t)(cap - pos),
                    "{\"ts\":\"%s\",\"level\":\"info\",\"event\":\"tool_call\","
                    "\"tool\":\"%s\",\"args\":%s,"
                    "\"result_size\":%zu,\"result_ok\":%d,"
                    "\"duration_ms\":%ld",
                    ts, tool_esc, args_buf,
                    result_size, result_ok ? 1 : 0, duration_ms);

    /* Optional fields — omit if NULL or empty. */
    if (sandbox_profile && sandbox_profile[0]) {
        char esc[256];
        json_escape(esc, sizeof(esc), sandbox_profile);
        pos += snprintf(line + pos, (size_t)(cap - pos),
                        ",\"sandbox\":\"%s\"", esc);
    }
    if (cwd && cwd[0]) {
        char esc[512];
        json_escape(esc, sizeof(esc), cwd);
        pos += snprintf(line + pos, (size_t)(cap - pos),
                        ",\"cwd\":\"%s\"", esc);
    }
    if (session_id && session_id[0]) {
        char esc[256];
        json_escape(esc, sizeof(esc), session_id);
        pos += snprintf(line + pos, (size_t)(cap - pos),
                        ",\"session_id\":\"%s\"", esc);
    }

    /* Close object and newline. */
    if (pos < cap - 2) {
        line[pos++] = '}';
        line[pos++] = '\n';
        line[pos]   = '\0';
    }

    audit_write_line(log, line, (size_t)pos);
}

/* =========================================================================
 * Public: audit_sandbox_deny
 * ====================================================================== */

void audit_sandbox_deny(AuditLog   *log,
                        const char *denied,
                        const char *session_id,
                        const char *sandbox_profile)
{
    if (!log)
        return;

    char line[AUDIT_LINE_MAX];
    int  pos = 0;
    int  cap = (int)sizeof(line);
    char ts[32];
    char denied_esc[512];

    utc_timestamp(ts, sizeof(ts));
    json_escape(denied_esc, sizeof(denied_esc), denied ? denied : "");

    /* Mandatory fields. */
    pos += snprintf(line + pos, (size_t)(cap - pos),
                    "{\"ts\":\"%s\",\"level\":\"warn\","
                    "\"event\":\"sandbox_deny\",\"denied\":\"%s\"",
                    ts, denied_esc);

    /* Optional fields. */
    if (sandbox_profile && sandbox_profile[0]) {
        char esc[256];
        json_escape(esc, sizeof(esc), sandbox_profile);
        pos += snprintf(line + pos, (size_t)(cap - pos),
                        ",\"sandbox\":\"%s\"", esc);
    }
    if (session_id && session_id[0]) {
        char esc[256];
        json_escape(esc, sizeof(esc), session_id);
        pos += snprintf(line + pos, (size_t)(cap - pos),
                        ",\"session_id\":\"%s\"", esc);
    }

    /* Close object and newline. */
    if (pos < cap - 2) {
        line[pos++] = '}';
        line[pos++] = '\n';
        line[pos]   = '\0';
    }

    audit_write_line(log, line, (size_t)pos);
}
