/*
 * fileops.h — file operation tools: read_file, write_file, edit_file, glob
 *
 * Call fileops_register_all() at startup to register all four tools
 * with the executor. Individual handlers are exported for unit testing.
 */

#ifndef FILEOPS_H
#define FILEOPS_H

#include "executor.h"

/* Register read_file, write_file, edit_file, and glob with the executor. */
void fileops_register_all(void);

/*
 * Configure per-session resource limits.  Call once from main() after
 * config is loaded.  Resets the session file counter to zero.
 *
 *   max_file_size_bytes  — maximum bytes per write/edit (0 = unlimited)
 *   max_files_created    — maximum new files per session (0 = unlimited)
 */
void fileops_set_limits(long max_file_size_bytes, int max_files_created);

/*
 * Individual tool handlers — exported so tests can invoke them directly
 * without going through the registry.
 *
 * read_file  args: {"path":"...", "offset":N, "limit":N}
 *   offset/limit are optional 0-indexed line numbers.
 *   Without range, returns up to 50 KB.
 *
 * write_file args: {"path":"...", "content":"..."}
 *   Creates parent directories if needed (mkdir -p semantics).
 *
 * edit_file  args: {"path":"...", "old_string":"...", "new_string":"...",
 *                   "replace_all":false}
 *   Exact-match replacement. Fails if old_string not found.
 *   Fails with ambiguity error if found >1 time and replace_all is false.
 *
 * glob       args: {"pattern":"GLOB", "path":"./"}
 *   Supports ** for recursive matching. Returns newline-separated paths.
 */
ToolResult fileops_read(Arena *arena, const char *args_json);
ToolResult fileops_write(Arena *arena, const char *args_json);
ToolResult fileops_edit(Arena *arena, const char *args_json);
ToolResult fileops_glob(Arena *arena, const char *args_json);

#endif /* FILEOPS_H */
