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
#include "git.h"
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
     * 4. Git status and recent commits (delegated to git.c)
     * ------------------------------------------------------------------ */
    {
        char *git_ctx = git_context_summary(arena, cwd);
        if (git_ctx && git_ctx[0])
            buf_append_str(&b, git_ctx);
    }

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
