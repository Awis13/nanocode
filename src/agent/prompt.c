/*
 * prompt.c — system prompt builder
 *
 * Sections assembled in order:
 *
 * Static (cache-safe, rarely changes):
 *   1. Base identity
 *   2. Available tools
 *   3. Sandbox policy block (if enabled)
 *
 * Dynamic (per-turn, rebuilt each call):
 *   4. Project detection
 *   5. CLAUDE.md / .nanocode.md injection
 *   6. Git status           \
 *   7. Environment bootstrap  > skipped in ASan builds (popen/fork hazard)
 *                            /
 *   8. Working directory
 *   9. Budget hint (when budget_tokens > 0)
 *
 * ASan guard rationale (PROMPT_NO_POPEN, defined in prompt.h):
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
#include "git.h"
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

/* Copy a Buf into the arena; returns arena-allocated string or NULL. */
static const char *buf_to_arena(Arena *arena, Buf *b)
{
    if (b->len == 0)
        return arena_alloc(arena, 1); /* empty string "\0" */
    char *out = arena_alloc(arena, b->len + 1);
    if (!out)
        return NULL;
    const char *s = buf_str(b);
    memcpy(out, s, b->len + 1);
    return out;
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
 * prompt_build_parts
 * ---------------------------------------------------------------------- */

PromptParts prompt_build_parts(Arena *arena, const char *cwd, void *exec,
                                const SandboxConfig *sc, size_t budget_tokens,
                                int no_git)
{
    (void)exec; /* reserved — tools come from the global registry */

    PromptParts result = { NULL, NULL };

    if (!arena || !cwd)
        return result;

    /* ==================================================================
     * STATIC SECTION
     * Rarely changes across turns — safe to cache at a prompt boundary.
     * ================================================================== */
    Buf bs;
    buf_init(&bs);

    /* ------------------------------------------------------------------
     * 1. Base identity
     * ------------------------------------------------------------------ */
    buf_append_str(&bs,
        "You are nanocode, an AI coding agent. "
        "You help users read, write, and reason about code. "
        "You have access to tools for running shell commands, reading and "
        "writing files, and searching. Use them to complete tasks.\n\n");

    /* ------------------------------------------------------------------
     * 2. Available tools
     * ------------------------------------------------------------------ */
    {
        const char *names[TOOL_REGISTRY_MAX];
        int count = tool_list_names(names, TOOL_REGISTRY_MAX);
        if (count > 0) {
            buf_append_str(&bs, "## Available Tools\n");
            for (int i = 0; i < count; i++) {
                buf_append_str(&bs, "- ");
                buf_append_str(&bs, names[i]);
                buf_append_str(&bs, "\n");
            }
            buf_append_str(&bs, "\n");
        }
    }

    /* ------------------------------------------------------------------
     * 3. Sandbox policy block (only when sandbox is active)
     * ------------------------------------------------------------------ */
    if (sc && sc->enabled) {
        char sbox[512];
        sandbox_build_prompt_block(sc, sbox, sizeof(sbox));
        if (sbox[0] != '\0') {
            buf_append_str(&bs, "\n");
            buf_append_str(&bs, sbox);
            buf_append_str(&bs, "\n");
        }
    }

    result.static_part = buf_to_arena(arena, &bs);
    buf_destroy(&bs);

    /* ==================================================================
     * DYNAMIC SECTION
     * Rebuilt each turn — contains project context, runtime state, and
     * the optional per-turn budget hint.
     * ================================================================== */
    Buf bd;
    buf_init(&bd);

    size_t cwdlen = strlen(cwd);

    /* ------------------------------------------------------------------
     * 4. Project detection
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
            buf_append_str(&bd, "## Project\nDetected: ");
            buf_append_str(&bd, probes[i].desc);
            buf_append_str(&bd, "\n\n");
            break;
        }
    }

    /* ------------------------------------------------------------------
     * 5. CLAUDE.md / .nanocode.md injection
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
            buf_append_str(&bd, "## Project Instructions\n");
            buf_append_str(&bd, content);
            free(content);
            if (bd.len > 0 && bd.data[bd.len - 1] != '\n')
                buf_append_str(&bd, "\n");
            buf_append_str(&bd, "\n");
            break;
        }
    }

    /* ------------------------------------------------------------------
     * 6. Git context  +  7. Environment bootstrap
     *
     * All popen() calls live inside this block.  See PROMPT_NO_POPEN.
     * ------------------------------------------------------------------ */
#if !PROMPT_NO_POPEN
    /* -- 6. Git context (branch, diffs, untracked files, recent commits) -- */
    if (!no_git) {
        char *git_ctx = git_context_summary(arena, cwd);
        if (git_ctx && git_ctx[0]) {
            buf_append_str(&bd, git_ctx);
            if (bd.len > 0 && bd.data[bd.len - 1] != '\n')
                buf_append_str(&bd, "\n");
            buf_append_str(&bd, "\n");
        }
    }

    /* -- 7. Environment bootstrap -- */
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
            buf_append_str(&bd, "## Environment\nCompilers/runtimes: ");
            buf_append(&bd, found.data, found.len);
            buf_append_str(&bd, "\n");
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
            buf_append_str(&bd, "Package managers: ");
            buf_append(&bd, pkgs.data, pkgs.len);
            buf_append_str(&bd, "\n");
        }
        buf_destroy(&pkgs);

        /* Shell */
        const char *shell = getenv("SHELL");
        if (shell && shell[0]) {
            buf_append_str(&bd, "Shell: ");
            buf_append_str(&bd, shell);
            buf_append_str(&bd, "\n");
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
                    buf_append_str(&bd, "Working directory size: ");
                    buf_append(&bd, du.data, du.len);
                    buf_append_str(&bd, "\n");
                }
                buf_destroy(&du);
            }
        }

        buf_append_str(&bd, "\n");
    }
