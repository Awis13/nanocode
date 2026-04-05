/*
 * gui_coordinator.h — wires provider streaming into the GUI window (CMP-413)
 *
 * Provides the same role as repl_coordinator for the TUI, but routes
 * tokens to gui_post_output() instead of the terminal renderer.
 */

#ifndef GUI_COORDINATOR_H
#define GUI_COORDINATOR_H

#include "../../include/gui.h"
#include "../api/provider.h"
#include "../core/loop.h"
#include "../util/arena.h"
#include "../../include/sandbox.h"

/*
 * Initialise a GUI session: builds the system prompt, creates a Conversation
 * and Provider, and registers the input callback on `win` so that user
 * submissions trigger provider streaming turns.
 *
 * Must be called after gui_init() and before gui_run().
 */
void gui_coordinator_setup(GuiWindow *win,
                            Loop *loop,
                            const ProviderConfig *pcfg,
                            const SandboxConfig  *sc,
                            Arena *arena);

/*
 * Release provider and conversation resources.
 * Call after gui_run() returns.
 */
void gui_coordinator_teardown(void);

#endif /* GUI_COORDINATOR_H */
