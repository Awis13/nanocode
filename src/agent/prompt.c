/*
 * prompt.c — system prompt builder
 *
 * Sections assembled in order:
 *   1. Base identity
 *   2. Project detection
 *   3. CLAUDE.md / .nanocode.md injection
 *   4. Git status           \
 *   5. Environment bootstrap  > all skipped in ASan builds (popen/fork hazard)
 *                            /
 *   6. Available tools
 *   7. Working directory
 *
 * ASan guard rationale (PROMPT_NO_POPEN, defined below and in prompt.h):
 *   popen() calls fork() internally.  On macOS, AddressSanitizer instruments
 *   fork() and the child process can deadlock holding ASan-internal locks.
 *   This is a known libclang_rt.asan + popen interaction on Darwin.
 *
 *   GCC defines __SANITIZE_ADDRESS__ when -fsanitize=address is active.
 *   Clang exposes __has_feature(address_sanitizer) but does NOT always set
 *   __SANITIZE_ADDRESS__, so a GCC-only guard silently fails on Apple Clang.
 *   PROMPT_NO_POPEN checks both to cover all compilers.
 */

#include "prompt.h"
#include "../../include/sandbox.h"
#include "../tools/executor.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Internal helpers — always compiled
 * ---------------------------------------------------------------------- */

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file_heap(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0)                     { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * popen helpers — compiled only when not in an ASan build.
 *
 * Guarding the *definitions* (not just call sites) avoids the
 * -Wunused-function warning that would fire when the call sites are
 * excluded by PROMPT_NO_POPEN.
 * ---------------------------------------------------------------------- */
#if !PROMPT_NO_POPEN

static size_t shell_quote(char *dst, const char *src)
{
    size_t out = 0;
    dst[out++] = '\'';
    for (const char *p = src; *p; p++) {
        if (*p == '\'') {
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

#endif /* !PROMPT_NO_POPEN */

/* -------------------------------------------------------------------------
 * prompt_build
 * ---------------------------------------------------------------------- */

char *prompt_build(Arena *arena, const char *cwd, void *exec,
                   const SandboxConfig *sc)
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
    static const struct { const char *file; const char *desc; } probes[] = {
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
        size_t flen      = strlen(probes[i].file);
        char  *probe_path = malloc(cwdlen + 1 + flen + 1);
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
    static const char *const cfg_files[] = { "CLAUDE.md", ".nanocode.md", NULL };

    for (int i = 0; cfg_files[i]; i++) {
        size_t flen  = strlen(cfg_files[i]);
        char  *fpath = malloc(cwdlen + 1 + flen + 1);
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
            break;
        }
    }

    /* ------------------------------------------------------------------
     * 4. Git status  +  5. Environment bootstrap
     *
     * All popen() calls live inside this block.  See PROMPT_NO_POPEN.
     * ------------------------------------------------------------------ */
#if !PROMPT_NO_POPEN
    {
        char *qcwd = malloc(cwdlen * 4 + 3);
        if (qcwd) {
            shell_quote(qcwd, cwd);

            /* git status --short */
            {
                Buf gs; buf_init(&gs);
                size_t len = strlen("git -C  status --short 2>/dev/null")
                           + strlen(qcwd) + 1;
                char *cmd = malloc(len);
                if (cmd) {
                    snprintf(cmd, len,
                             "git -C %s status --short 2>/dev/null", qcwd);
                    run_cmd_to_buf(&gs, cmd);
                    free(cmd);
                }
                if (gs.len > 0) {
                    buf_append_str(&b, "## Git Status\n```\n");
                    buf_append(&b, gs.data, gs.len);
                    buf_append_str(&b, "```\n\n");
                }
                buf_destroy(&gs);
            }

            /* git log --oneline -5 */
            {
                Buf gl; buf_init(&gl);
                size_t len = strlen("git -C  log --oneline -5 2>/dev/null")
                           + strlen(qcwd) + 1;
                char *cmd = malloc(len);
                if (cmd) {
                    snprintf(cmd, len,
                             "git -C %s log --oneline -5 2>/dev/null", qcwd);
                    run_cmd_to_buf(&gl, cmd);
                    free(cmd);
                }
                if (gl.len > 0) {
                    buf_append_str(&b, "## Recent Commits\n```\n");
                    buf_append(&b, gl.data, gl.len);
                    buf_append_str(&b, "```\n\n");
                }
                buf_destroy(&gl);
            }

            free(qcwd);
        }
    }

    /* -- 5. Environment bootstrap -- */
    {
        static const char *const compilers[] = {
            "cc", "gcc", "clang", "rustc", "go", "node", "python3", NULL
        };
        static const char *const pkgmgrs[] = {
            "npm", "pip", "pip3", "cargo", "apt", "brew", NULL
        };

        /* Compilers/runtimes */
        Buf found; buf_init(&found);
        for (int i = 0; compilers[i]; i++) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", compilers[i]);
            Buf tmp; buf_init(&tmp);
            run_cmd_to_buf(&tmp, cmd);
            if (tmp.len > 0) {
                buf_append_str(&found, compilers[i]);
                buf_append_str(&found, " ");
            }
            buf_destroy(&tmp);
        }
        if (found.len > 0) {
            buf_append_str(&b, "## Environment\nCompilers/runtimes: ");
            buf_append(&b, found.data, found.len);
            buf_append_str(&b, "\n");
        }
        buf_destroy(&found);

        /* Package managers */
        Buf pkgs; buf_init(&pkgs);
        for (int i = 0; pkgmgrs[i]; i++) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", pkgmgrs[i]);
            Buf tmp; buf_init(&tmp);
            run_cmd_to_buf(&tmp, cmd);
            if (tmp.len > 0) {
                buf_append_str(&pkgs, pkgmgrs[i]);
                buf_append_str(&pkgs, " ");
            }
            buf_destroy(&tmp);
        }
        if (pkgs.len > 0) {
            buf_append_str(&b, "Package managers: ");
            buf_append(&b, pkgs.data, pkgs.len);
            buf_append_str(&b, "\n");
        }
        buf_destroy(&pkgs);

        /* Shell */
        const char *shell = getenv("SHELL");
        if (shell && shell[0]) {
            buf_append_str(&b, "Shell: ");
            buf_append_str(&b, shell);
            buf_append_str(&b, "\n");
        }

        /* Working-directory disk usage */
        {
            char *qcwd2 = malloc(cwdlen * 4 + 3);
            if (qcwd2) {
                shell_quote(qcwd2, cwd);
                char cmd[768];
                snprintf(cmd, sizeof(cmd),
                         "du -sh %s 2>/dev/null | awk '{print $1}'", qcwd2);
                free(qcwd2);
                Buf du; buf_init(&du);
                run_cmd_to_buf(&du, cmd);
                if (du.len > 0) {
                    while (du.len > 0 &&
                           (du.data[du.len-1] == '\n' ||
                            du.data[du.len-1] == '\r'))
                        du.len--;
                    buf_append_str(&b, "Working directory size: ");
                    buf_append(&b, du.data, du.len);
                    buf_append_str(&b, "\n");
                }
                buf_destroy(&du);
            }
        }

        buf_append_str(&b, "\n");
    }
#endif /* !PROMPT_NO_POPEN */

    /* ------------------------------------------------------------------
     * 6. Available tools
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
     * 7. Working directory
     * ------------------------------------------------------------------ */
    buf_append_str(&b, "## Working Directory\n");
    buf_append_str(&b, cwd);
    buf_append_str(&b, "\n");

    /* ------------------------------------------------------------------
     * 8. Sandbox policy block (only when sandbox is active)
     * ------------------------------------------------------------------ */
    if (sc && sc->enabled) {
        char sbox[512];
        sandbox_build_prompt_block(sc, sbox, sizeof(sbox));
        if (sbox[0] != '\0') {
            buf_append_str(&b, "\n");
            buf_append_str(&b, sbox);
            buf_append_str(&b, "\n");
        }
    }

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
