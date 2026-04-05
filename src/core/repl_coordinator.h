/*
 * repl_coordinator.h — interactive REPL session coordinator
 *
 * Wires input -> conversation -> provider streaming -> renderer in a single
 * event-driven loop.  Called from main() after all subsystems are initialised.
 */

#ifndef REPL_COORDINATOR_H
#define REPL_COORDINATOR_H

#include "core/loop.h"
#include "api/provider.h"
#include "tui/repl_state.h"
#include "tui/renderer.h"
#include "util/arena.h"
#include "../include/sandbox.h"

/*
 * Context holding main.c globals needed by the coordinator callbacks.
 * Filled in by the caller (main.c) before calling repl_coordinator_setup().
 */
typedef struct {
    Renderer **renderer;   /* pointer to main's g_renderer */
    ReplCtx   *repl_ctx;   /* pointer to main's g_repl_ctx */
    int       *sb_in_tok;  /* pointer to main's g_sb_in_tok */
    int       *sb_out_tok; /* pointer to main's g_sb_out_tok */
    int       *sb_turn;    /* pointer to main's g_sb_turn */
} ReplGlobals;

/*
 * Initialise and register the interactive REPL with the event loop.
 *
 * Builds the system prompt, creates a Conversation, allocates a Provider,
 * wires a Renderer for token output, and registers stdin with the loop.
 *
 * On return the event loop is ready to drive the session; call loop_run()
 * to start the REPL.
 */
void repl_coordinator_setup(Loop *loop,
                             ProviderConfig *provider_cfg,
                             const SandboxConfig *sc,
                             Arena *arena,
                             ReplGlobals *globals);

/*
 * Release REPL resources.  Call after loop_run() returns.
 */
void repl_coordinator_teardown(void);

#endif /* REPL_COORDINATOR_H */
