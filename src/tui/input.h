/*
 * input.h — terminal line editor
 *
 * Readline-like UX: raw mode, arrow-key navigation, persistent history,
 * tab completion for /commands, multi-line continuation with '\'.
 *
 * Primary API:
 *   InputLine input_read(Arena *arena, const char *prompt);
 *
 * History management (call from the application layer):
 *   input_history_add(), input_history_save(), input_history_load()
 *
 * The EditBuf struct and its helpers are exposed for unit testing.
 */

#ifndef INPUT_H
#define INPUT_H

#include <stddef.h>
#include "util/arena.h"

/* -------------------------------------------------------------------------
 * Result type
 * ---------------------------------------------------------------------- */

typedef struct {
    char   *text;   /* arena-allocated, NUL-terminated */
    size_t  len;
    int     is_eof; /* 1 when user presses Ctrl+D on an empty line */
} InputLine;

/* -------------------------------------------------------------------------
 * Editing buffer — exposed for unit testing
 * ---------------------------------------------------------------------- */

#define INPUT_LINE_MAX 4096

typedef struct {
    char   buf[INPUT_LINE_MAX]; /* NUL-terminated content */
    size_t len;                 /* bytes in use (not counting NUL) */
    size_t pos;                 /* cursor position: 0..len */
} EditBuf;

/* All edit operations silently clamp when at buffer limits. */
void edit_insert(EditBuf *e, char c);
void edit_insert_str(EditBuf *e, const char *s, size_t n);
void edit_backspace(EditBuf *e);
void edit_delete(EditBuf *e);
void edit_move_left(EditBuf *e);
void edit_move_right(EditBuf *e);
void edit_move_home(EditBuf *e);
void edit_move_end(EditBuf *e);
void edit_reset(EditBuf *e);

/* -------------------------------------------------------------------------
 * Tab completion (exposed for testing)
 * ---------------------------------------------------------------------- */

/*
 * If the buffer starts with '/' and has no space, complete from the
 * known /command list.  On a unique match the command is filled in;
 * on multiple matches the longest common prefix is applied.
 */
void input_tab_complete(EditBuf *e);

/* -------------------------------------------------------------------------
 * Main API
 * ---------------------------------------------------------------------- */

/*
 * Read one logical line from stdin with raw-mode editing.
 *
 *  - Returns is_eof=1 when Ctrl+D is pressed on an empty line.
 *  - Lines ending with '\' are joined with the next line (multi-line mode).
 *  - Falls back to fgets(3) when stdin is not a tty.
 *  - text is arena-allocated and NUL-terminated; len excludes the NUL.
 */
InputLine input_read(Arena *arena, const char *prompt);

/* Add line to the in-memory history ring (empty lines and consecutive
 * duplicates are silently dropped). */
void input_history_add(const char *line);

/* Write history to path, one entry per line, oldest first. */
void input_history_save(const char *path);

/* Load history from path, replacing the current in-memory ring. */
void input_history_load(const char *path);

/* Reset all history state (used by tests). */
void input_history_reset(void);

/* -------------------------------------------------------------------------
 * Event-driven input — integrate stdin with the event loop
 * ---------------------------------------------------------------------- */

/*
 * Callback fired by input_on_readable when a complete logical line is ready.
 * `line.is_eof` is 1 when the user presses Ctrl+D on an empty line or when
 * stdin reaches EOF.  After an EOF callback the fd has been removed from raw
 * mode and must not be re-registered with the loop.
 */
typedef void (*input_line_cb)(InputLine line, void *userdata);

/* Opaque context for event-driven line editing. */
typedef struct InputCtx InputCtx;

/*
 * Allocate and initialise an InputCtx.  If stdin is a tty, enables raw mode
 * and writes the initial prompt to stdout.
 * `on_line` is called each time a complete logical line (or EOF) is received.
 * Returns NULL on allocation failure.
 *
 * The `prompt` string must remain valid for the lifetime of the context.
 */
InputCtx *input_ctx_new(Arena *arena, const char *prompt,
                        input_line_cb on_line, void *userdata);

/*
 * Free the context and restore the terminal to cooked mode if raw mode was
 * enabled.  Safe to call with NULL.
 */
void input_ctx_free(InputCtx *ctx);

/*
 * Prepare the context for the next input line.  Updates the prompt and arena,
 * then writes the new prompt to stdout when the terminal is in raw mode.
 *
 * Call this after `on_line` fires, once the application is ready to accept
 * the next line (e.g. after the AI response has finished streaming).
 * The `prompt` string must remain valid until the next call to input_ctx_reset
 * or input_ctx_free.
 */
void input_ctx_reset(InputCtx *ctx, Arena *arena, const char *prompt);

/*
 * loop_cb-compatible callback for stdin readability.  Register once with:
 *
 *   loop_add_fd(loop, STDIN_FILENO, LOOP_READ, input_on_readable, ctx);
 *
 * Drains all immediately available bytes from fd (required for edge-triggered
 * epoll).  Returns 0 to remain in the loop, -1 on EOF (the loop removes the
 * fd automatically).
 */
int input_on_readable(int fd, int events, void *ctx);

#endif /* INPUT_H */
