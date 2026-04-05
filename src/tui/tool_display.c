/*
 * tool_display.c — terminal display of tool invocations and results
 */

#include "tool_display.h"
#include "spinner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Detect whether the terminal supports 256 colours. */
static int detect_256color(void)
{
    const char *term  = getenv("TERM");
    const char *cterm = getenv("COLORTERM");
    if (term  && strstr(term,  "256color"))    return 1;
    if (cterm && (strcmp(cterm,"truecolor")==0 ||
                  strcmp(cterm,"24bit")==0))    return 1;
    return 0;
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

    /* Top border: dim ╭──────... to visually open the tool block. */
    {
        /* Layout: DIM(3) + ╭(3) + (WIDTH-1)×─(3) + RESET(3) + '\n'(1) = 247;
         * TOOL_DISPLAY_WIDTH*3 = 240 accounts for the UTF-8 chars, +32 covers ANSI sequences */
        char border[TOOL_DISPLAY_WIDTH * 3 + 32];
        int bpos = 0;
        /* DIM + ╭ (U+256D: \xe2\x95\xad) */
        memcpy(border + bpos, ANSI_DIM, sizeof(ANSI_DIM) - 1);
        bpos += sizeof(ANSI_DIM) - 1;
        memcpy(border + bpos, "\xe2\x95\xad", 3); bpos += 3; /* ╭ */
        /* Fill: (TOOL_DISPLAY_WIDTH - 1) × ─ (U+2500: \xe2\x94\x80) */
        for (int i = 1; i < TOOL_DISPLAY_WIDTH; i++) {
            memcpy(border + bpos, "\xe2\x94\x80", 3); bpos += 3;
        }
        memcpy(border + bpos, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        bpos += sizeof(ANSI_RESET) - 1;
        border[bpos++] = '\n';
        fd_write(fd, border, (size_t)bpos);
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
 * Progress spinner — braille glyph via Spinner API
 * ---------------------------------------------------------------------- */

void tool_progress_start(ToolProgress *p, int fd)
{
    spinner_start(&p->sp, fd, detect_256color());
}

void tool_progress_tick(ToolProgress *p)
{
    if (!p)
        return;
    spinner_tick(&p->sp);
}

void tool_progress_stop(ToolProgress *p)
{
    if (!p)
        return;
    spinner_stop(&p->sp);
}
