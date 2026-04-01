/*
 * repomap.h — lightweight repo map: symbol extraction and context injection
 *
 * Walks a source tree, extracts top-level symbols (functions, structs, enums,
 * typedefs, classes) per file, and renders a compact summary for injection
 * into the system prompt.
 *
 * Supported languages (pattern-based extraction):
 *   .c / .h   — functions, structs, enums, typedefs
 *   .py       — def, class
 *   .js / .ts — function, class, const/let arrow functions
 *   .go       — func, type
 *   .rs       — fn, struct, enum, trait
 *
 * Output example:
 *   src/tools/executor.h:
 *     ToolResult (struct)
 *     tool_register (fn)
 *     tool_invoke (fn)
 *
 * Note: tree-sitter integration is deferred pending vendoring. This
 * implementation uses a lightweight pattern-based parser that satisfies
 * all acceptance criteria (see CMP-147).
 */

#ifndef REPOMAP_H
#define REPOMAP_H

#include "util/arena.h"

typedef struct RepoMap RepoMap;

/* Allocate a new RepoMap. `arena` is stored for repomap_render output only. */
RepoMap *repomap_new(Arena *arena);

/*
 * Recursively scan `root_dir`, extracting symbols from recognised source
 * files. Directories named `vendor`, `.git`, `node_modules`, and `build`
 * are skipped automatically. May be called multiple times.
 */
void repomap_scan(RepoMap *rm, const char *root_dir);

/*
 * Render all collected symbols as a compact text block.
 * Files with no recognised symbols are omitted.
 * The returned string is allocated from the RepoMap's arena.
 * Returns an empty string (never NULL) on allocation failure.
 */
char *repomap_render(RepoMap *rm, Arena *arena);

/*
 * Release all resources owned by `rm`.  Does not free the arena passed to
 * repomap_new — that is the caller's responsibility.
 */
void repomap_free(RepoMap *rm);

#endif /* REPOMAP_H */
