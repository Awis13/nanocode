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

#endif /* INPUT_H */
