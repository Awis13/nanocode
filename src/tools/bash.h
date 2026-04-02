/*
 * bash.h — bash tool: fork/exec with pipe capture and OS-native sandboxing
 *
 * Call bash_tool_register() at startup to make the "bash" tool available
 * via tool_invoke().
 */

#ifndef BASH_H
#define BASH_H

/* Maximum bytes captured from a single command invocation. */
#define BASH_OUTPUT_MAX (256 * 1024)

/* Default command timeout when none is specified in args. */
#define BASH_DEFAULT_TIMEOUT_SECS 30

/*
 * Register the "bash" tool in the executor registry.
 * Must be called before any tool_invoke("bash", ...) call.
 * Aborts if the registry is full (see TOOL_REGISTRY_MAX in executor.h).
 */
void bash_tool_register(void);

/*
 * Configure command allow/deny filters.  Call once from main() after
 * config is loaded.
 *
 *   allowed_colon_sep  — colon-separated basenames that may run ("" = any)
 *   denied_colon_sep   — colon-separated basenames that are always blocked
 *
 * Denylist is checked first; allowlist is checked second.
 * Empty string means "no restriction for that list."
 */
void bash_set_cmd_filter(const char *allowed_colon_sep,
                         const char *denied_colon_sep);

#endif /* BASH_H */
