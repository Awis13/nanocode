/*
 * editor.c — open files in the user's terminal editor
 *
 * Resolves $VISUAL → $EDITOR → "vi", builds editor-specific line-jump argv,
 * suspends the current process (raw terminal teardown is the caller's
 * responsibility), forks, execs the editor, and waits for it to exit.
 *
 * Sandbox path check: the inline prefix walk below is a temporary stub.
 * TODO: replace with sandbox_check_path() once CMP-198 exports that symbol.
 */

#define _POSIX_C_SOURCE 200809L

#include "editor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Maximum path length we handle (matches PATH_MAX on most platforms). */
#define EDITOR_PATH_MAX 4096

/* -------------------------------------------------------------------------
 * sandbox path check — prefix walk
 *
 * Returns 1 if path starts with any colon-separated prefix in the list,
 * or if the list is NULL/"" (no restriction active).
 * Returns 0 if the list is non-empty and no prefix matches.
 * ---------------------------------------------------------------------- */

static int path_in_allowed(const char *path, const char *allowed_colon_sep)
{
    if (!allowed_colon_sep || !allowed_colon_sep[0]) return 1;

    char list[EDITOR_PATH_MAX];
    strncpy(list, allowed_colon_sep, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    char *p = list;
    while (p) {
        char *colon = strchr(p, ':');
        if (colon) *colon = '\0';
        if (*p && strncmp(path, p, strlen(p)) == 0)
            return 1;
        p = colon ? colon + 1 : NULL;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * editor_open
 * ---------------------------------------------------------------------- */

int editor_open(const char *path, int line, const char *sandbox_allowed_paths)
{
    if (!path || !path[0]) return -1;

    /* Sandbox check (TODO: replace with sandbox_check_path() from CMP-198). */
    if (!path_in_allowed(path, sandbox_allowed_paths)) {
        fprintf(stderr, "editor: sandbox denied: %s\n", path);
        return -1;
    }

    /* Resolve editor: $VISUAL → $EDITOR → "vi". */
    const char *editor = getenv("VISUAL");
    if (!editor || !editor[0]) editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vi";

    /* Determine editor family from basename. */
    const char *base = strrchr(editor, '/');
    base = base ? base + 1 : editor;

    /* Build argv — max 4 elements: editor [+flag] path NULL. */
    char        line_flag[32];
    const char *argv[4];
    int         argc = 0;

    argv[argc++] = editor;

    if (line > 0) {
        if (strcmp(base, "emacs") == 0 || strcmp(base, "emacsclient") == 0) {
            /* emacs: +line:col */
            snprintf(line_flag, sizeof(line_flag), "+%d:0", line);
        } else {
            /* vim, nvim, nano, vi and unknowns: +line */
            snprintf(line_flag, sizeof(line_flag), "+%d", line);
        }
        argv[argc++] = line_flag;
    }

    argv[argc++] = path;
    argv[argc]   = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("editor: fork");
        return -1;
    }

    if (pid == 0) {
        /* Child: exec the editor.  stdin/stdout/stderr are inherited so the
         * editor has full terminal access.  If exec fails _exit immediately
         * so atexit handlers (e.g. raw-mode restore) do not fire twice. */
        execvp(editor, (char *const *)argv);
        perror("editor: exec");
        _exit(127);
    }

    /* Parent: wait for the editor to exit. */
    int wstatus = 0;
    if (waitpid(pid, &wstatus, 0) < 0) {
        perror("editor: waitpid");
        return -1;
    }

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
        return 0;

    return -1;
}

/* -------------------------------------------------------------------------
 * editor_open_from_ref — parse "path:line" and call editor_open
 * ---------------------------------------------------------------------- */

int editor_open_from_ref(const char *ref, const char *sandbox_allowed_paths)
{
    if (!ref || !ref[0]) return -1;

    char path[EDITOR_PATH_MAX];
    int  line = 0;

    /* Find the last colon that is followed only by digits. */
    const char *colon = strrchr(ref, ':');
    if (colon && colon[1] != '\0') {
        /* Verify every character after the colon is a digit. */
        int all_digits = 1;
        for (const char *p = colon + 1; *p; p++) {
            if (*p < '0' || *p > '9') { all_digits = 0; break; }
        }
        if (all_digits) {
            line = atoi(colon + 1);
            size_t plen = (size_t)(colon - ref);
            if (plen == 0 || plen >= sizeof(path)) return -1;
            memcpy(path, ref, plen);
            path[plen] = '\0';
        } else {
            /* No valid line suffix — treat whole ref as path. */
            strncpy(path, ref, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    } else {
        strncpy(path, ref, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    return editor_open(path, line, sandbox_allowed_paths);
}
