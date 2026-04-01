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

#endif /* BASH_H */
