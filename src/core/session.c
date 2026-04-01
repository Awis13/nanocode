/*
 * session.c — bounded session event log (CMP-183)
 *
 * SIGCHLD audit findings (CMP-183):
 *   bash.c does NOT install a SIGCHLD handler. It uses blocking waitpid()
 *   which reliably reaps each child it spawns. This is correct for the
 *   current sequential execution model (one tool at a time per turn).
 *   No SIGCHLD handler is required — adding one without careful coordination
 *   with bash.c's blocking waitpid() could introduce EINTR races.
 *
 *   The loop_run() zombie sweep (waitpid(-1,NULL,WNOHANG) added in loop.c)
 *   acts as a safety net for any orphaned children that slip past the normal
 *   bash.c reaping path (e.g. after SIGKILL of a timed-out child group).
 *   SIGCHLD remains SIG_DFL so the default behaviour (marking children as
 *   zombies until waited) is preserved — this lets bash.c's explicit
 *   waitpid() collect the exit status before the sweep can steal it.
 */

#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

struct SessionLog {
    char   path[512];
    char   rotated_path[516]; /* path + ".1" */
    size_t max_bytes;
    FILE  *fp;
    size_t bytes_written;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static size_t file_size_fd(FILE *fp)
{
    struct stat st;
    if (fstat(fileno(fp), &st) < 0)
        return 0;
    return (size_t)st.st_size;
}

/*
 * Escape `src` into `dst` (capacity `cap`, must be >= 1 for NUL).
 * Returns bytes written, not including the NUL terminator.
 * Stops encoding when fewer than 7 bytes remain (worst-case \u00XX).
 */
static size_t json_escape(char *dst, size_t cap, const char *src)
{
    size_t w = 0;
    if (!src || cap < 1)
        return 0;
    for (const unsigned char *p = (const unsigned char *)src;
         *p && w + 7 < cap; p++) {
        if (*p == '"')       { dst[w++] = '\\'; dst[w++] = '"';  }
        else if (*p == '\\') { dst[w++] = '\\'; dst[w++] = '\\'; }
        else if (*p == '\n') { dst[w++] = '\\'; dst[w++] = 'n';  }
        else if (*p == '\r') { dst[w++] = '\\'; dst[w++] = 'r';  }
        else if (*p == '\t') { dst[w++] = '\\'; dst[w++] = 't';  }
        else if (*p < 0x20)  {
            w += (size_t)snprintf(dst + w, 7, "\\u00%02x", (unsigned)*p);
        } else {
            dst[w++] = (char)*p;
        }
    }
    dst[w] = '\0';
    return w;
}

/*
 * Rotate: close current file, rename to .1, reopen fresh.
 * Rotation errors are silently ignored — logging is best-effort.
 */
static void rotate(SessionLog *sl)
{
    if (sl->fp) {
        fclose(sl->fp);
        sl->fp = NULL;
    }
    rename(sl->path, sl->rotated_path); /* best-effort; ignore errors */
    sl->fp = fopen(sl->path, "a");
    sl->bytes_written = 0;
}

/* Write `len` bytes, rotating first if the threshold would be exceeded. */
static void write_line(SessionLog *sl, const char *line, size_t len)
{
    if (!sl->fp || len == 0)
        return;
    if (sl->bytes_written + len >= sl->max_bytes)
        rotate(sl);
    if (!sl->fp)
        return;
    fwrite(line, 1, len, sl->fp);
    fflush(sl->fp);
    sl->bytes_written += len;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

SessionLog *session_log_open(const char *path, size_t max_bytes)
{
    if (!path)
        return NULL;

    SessionLog *sl = calloc(1, sizeof(SessionLog));
    if (!sl)
        return NULL;

    snprintf(sl->path,         sizeof(sl->path),         "%s",    path);
    snprintf(sl->rotated_path, sizeof(sl->rotated_path), "%s.1",  path);
    sl->max_bytes = (max_bytes > 0) ? max_bytes : SESSION_LOG_DEFAULT_MAX_BYTES;

    sl->fp = fopen(path, "a");
    if (!sl->fp) {
        free(sl);
        return NULL;
    }

    sl->bytes_written = file_size_fd(sl->fp);
    return sl;
}

void session_log_close(SessionLog *sl)
{
    if (!sl)
        return;
    if (sl->fp)
        fclose(sl->fp);
    free(sl);
}

void session_log_start(SessionLog *sl, int pid)
{
    if (!sl)
        return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%lld,\"type\":\"start\",\"pid\":%d}\n",
        (long long)time(NULL), pid);
    if (n > 0)
        write_line(sl, buf, (size_t)n);
}

void session_log_child_spawn(SessionLog *sl, pid_t child, const char *cmd)
{
    if (!sl)
        return;
    char esc[512];
    json_escape(esc, sizeof(esc), cmd);
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%lld,\"type\":\"child_spawn\",\"pid\":%lld,\"cmd\":\"%s\"}\n",
        (long long)time(NULL), (long long)child, esc);
    if (n > 0)
        write_line(sl, buf, (size_t)n);
}

void session_log_child_reap(SessionLog *sl, pid_t child, int exit_code)
{
    if (!sl)
        return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%lld,\"type\":\"child_reap\",\"pid\":%lld,\"exit\":%d}\n",
        (long long)time(NULL), (long long)child, exit_code);
    if (n > 0)
        write_line(sl, buf, (size_t)n);
}

void session_log_event(SessionLog *sl, const char *type, const char *detail)
{
    if (!sl || !type)
        return;
    char esc[256];
    json_escape(esc, sizeof(esc), detail ? detail : "");
    char buf[384];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%lld,\"type\":\"%s\",\"detail\":\"%s\"}\n",
        (long long)time(NULL), type, esc);
    if (n > 0)
        write_line(sl, buf, (size_t)n);
}
