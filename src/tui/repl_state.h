/*
 * repl_state.h — REPL state machine driving pet animations and loop mode
 *
 * Defines a clean state machine for the interactive session lifecycle:
 *
 *   REPL_PROMPT     — waiting for user input
 *   REPL_THINKING   — request dispatched, waiting for first token
 *   REPL_STREAMING  — tokens arriving from the provider
 *   REPL_TOOL_EXEC  — executing a tool call
 *   REPL_DONE       — turn complete
 *
 * State transitions drive:
 *   - Pet animations (IDLE / ACTIVE / DONE)
 *   - Loop mode     (LOOP_IDLE at 100 ms / LOOP_STREAMING at 16 ms)
 *   - StatusBar timing markers (turn start, first token)
 *   - Spinner visibility (braille wait glyph during REPL_THINKING)
 *
 * No heap allocations — ReplCtx is stack / caller-allocated.
 */

#ifndef REPL_STATE_H
#define REPL_STATE_H

#include "../core/loop.h"
#include "pet.h"
#include "spinner.h"
#include "statusbar.h"

/* -------------------------------------------------------------------------
 * States
 * ---------------------------------------------------------------------- */

typedef enum {
    REPL_PROMPT    = 0,  /* idle, waiting for user input */
    REPL_THINKING  = 1,  /* API request in flight, no tokens yet */
    REPL_STREAMING = 2,  /* streaming tokens from provider */
    REPL_TOOL_EXEC = 3,  /* executing a tool use block */
    REPL_DONE      = 4   /* turn complete, before next prompt */
} ReplState;

/* -------------------------------------------------------------------------
 * Context
 * ---------------------------------------------------------------------- */

typedef struct {
    ReplState  state;
    Loop      *loop;       /* event loop — for loop_set_mode() */
    Pet       *pet;        /* pet state machine — may be NULL */
    StatusBar *statusbar;  /* status bar — may be NULL */
    Spinner   *spinner;    /* braille wait spinner — may be NULL */
} ReplCtx;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/*
 * Initialise a ReplCtx.  All pointer fields may be NULL (safe no-ops).
 * Initial state is REPL_PROMPT.
 */
void repl_ctx_init(ReplCtx *ctx, Loop *loop, Pet *pet,
                   StatusBar *statusbar, Spinner *spinner);

/*
 * Transition to `new_state`.  Side effects:
 *
 *   REPL_PROMPT    → pet IDLE; loop LOOP_IDLE; spinner stop
 *   REPL_THINKING  → pet ACTIVE; loop LOOP_STREAMING;
 *                    calls statusbar_mark_turn_start(); spinner start
 *   REPL_STREAMING → pet ACTIVE (stays); loop LOOP_STREAMING;
 *                    calls statusbar_mark_first_token(); spinner stop
 *   REPL_TOOL_EXEC → pet ACTIVE; loop LOOP_STREAMING; spinner stop
 *   REPL_DONE      → pet DONE; loop LOOP_IDLE; spinner stop
 *
 * Calling with the same state is safe (idempotent for pet/loop, but
 * REPL_STREAMING still marks the first token once).
 */
void repl_transition(ReplCtx *ctx, ReplState new_state);

/*
 * Tick the spinner (if in REPL_THINKING state) — call from the animation
 * timer callback so the braille glyph animates at ~60 fps.
 */
void repl_tick(ReplCtx *ctx);

/*
 * Return the current state.
 */
ReplState repl_get_state(const ReplCtx *ctx);

#endif /* REPL_STATE_H */
