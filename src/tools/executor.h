/*
 * executor.h — tool registry and dispatch
 *
 * All allocations for results go through the caller-supplied Arena.
 * The registry itself uses static storage (no heap).
 */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../util/arena.h"
#include <stddef.h>

/* Hard cap on number of registered tools. */
#define TOOL_REGISTRY_MAX 64

/* Result returned by every tool handler and by tool_invoke(). */
typedef struct {
    int     error;    /* 0 = success, non-zero = failure */
    char   *content;  /* arena-allocated JSON or plain text */
    size_t  len;      /* strlen(content) */
} ToolResult;

/* Signature every tool handler must implement. */
typedef ToolResult (*ToolHandler)(Arena *arena, const char *args_json);

/*
 * Register a tool.
 * `name` and `schema_json` are NOT copied — caller must keep them alive.
 * Aborts if TOOL_REGISTRY_MAX is exceeded.
 */
void tool_register(const char *name, const char *schema_json, ToolHandler fn);

/*
 * Dispatch a tool by name.
 * Returns ToolResult with error=1 and a descriptive message for unknown names.
 * All result memory is arena-allocated.
 */
ToolResult tool_invoke(Arena *arena, const char *name, const char *args_json);

/*
 * Reset the registry — useful in tests to start from a clean state.
 */
void tool_registry_reset(void);

/*
 * Fill `names` with pointers to the names of registered tools (up to max_names).
 * Returns the total number of registered tools (may exceed max_names).
 * Pointers are into static storage — do not free.
 */
int tool_list_names(const char **names, int max_names);

/*
 * Serialize a ToolResult into a Claude-compatible tool_result block:
 *   {"type":"tool_result","tool_use_id":"<id>","content":"<esc>"}
 * If result->error is non-zero, "is_error":true is included.
 * Returns an arena-allocated NUL-terminated JSON string, or NULL on failure.
 */
char *tool_result_to_json(Arena *arena, const char *tool_use_id,
                          const ToolResult *result);

#endif /* EXECUTOR_H */
