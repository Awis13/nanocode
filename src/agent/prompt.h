/*
 * prompt.h — system prompt builder
 *
 * Assembles the system prompt from project context:
 *   1. Base identity          \
 *   2. Available tools         > static — rarely changes, safe to cache
 *   3. Sandbox policy block   /
 *
 *   4. Project detection      \
 *   5. CLAUDE.md / .nanocode.md\
 *   6. Git status              > dynamic — rebuilt each turn
 *   7. Environment bootstrap   |
 *   8. Working directory       |
 *   9. Budget hint (optional) /
 */

#ifndef PROMPT_H
#define PROMPT_H

#include "../util/arena.h"
#include "../../include/sandbox.h"
#include <stddef.h>

/*
 * PROMPT_NO_POPEN — set to 1 in any ASan build (GCC or Clang).
 *
 * popen() calls fork() which deadlocks under macOS ASan.
 * GCC defines __SANITIZE_ADDRESS__; Clang uses __has_feature().
 * Tests check this macro to skip popen-dependent cases.
 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define PROMPT_NO_POPEN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define PROMPT_NO_POPEN 1
#endif
#ifndef PROMPT_NO_POPEN
#  define PROMPT_NO_POPEN 0
#endif

/*
 * PromptParts — static/dynamic sections of the system prompt.
 *
 * static_part  — base identity, tools, sandbox: rarely changes across turns.
 *                Safe to use as a prompt-cache boundary for providers that
 *                support cache_control (e.g. Claude extended caching).
 * dynamic_part — project detection, CLAUDE.md, git status, environment,
 *                working directory, and optional budget hint: rebuilt each turn.
 *
 * Both fields are arena-allocated NUL-terminated strings.
 * Either may be NULL if arena allocation failed.
 */
typedef struct {
    const char *static_part;
    const char *dynamic_part;
} PromptParts;

/*
 * prompt_build_parts — build the system prompt split into static and dynamic
 * sections.
 *
 * `arena`         — all output is arena-allocated; caller owns no separate memory.
 * `cwd`           — directory to inspect for build files and config files.
 * `exec`          — reserved for future non-global executor; pass NULL for now.
 * `sc`            — sandbox config; pass NULL or a config with enabled==0 to omit
 *                   the sandbox policy block (static section).
 * `budget_tokens` — remaining context-window token budget.  When > 0, a one-line
 *                   hint is appended to the dynamic section so the model can
 *                   adapt its verbosity.  Pass 0 to omit the hint; the rest of
 *                   the prompt is unaffected.
 *
 * Returns a PromptParts struct; either field may be NULL on allocation failure.
 * Both fields are NULL if arena or cwd is NULL.
 */
PromptParts prompt_build_parts(Arena *arena, const char *cwd, void *exec,
                                const SandboxConfig *sc, size_t budget_tokens);

/*
 * prompt_build — build the system prompt as a single concatenated string.
 *
 * Calls prompt_build_parts() with budget_tokens=0 and concatenates the two
 * sections.  Provided for backward compatibility; prefer prompt_build_parts()
 * when the caller needs the static/dynamic split for cache-control.
 *
 * Returns an arena-allocated NUL-terminated string, or NULL on failure.
 */
char *prompt_build(Arena *arena, const char *cwd, void *exec,
                   const SandboxConfig *sc);

#endif /* PROMPT_H */
