/*
 * memory.h — cross-session memory tool
 *
 * Provides the memory_write tool that lets the model persist notes across
 * sessions in ~/.nanocode/memory.md.
 *
 * File format:
 *   Markdown with named sections:  ## <key>\n<content>\n
 *   Sections are appended or replaced.  When the total line count exceeds
 *   MEMORY_MAX_LINES the oldest sections (top of file) are trimmed first.
 */

#ifndef MEMORY_H
#define MEMORY_H

#include "../util/arena.h"

/* Line cap for ~/.nanocode/memory.md — oldest sections trimmed on overflow. */
#define MEMORY_MAX_LINES 200

/*
 * Register the memory_write built-in tool.
 * Call once at startup after other tools are registered.
 */
void memory_tool_register(void);

/*
 * Read ~/.nanocode/memory.md and return its contents as an
 * arena-allocated NUL-terminated string.
 * Returns NULL if the file does not exist or is empty.
 */
char *memory_load(Arena *arena);

#endif /* MEMORY_H */
