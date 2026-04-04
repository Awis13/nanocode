/*
 * commands.h — slash-command parser and dispatcher
 *
 * Detects leading '/' on user input, tokenizes, and dispatches to
 * a static function-pointer registry of named handlers.
 *
 * Usage:
 *   if (cmd_is_command(line)) {
 *       CmdContext ctx = { .conv = conv, .sandbox = sb, ... };
 *       cmd_dispatch(line, &ctx);
 *   }
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include "../agent/conversation.h"
#include "../tools/diff_sandbox.h"

/* Forward declaration — full type defined in history.h. */
typedef struct HistoryCtx HistoryCtx;

/*
 * Context passed to every command handler.
 * All pointer fields are optional — handlers NULL-check before use.
 */
typedef struct {
    Conversation  *conv;        /* /clear, /compact   */
    DiffSandbox   *sandbox;     /* /diff, /apply, /reject */
    char         **model;       /* pointer to current model name; /model updates *model */
    int            in_tok;      /* cumulative input  tokens this session, for /cost */
    int            out_tok;     /* cumulative output tokens this session, for /cost */
    const char   **mcp_names;   /* MCP server name strings, for /mcp */
    int            nmcp;        /* number of MCP servers */
    int            fd_out;      /* output fd (0 → STDOUT_FILENO) */
    int            fd_in;       /* input  fd (0 → STDIN_FILENO)  */
    HistoryCtx    *history;     /* open history file; /resume loads, /export uses */
} CmdContext;

/*
 * Return 1 if `input` starts with '/', 0 otherwise.
 * NULL input returns 0.
 */
int cmd_is_command(const char *input);

/*
 * Parse and dispatch a slash command.
 *
 * `input` — NUL-terminated user line, e.g. "/model claude-opus-4-6"
 * `ctx`   — caller-provided context (may be NULL; handlers NULL-check)
 *
 * Returns:
 *   0   — command recognised and handled
 *   1   — unknown command (error printed to fd_out)
 *  -1   — `input` is not a slash command
 */
int cmd_dispatch(const char *input, CmdContext *ctx);

#endif /* COMMANDS_H */
