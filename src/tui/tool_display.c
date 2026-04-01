/*
 * tool_display.c — terminal display of tool invocations and results
 */

#include "tool_display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * ANSI escape sequences
 * ---------------------------------------------------------------------- */

#define ANSI_RESET "\033[0m"
#define ANSI_DIM   "\033[2m"
#define ANSI_RED   "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_CYAN  "\033[36m"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void fd_write(int fd, const char *s, size_t n)
{
    if (n > 0)
        (void)write(fd, s, n);
}

static void fd_puts(int fd, const char *s)
{
    fd_write(fd, s, strlen(s));
}

/*
 * Write one line of tool output to fd, indented 2 spaces and wrapped
 * (hard-wrapped, not word-wrapped) at TOOL_DISPLAY_WIDTH - 2 = 78 chars.
 */
#define INNER_WIDTH (TOOL_DISPLAY_WIDTH - 2)

static void write_indented_line(int fd, const char *line, size_t len)
{
    const char indent[] = "  ";

    while (len > 0) {
        size_t chunk = len > (size_t)INNER_WIDTH ? (size_t)INNER_WIDTH : len;
        fd_write(fd, indent, sizeof(indent) - 1);
        fd_write(fd, line, chunk);
        fd_puts(fd, "\n");
        line += chunk;
        len  -= chunk;
    }
}

/* Return current time as milliseconds (CLOCK_MONOTONIC). */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* -------------------------------------------------------------------------
 * tool_display_invocation
 * ---------------------------------------------------------------------- */

void tool_display_invocation(int fd, const char *tool,
                             const char *args_summary)
{
    /*
     * Format:  > tool: args_summary
     * Visible: 2 ("> ") + len(tool) + 2 (": ") + len(args) <= TOOL_DISPLAY_WIDTH
     */
    const char *t = tool          ? tool          : "";
    const char *a = args_summary  ? args_summary  : "";

    char  tbuf[32];           /* truncated tool name */
    char  abuf[TOOL_DISPLAY_WIDTH]; /* truncated args */
    char  line[TOOL_DISPLAY_WIDTH + 64]; /* extra room for ANSI codes */
    int   n;

    /* Truncate tool name at 28 chars. */
    snprintf(tbuf, sizeof(tbuf), "%s", t);

    /* Args get whatever remains of the 80-column budget. */
    int tlen    = (int)strlen(tbuf);
    int args_max = TOOL_DISPLAY_WIDTH - 4 - tlen; /* 4 = "> " + ": " */
    if (args_max < 0)
        args_max = 0;

    if ((int)strlen(a) > args_max && args_max > 3) {
        snprintf(abuf, sizeof(abuf), "%.*s...", args_max - 3, a);
    } else {
        snprintf(abuf, (size_t)(args_max + 1 < (int)sizeof(abuf)
                                    ? args_max + 1
                                    : (int)sizeof(abuf)),
                 "%s", a);
    }

    n = snprintf(line, sizeof(line),
                 ANSI_DIM "> %s: %s" ANSI_RESET "\n", tbuf, abuf);
    if (n > 0)
        fd_write(fd, line, (size_t)n);
}

/* -------------------------------------------------------------------------
 * tool_display_result
 * ---------------------------------------------------------------------- */

void tool_display_result(int fd, const ToolResult *r)
{
    if (!r)
        return;

    if (r->error) {
        tool_display_error(fd, r->content);
        return;
    }

    if (!r->content || r->len == 0) {
        fd_puts(fd, "  " ANSI_DIM "(empty)" ANSI_RESET "\n");
        return;
    }

    /* Count total lines in the result. */
    const char *p   = r->content;
    const char *end = r->content + r->len;
    int total_lines = 0;
    for (const char *q = p; q < end; q++) {
        if (*q == '\n')
            total_lines++;
    }
    /* Last line without trailing newline counts too. */
    if (end > p && *(end - 1) != '\n')
        total_lines++;

    /* Display up to TOOL_DISPLAY_MAX_LINES lines. */
    int shown = 0;
    while (p < end && shown < TOOL_DISPLAY_MAX_LINES) {
        const char *nl      = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t      linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

        write_indented_line(fd, p, linelen);
        shown++;
        p = nl ? nl + 1 : end;
    }

    int remaining = total_lines - shown;
    if (remaining > 0) {
        char footer[128];
        int  n = snprintf(footer, sizeof(footer),
                          ANSI_DIM "[%d more line%s \xe2\x80\x94"
                          " press T to expand]" ANSI_RESET "\n",
                          remaining, remaining == 1 ? "" : "s");
        if (n > 0)
            fd_write(fd, footer, (size_t)n);
    }
}

/* -------------------------------------------------------------------------
 * tool_display_diff
 * ---------------------------------------------------------------------- */

void tool_display_diff(int fd, const char *diff_text)
{
    if (!diff_text)
        return;

    const char *p   = diff_text;
    const char *end = diff_text + strlen(diff_text);

    while (p < end) {
        const char *nl      = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t      linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

        const char *color;
        switch (*p) {
        case '+': color = ANSI_GREEN; break;
        case '-': color = ANSI_RED;   break;
        case '@': color = ANSI_CYAN;  break;
        default:  color = ANSI_DIM;   break;
        }

        fd_puts(fd, color);
        fd_write(fd, p, linelen);
        fd_puts(fd, ANSI_RESET "\n");

        p = nl ? nl + 1 : end;
    }
}

/* -------------------------------------------------------------------------
 * tool_display_error
 * ---------------------------------------------------------------------- */

void tool_display_error(int fd, const char *msg)
{
    char buf[512];
    int  n = snprintf(buf, sizeof(buf),
                      ANSI_RED "! tool error: %s" ANSI_RESET "\n",
                      msg ? msg : "");
    if (n > 0)
        fd_write(fd, buf, (size_t)n);
}

/* -------------------------------------------------------------------------
 * Progress spinner
 * ---------------------------------------------------------------------- */

static const char SPINNER_FRAMES[] = { '-', '\\', '|', '/' };

void tool_progress_start(ToolProgress *p, int fd)
{
    p->fd       = fd;
    p->frame    = 0;
    p->shown    = 0;
    p->start_ms = now_ms();
}

void tool_progress_tick(ToolProgress *p)
{
    if (!p)
        return;

    long elapsed = now_ms() - p->start_ms;
    if (elapsed < 500)
        return;

    char buf[8];
    int  n;

    if (p->shown) {
        /* Overwrite the previous glyph with carriage-return. */
        n = snprintf(buf, sizeof(buf), "\r%c", SPINNER_FRAMES[p->frame]);
    } else {
        n = snprintf(buf, sizeof(buf), "%c", SPINNER_FRAMES[p->frame]);
        p->shown = 1;
    }

    p->frame = (p->frame + 1) & 3;

    if (n > 0)
        fd_write(p->fd, buf, (size_t)n);
}

void tool_progress_stop(ToolProgress *p)
{
    if (!p)
        return;

    if (p->shown) {
        /* Erase the spinner glyph. */
        fd_puts(p->fd, "\r \r");
        p->shown = 0;
    }
}
