/*
 * prompt.h — system prompt builder
 *
 * Assembles the system prompt from project context:
 *   1. Base identity
 *   2. Project detection (Makefile, package.json, Cargo.toml, …)
 *   3. CLAUDE.md / .nanocode.md injection
 *   4. Git status (git status --short, git log --oneline -5)
 *   5. Available tools declaration
 *   6. Working directory
 *   7. Sandbox policy block (if sandbox is enabled)
 */

#ifndef PROMPT_H
#define PROMPT_H

#include "../util/arena.h"
#include "../../include/sandbox.h"

/*
 * Build the system prompt from project context.
 *
 * `arena` — all output is arena-allocated; caller owns no separate memory.
 * `cwd`   — directory to inspect for build files and config files.
 * `exec`  — reserved for future non-global executor; pass NULL for now.
 * `sc`    — sandbox config; pass NULL or a config with enabled==0 to omit
 *            the sandbox policy block.
 *
 * Returns an arena-allocated NUL-terminated string, or NULL on allocation
 * failure.
 */
char *prompt_build(Arena *arena, const char *cwd, void *exec,
                   const SandboxConfig *sc);

#endif /* PROMPT_H */
