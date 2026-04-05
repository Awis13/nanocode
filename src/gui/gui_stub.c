/*
 * gui_stub.c — GUI stub for platforms where AppKit / Wayland is not available.
 *
 * gui_init() prints an error and returns NULL so main() can exit cleanly.
 * All other functions are no-ops.
 */

#ifndef __APPLE__

#include <stdio.h>
#include "../../include/gui.h"

GuiWindow *
gui_init(Loop *loop, Arena *arena)
{
    (void)loop; (void)arena;
    fprintf(stderr, "nanocode: --gui is not supported on this platform\n");
    return NULL;
}

void gui_run(GuiWindow *win)               { (void)win; }
void gui_post_output(GuiWindow *win,
                     const char *text,
                     size_t len)           { (void)win; (void)text; (void)len; }
void gui_set_input_callback(GuiWindow *win,
                             gui_input_cb cb,
                             void *ctx)    { (void)win; (void)cb; (void)ctx; }
void gui_update_title(GuiWindow *win,
                      const char *model,
                      int in_tok,
                      int out_tok)         { (void)win; (void)model;
                                             (void)in_tok; (void)out_tok; }
void gui_destroy(GuiWindow *win)           { (void)win; }

#endif /* !__APPLE__ */
