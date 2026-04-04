/*
 * tool_display.h — terminal display of tool invocations and results
 *
 * Renders tool invocation headers, result blocks (truncated to
 * TOOL_DISPLAY_MAX_LINES lines), unified diff with ANSI coloring,
 * and error messages.  All output goes to the supplied file descriptor
 * via a single write(2) per call.
 */

#ifndef TOOL_DISPLAY_H
#define TOOL_DISPLAY_H

#include "../tools/executor.h"
#include "spinner.h"

/* Lines of output shown before the "[N more lines]" footer. */
#define TOOL_DISPLAY_MAX_LINES 5

/* Maximum visible column width for tool output blocks. */
#define TOOL_DISPLAY_WIDTH 80

/*
 * Emit a dimmed invocation header:
 *   > tool: args_summary
 *
 * Both tool and args_summary are truncated so the total visible line
 * width does not exceed TOOL_DISPLAY_WIDTH.  NULL arguments are treated
 * as empty strings.
 */
void tool_display_invocation(int fd, const char *tool,
                             const char *args_summary);

/*
 * Display a tool result block.
 *
 * - Content is indented 2 spaces and word-wrapped at TOOL_DISPLAY_WIDTH.
 * - At most TOOL_DISPLAY_MAX_LINES lines are shown; if more exist, a
 *   dimmed footer "[N more lines — press T to expand]" is appended.
 * - If r->error is non-zero, delegates to tool_display_error().
 * - NULL or empty content emits an "(empty)" placeholder.
 */
void tool_display_result(int fd, const ToolResult *r);

/*
 * Render a unified diff with ANSI colours:
 *   '+' lines  → green
 *   '-' lines  → red
 *   '@' lines  → cyan   (hunk header)
 *   other      → dim    (context / file header)
 *
 * NULL diff_text is a no-op.
 */
void tool_display_diff(int fd, const char *diff_text);

/*
 * Emit a red error line:
 *   ! tool error: msg
 *
 * NULL msg emits an empty error line.
 */
void tool_display_error(int fd, const char *msg);

/*
 * Progress spinner — braille breathing spinner shown while a tool runs.
 * Wraps Spinner; all state is stored inline with no heap allocation.
 */
typedef struct {
    Spinner sp;   /* braille spinner — delegates to spinner_*() */
} ToolProgress;

/* Initialise *p and record the start time. Does not write to fd yet. */
void tool_progress_start(ToolProgress *p, int fd);

/*
 * Advance the spinner.  Writes a one-character spinner to fd only after
 * 500 ms have elapsed; subsequent calls update the glyph in-place using
 * a carriage-return.  Safe to call in a tight poll loop.
 */
void tool_progress_tick(ToolProgress *p);

/*
 * Erase the spinner glyph (if shown) and reset *p.
 * Call before tool_display_result() so the spinner does not appear in
 * the final output.
 */
void tool_progress_stop(ToolProgress *p);

#endif /* TOOL_DISPLAY_H */
