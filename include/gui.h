/*
 * gui.h — native GUI interface (CMP-413)
 *
 * Platform-specific windowed UI activated by the --gui flag.
 *
 * macOS: NSApplication + NSWindow + NSTextView + NSTextField via ObjC runtime.
 *        No .m files; all ObjC calls go through objc_msgSend.
 * Linux: stub — prints unsupported message and returns NULL.
 *
 * The GUI runs on the main thread.  gui_run() calls [NSApp run] (macOS);
 * an NSTimer fires every 16 ms to drive loop_step() so kqueue events
 * (provider SSE, etc.) are dispatched from within the AppKit run loop.
 */

#ifndef GUI_H
#define GUI_H

#include <stddef.h>

/* Loop is a named struct so a forward typedef works cleanly. */
typedef struct Loop Loop;

/*
 * Arena is a typedef for an anonymous struct; include the header rather
 * than attempting to forward-declare the anonymous inner struct.
 */
#include "../src/util/arena.h"

typedef struct GuiWindow GuiWindow;

/*
 * Callback invoked when the user submits text (Enter or Send button).
 * `text` is NUL-terminated; `len` is strlen(text).
 * `ctx` is the value passed to gui_set_input_callback().
 */
typedef void (*gui_input_cb)(const char *text, size_t len, void *ctx);

/*
 * Initialise the GUI subsystem and create the main window.
 *
 * `loop`  — the main kqueue/epoll loop; gui_run() drives it internally.
 * `arena` — all GuiWindow state is allocated here (no individual malloc/free).
 *
 * Returns a GuiWindow on success, NULL if the platform is unsupported or
 * initialisation fails.  On an unsupported platform an error is printed.
 */
GuiWindow *gui_init(Loop *loop, Arena *arena);

/*
 * Run the GUI event loop.  Blocks until the window is closed.
 *
 * macOS: calls [NSApp run] with an NSTimer that drives loop_step() at ~60 fps.
 * On window close, loop_stop() is called and gui_run() returns.
 */
void gui_run(GuiWindow *win);

/*
 * Append `len` bytes of `text` to the output text view.
 * Safe to call from within loop_step() callbacks (same thread, same stack).
 * `text` need not be NUL-terminated.
 */
void gui_post_output(GuiWindow *win, const char *text, size_t len);

/*
 * Register a callback to receive user input submissions.
 * Called at most once before gui_run(); replaces any previous registration.
 */
void gui_set_input_callback(GuiWindow *win, gui_input_cb cb, void *ctx);

/*
 * Update the window title to show the active model name and token counts.
 * Safe to call at any time while the window is open.
 */
void gui_update_title(GuiWindow *win, const char *model,
                      int in_tokens, int out_tokens);

/*
 * Release GUI resources.  Call after gui_run() returns.
 * Does NOT free the arena; arena_free() is the caller's responsibility.
 */
void gui_destroy(GuiWindow *win);

#endif /* GUI_H */
