/*
 * statusbar.h — one-line status bar at the bottom of the terminal
 *
 * Displays model name, token counts, estimated cost, and turn counter.
 * Example output (right-aligned, dimmed):
 *
 *   [claude-opus-4-6]  in: 12,450  out: 892  ~$0.08  turn 7/∞
 *
 * Uses ANSI save-cursor / move-to-last-row / restore to update without
 * scrolling. All output is emitted in a single write(2) per update to
 * minimise flicker.
 */

#ifndef STATUSBAR_H
#define STATUSBAR_H

#include "../api/provider.h"

typedef struct StatusBar StatusBar;

/*
 * Create a status bar writing to file descriptor `fd`.
 * `cfg`       — provider config; used for model name and cost rates.
 * `max_turns` — denominator in "turn N/M"; pass 0 to display ∞.
 *
 * Returns NULL on allocation failure.
 */
StatusBar *statusbar_new(int fd, const ProviderConfig *cfg, int max_turns);

/*
 * Redraw the status bar with updated counters.
 * `in_tok`  — cumulative input tokens for the session.
 * `out_tok` — cumulative output tokens for the session.
 * `turn`    — current turn number (1-based).
 *
 * Call after each token batch, not per-token.
 */
void statusbar_update(StatusBar *sb, int in_tok, int out_tok, int turn);

/*
 * Erase the status bar line, leaving the terminal clean.
 * Call on exit before the process terminates.
 */
void statusbar_clear(StatusBar *sb);

/*
 * Free the StatusBar struct. Does not close `fd`.
 */
void statusbar_free(StatusBar *sb);

/*
 * Format the visible status line (no ANSI escapes) into `buf[cap]`.
 * Returns number of bytes written (not including NUL).
 * Exposed for unit tests; call statusbar_update for normal use.
 */
int statusbar_format_line(const StatusBar *sb, int in_tok, int out_tok,
                          int turn, char *buf, int cap);

#endif /* STATUSBAR_H */
