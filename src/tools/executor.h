/*
 * executor.h — tool registry and dispatch
 *
 * All allocations for results go through the caller-supplied Arena.
 * The registry itself uses static storage (no heap).
 */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../util/arena.h"
#include "../../include/status_file.h"
#include <stddef.h>

/* Hard cap on number of registered tools. */
#define TOOL_REGISTRY_MAX 64

/*
 * Execution mode — controls which tools are permitted to run.
 *
 * EXEC_MODE_NORMAL   : all registered tools may be invoked.
 *
 * EXEC_MODE_PLAN     : write/execute tools (bash, write_file, edit_file) are
 *                      blocked; the model may only read and reason.
 *                      Set by the /plan slash command (agent-initiated).
 *                      Error message includes a hint to toggle plan mode off.
 *
 * EXEC_MODE_DRY_RUN  : no tool is actually executed; every call returns a
 *                      synthetic {"dry_run":true} result and a log line is
 *                      emitted to stderr.
 *                      Set by --dry-run flag or session.mode = "dry-run".
 *
 * EXEC_MODE_READONLY : write/execute tools (bash, write_file, edit_file) are
 *                      blocked with an error; read tools run normally.
 *                      Set by --readonly flag or session.mode = "readonly".
 *                      Use when you want to allow reads but prevent mutations.
 *
 * EXEC_MODE_PLAN vs EXEC_MODE_READONLY: both block the same tool set but
 * serve different contexts. PLAN is agent-toggled mid-session for structured
 * reasoning; READONLY is a user-imposed constraint at startup. Their error
 * messages differ to guide the user appropriately in each context.
 */
typedef enum {
    EXEC_MODE_NORMAL   = 0,
    EXEC_MODE_PLAN     = 1,
    EXEC_MODE_DRY_RUN  = 2,
    EXEC_MODE_READONLY = 3
} ExecMode;

void     executor_set_mode(ExecMode mode);
ExecMode executor_get_mode(void);

/*
 * Safety class for a registered tool.
 * TOOL_SAFE_READONLY : tool does not mutate state; safe to block in readonly mode.
 * TOOL_SAFE_MUTATING : tool may mutate files or state; blocked in readonly mode.
 */
typedef enum {
    TOOL_SAFE_READONLY = 0,
    TOOL_SAFE_MUTATING = 1
} ToolSafety;

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
void tool_register(const char *name, const char *schema_json, ToolHandler fn,
                   ToolSafety safety);
ToolSafety tool_get_safety(const char *name);
ToolResult tool_invoke_noside(Arena *arena, const char *name,
                              const char *args_json);

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
 * Register the tool_search meta-tool.
 * Call once at startup (after all other tools are registered) so the model
 * can request full schemas on demand.  tool_search itself always carries its
 * full schema and is excluded from the name-stub list returned by
 * tool_names_json().
 */
void tool_search_register(void);

/*
 * Return a JSON array of name-only stubs for every registered tool except
 * tool_search itself:
 *   [{"name":"bash"},{"name":"grep"},...]
 * Arena-allocated NUL-terminated string.  Returns NULL on allocation failure.
 */
char *tool_names_json(Arena *arena);

/*
 * Return the full tool-schemas JSON array for every registered tool
 * (including tool_search):
 *   [<schema1>,<schema2>,...]
 * Arena-allocated NUL-terminated string.  Returns NULL on allocation failure.
 */
char *tool_schemas_json(Arena *arena);

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

/*
 * Attach a status tracker: after every tool invocation executor updates
 * info->last_action and info->tool_calls and calls status_file_write.
 * `info` must be a pointer to a StatusInfo (from status_file.h).
 * Pass NULL path to disable.
 */
void executor_set_status_tracker(const char *path, StatusInfo *info);

/* -------------------------------------------------------------------------
 * Tool event hook — observer callback fired before/after each invocation.
 *
 * TOOL_EVENT_START : tool is about to run (may be blocked by mode)
 * TOOL_EVENT_DONE  : tool completed without error
 * TOOL_EVENT_ERROR : tool completed with error, or was blocked
 *
 * Register with executor_set_tool_event_cb(). Pass cb=NULL to remove.
 * `ctx` is passed through to the callback unchanged.
 * ---------------------------------------------------------------------- */
typedef enum {
    TOOL_EVENT_START = 0,
    TOOL_EVENT_DONE  = 1,
    TOOL_EVENT_ERROR = 2
} ToolEvent;

typedef void (*tool_event_cb)(ToolEvent ev, void *ctx);

void executor_set_tool_event_cb(tool_event_cb cb, void *ctx);

#endif /* EXECUTOR_H */
