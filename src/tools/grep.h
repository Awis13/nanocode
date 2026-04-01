/*
 * grep.h — grep tool: recursive content search, POSIX regex.h, context lines
 *
 * Call grep_register() at startup to register the "grep" tool with the
 * executor. The handler is exported for unit testing.
 */

#ifndef GREP_H
#define GREP_H

#include "executor.h"

/* Register the grep tool with the executor. */
void grep_register(void);

/*
 * grep handler — exported for direct testing without the executor registry.
 *
 * args: {
 *   "pattern":     "regex"           required — POSIX extended regex
 *   "path":        "."               optional — base directory (default ".")
 *   "glob":        "*.c"             optional — filename filter (default: all)
 *   "output_mode": "files_with_matches" optional — "content" | "files_with_matches" | "count"
 *   "-A":          0                 optional — lines of after-context
 *   "-B":          0                 optional — lines of before-context
 *   "-C":          0                 optional — lines of before+after context
 *   "-i":          false             optional — case-insensitive
 *   "head_limit":  250               optional — max output lines (0 = unlimited)
 * }
 */
ToolResult grep_search(Arena *arena, const char *args_json);

#endif /* GREP_H */
