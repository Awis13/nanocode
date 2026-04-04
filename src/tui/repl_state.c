/*
 * repl_state.c — REPL state machine: pet animations + loop mode transitions
 */

#include "repl_state.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void set_pet(ReplCtx *ctx, PetState ps)
{
    if (ctx->pet)
        pet_transition(ctx->pet, ps);
}

static void set_loop_mode(ReplCtx *ctx, LoopMode mode)
{
    if (ctx->loop)
        loop_set_mode(ctx->loop, mode);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void repl_ctx_init(ReplCtx *ctx, Loop *loop, Pet *pet,
                   StatusBar *statusbar, Spinner *spinner)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state     = REPL_PROMPT;
    ctx->loop      = loop;
    ctx->pet       = pet;
    ctx->statusbar = statusbar;
    ctx->spinner   = spinner;
}

void repl_transition(ReplCtx *ctx, ReplState new_state)
{
    ReplState old = ctx->state;
    ctx->state = new_state;

    switch (new_state) {

    case REPL_PROMPT:
        /* User is typing — idle animations, relaxed poll. */
        set_pet(ctx, PET_IDLE);
        set_loop_mode(ctx, LOOP_IDLE);
        if (ctx->spinner)
            spinner_stop(ctx->spinner);
        break;

    case REPL_THINKING:
        /* Request dispatched — spinner appears, pet goes active, tight poll. */
        set_pet(ctx, PET_ACTIVE);
        set_loop_mode(ctx, LOOP_STREAMING);
        if (ctx->statusbar)
            statusbar_mark_turn_start(ctx->statusbar);
        if (ctx->spinner) {
            const char *term = getenv("TERM");
            int color = (term && strstr(term, "256color")) ? 1 : 0;
            spinner_start(ctx->spinner, 1 /* stdout */, color);
        }
        break;

    case REPL_STREAMING:
        /* First tokens arriving — stop spinner, mark latency. */
        if (old != REPL_STREAMING) {
            set_pet(ctx, PET_ACTIVE);
            set_loop_mode(ctx, LOOP_STREAMING);
            if (ctx->spinner)
                spinner_stop(ctx->spinner);
        }
        if (ctx->statusbar)
            statusbar_mark_first_token(ctx->statusbar);
        break;

    case REPL_TOOL_EXEC:
        /* Tool running — pet stays active (working). */
        set_pet(ctx, PET_ACTIVE);
        set_loop_mode(ctx, LOOP_STREAMING);
        if (ctx->spinner)
            spinner_stop(ctx->spinner);
        break;

    case REPL_DONE:
        /* Turn complete — celebrate briefly, then relax. */
        set_pet(ctx, PET_DONE);
        set_loop_mode(ctx, LOOP_IDLE);
        if (ctx->spinner)
            spinner_stop(ctx->spinner);
        break;
    }
}

void repl_tick(ReplCtx *ctx)
{
    /* Advance the spinner while waiting for first token. */
    if (ctx->state == REPL_THINKING && ctx->spinner)
        spinner_tick(ctx->spinner);
}

ReplState repl_get_state(const ReplCtx *ctx)
{
    return ctx->state;
}
