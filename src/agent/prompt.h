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
 */

#ifndef PROMPT_H
#define PROMPT_H

#include "../util/arena.h"

/*
 * Build the system prompt from project context.
 *
 * `arena` — all output is arena-allocated; caller owns no separate memory.
 * `cwd`   — directory to inspect for build files and config files.
 * `exec`  — reserved for future non-global executor; pass NULL for now.
 *
 * Returns an arena-allocated NUL-terminated string, or NULL on allocation
 * failure.
 */
char *prompt_build(Arena *arena, const char *cwd, void *exec);

#endif /* PROMPT_H */