#endif /* !PROMPT_NO_POPEN */

    /* ------------------------------------------------------------------
     * 8. Working directory
     * ------------------------------------------------------------------ */
    buf_append_str(&bd, "## Working Directory\n");
    buf_append_str(&bd, cwd);
    buf_append_str(&bd, "\n");

    /* ------------------------------------------------------------------
     * 9. Budget hint — injected only when budget_tokens > 0.
     *
     * A provider-agnostic plain-text note so the model can adapt its
     * verbosity as the context window fills.  Placed last so it is
     * always the freshest information in the dynamic section.
     * ------------------------------------------------------------------ */
    if (budget_tokens > 0) {
        char hint[64];
        snprintf(hint, sizeof(hint), "\nContext budget remaining: %zu tokens.\n",
                 budget_tokens);
        buf_append_str(&bd, hint);
    }

    result.dynamic_part = buf_to_arena(arena, &bd);
    buf_destroy(&bd);

    return result;
}

/* -------------------------------------------------------------------------
 * prompt_build — backward-compatible single-string variant
 * ---------------------------------------------------------------------- */

char *prompt_build(Arena *arena, const char *cwd, void *exec,
                   const SandboxConfig *sc, int no_git)
{
    PromptParts parts = prompt_build_parts(arena, cwd, exec, sc, 0, no_git);
    if (!parts.static_part && !parts.dynamic_part)
        return NULL;

    const char *s = parts.static_part  ? parts.static_part  : "";
    const char *d = parts.dynamic_part ? parts.dynamic_part : "";

    size_t slen = strlen(s);
    size_t dlen = strlen(d);
    size_t total = slen + dlen;
    if (total == 0)
        return NULL;

    char *result = arena_alloc(arena, total + 1);
    if (!result)
        return NULL;
    memcpy(result, s, slen);
    memcpy(result + slen, d, dlen + 1); /* include the NUL */
    return result;
}
