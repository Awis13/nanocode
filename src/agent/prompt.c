/*
 * prompt.c — system prompt builder
 *
 * Sections assembled in order:
 *   1. Base identity
 *   2. Project detection
 *   3. CLAUDE.md / .nanocode.md injection
 *   4. Git status
 *   5. Available tools
 *   6. Working directory
 */

#include "prompt.h"
#include "../tools/executor.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Check whether `path` exists as a regular file.
 * Returns 1 if it exists, 0 otherwise.
 */
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Read the entire contents of `path` into a heap-allocated buffer.
 * Caller must free() the returned pointer.
 * Returns NULL on failure or if the file is empty.
 */
static char *read_file_heap(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    /* Determine size. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/*
 * Shell-escape a path for use in a single-quoted shell argument.
 * Writes the escaped path (with surrounding single quotes) into `dst`.
 * dst must be at least strlen(src)*4 + 3 bytes.
 * Returns number of bytes written (excluding NUL).
 */
static size_t shell_quote(char *dst, const char *src)
{
    size_t out = 0;
    dst[out++] = '\'';
    for (const char *p = src; *p; p++) {
        if (*p == '\'') {
            /* End quote, escaped quote, re-open quote: '\'' */
            dst[out++] = '\'';
            dst[out++] = '\\';
            dst[out++] = '\'';
            dst[out++] = '\'';
        } else {
            dst[out++] = *p;
        }
    }
    dst[out++] = '\'';
    dst[out]   = '\0';
    return out;
}

/*
 * Run `cmd` via popen and append all its stdout to `b`.
 * Silently ignores errors.
 */
static void run_cmd_to_buf(Buf *b, const char *cmd)
{
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return;

    char chunk[256];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
        buf_append(b, chunk, n);

    pclose(fp);
}

/* -------------------------------------------------------------------------
 * prompt_build
 * ---------------------------------------------------------------------- */

char *prompt_build(Arena *arena, const char *cwd, void *exec)
{
    (void)exec; /* reserved — tools come from the global registry */

    if (!arena || !cwd)
        return NULL;

    Buf b;
    buf_init(&b);

    /* ------------------------------------------------------------------
     * 1. Base identity
     * ------------------------------------------------------------------ */
    buf_append_str(&b,
        "You are nanocode, an AI coding agent. "
        "You help users read, write, and reason about code. "
        "You have access to tools for running shell commands, reading and "
        "writing files, and searching. Use them to complete tasks.\n\n");

    /* ------------------------------------------------------------------
     * 2. Project detection
     * ------------------------------------------------------------------ */
    static const struct {
        const char *file;
        const char *desc;
    } probes[] = {
        { "Makefile",       "C/C++ project (Makefile)" },
        { "package.json",   "Node.js project"          },
        { "Cargo.toml",     "Rust project"             },
        { "go.mod",         "Go project"               },
        { "pyproject.toml", "Python project"           },
        { "setup.py",       "Python project"           },
        { "CMakeLists.txt", "CMake project"            },
        { NULL, NULL }
    };

    size_t cwdlen = strlen(cwd);

    for (int i = 0; probes[i].file; i++) {
        size_t flen  = strlen(probes[i].file);
        /* cwd + "/" + file + NUL */
        char *probe_path = malloc(cwdlen + 1 + flen + 1);
        if (!probe_path)
            continue;
        memcpy(probe_path, cwd, cwdlen);
        probe_path[cwdlen] = '/';
        memcpy(probe_path + cwdlen + 1, probes[i].file, flen + 1);

        int found = file_exists(probe_path);
        free(probe_path);

        if (found) {
            buf_append_str(&b, "## Project\nDetected: ");
            buf_append_str(&b, probes[i].desc);
            buf_append_str(&b, "\n\n");
            break;
        }
    }

    /* ------------------------------------------------------------------
     * 3. CLAUDE.md / .nanocode.md injection
     * ------------------------------------------------------------------ */
    static const char *const cfg_files[] = {
        "CLAUDE.md",
        ".nanocode.md",
        NULL
    };

    for (int i = 0; cfg_files[i]; i++) {
        size_t flen   = strlen(cfg_files[i]);
        char  *fpath  = malloc(cwdlen + 1 + flen + 1);
        if (!fpath)
            continue;
        memcpy(fpath, cwd, cwdlen);
        fpath[cwdlen] = '/';
        memcpy(fpath + cwdlen + 1, cfg_files[i], flen + 1);

        char *content = read_file_heap(fpath);
        free(fpath);

        if (content) {
            buf_append_str(&b, "## Project Instructions\n");
            buf_append_str(&b, content);
            free(content);
            if (b.len > 0 && b.data[b.len - 1] != '\n')
                buf_append_str(&b, "\n");
            buf_append_str(&b, "\n");
            break; /* use first one found */
        }
    }

    /* ------------------------------------------------------------------
     * 4. Git status
     *
     * Subprocess spawning (popen/fork) hangs under AddressSanitizer on macOS
     * due to ASan's fork() interception.  Guard with __SANITIZE_ADDRESS__ so
     * ASan builds remain clean; the section is exercised in normal builds.
     * ------------------------------------------------------------------ */
#ifndef __SANITIZE_ADDRESS__
    /* Build a quoted cwd for shell use — worst case 4x expansion + quotes. */
    char *quoted_cwd = malloc(cwdlen * 4 + 3);
    if (quoted_cwd) {
        shell_quote(quoted_cwd, cwd);

        /* git status --short */
        {
            Buf git_status;
            buf_init(&git_status);

            /* Build command: git -C <cwd> status --short 2>/dev/null */
            size_t cmdlen = strlen("git -C  status --short 2>/dev/null")
                          + strlen(quoted_cwd) + 1;
            char *cmd = malloc(cmdlen);
            if (cmd) {
                snprintf(cmd, cmdlen, "git -C %s status --short 2>/dev/null",
                         quoted_cwd);
                run_cmd_to_buf(&git_status, cmd);
                free(cmd);
            }

            if (git_status.len > 0) {
                buf_append_str(&b, "## Git Status\n```\n");
                buf_append(&b, git_status.data, git_status.len);
                buf_append_str(&b, "```\n\n");
            }
            buf_destroy(&git_status);
        }

        /* git log --oneline -5 */
        {
            Buf git_log;
            buf_init(&git_log);

            size_t cmdlen = strlen("git -C  log --oneline -5 2>/dev/null")
                          + strlen(quoted_cwd) + 1;
            char *cmd = malloc(cmdlen);
            if (cmd) {
                snprintf(cmd, cmdlen, "git -C %s log --oneline -5 2>/dev/null",
                         quoted_cwd);
                run_cmd_to_buf(&git_log, cmd);
                free(cmd);
            }

            if (git_log.len > 0) {
                buf_append_str(&b, "## Recent Commits\n```\n");
                buf_append(&b, git_log.data, git_log.len);
                buf_append_str(&b, "```\n\n");
            }
            buf_destroy(&git_log);
        }

        free(quoted_cwd);
    }
#endif /* __SANITIZE_ADDRESS__ */

    /* ------------------------------------------------------------------
     * 5. Available tools
     * ------------------------------------------------------------------ */
    {
        const char *names[TOOL_REGISTRY_MAX];
        int count = tool_list_names(names, TOOL_REGISTRY_MAX);
        if (count > 0) {
            buf_append_str(&b, "## Available Tools\n");
            for (int i = 0; i < count; i++) {
                buf_append_str(&b, "- ");
                buf_append_str(&b, names[i]);
                buf_append_str(&b, "\n");
            }
            buf_append_str(&b, "\n");
        }
    }

    /* ------------------------------------------------------------------
     * 6. Working directory
     * ------------------------------------------------------------------ */
    buf_append_str(&b, "## Working Directory\n");
    buf_append_str(&b, cwd);
    buf_append_str(&b, "\n");

    /* ------------------------------------------------------------------
     * Copy result to arena
     * ------------------------------------------------------------------ */
    char *result = NULL;
    if (b.len > 0) {
        result = arena_alloc(arena, b.len + 1);
        if (result) {
            const char *s = buf_str(&b);
            memcpy(result, s, b.len + 1);
        }
    }

    buf_destroy(&b);
    return result;
}
