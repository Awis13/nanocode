/*
 * gui_coordinator.c — wires provider streaming into the GUI (CMP-413)
 *
 * Mirrors the role of repl_coordinator for TUI sessions, but routes token
 * output to gui_post_output() and accepts input from the GUI text field.
 *
 * Design:
 *   - Single static GuiSession holds all session state.
 *   - gui_on_input() is registered as the gui_input_cb; it appends the user
 *     message to the Conversation and starts a provider_stream() call.
 *   - on_token / on_done callbacks run on the main thread (called from
 *     loop_step() which the NSTimer drives from within [NSApp run]).
 *   - Tool calls are echoed to the output area as plain text; full tool
 *     execution is Phase 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui_coordinator.h"
#include "../../include/gui.h"
#include "../agent/conversation.h"
#include "../agent/prompt.h"
#include "../api/provider.h"
#include "../core/loop.h"
#include "../util/arena.h"
#include "../util/buf.h"

/* -------------------------------------------------------------------------
 * Session state
 * ---------------------------------------------------------------------- */

typedef struct {
    GuiWindow            *win;
    Loop                 *loop;
    Provider             *provider;
    Conversation         *conv;
    Arena                *arena;
    const ProviderConfig *pcfg;
    Buf                   resp_buf;   /* accumulates assistant response text */
    int                   streaming;  /* 1 while a stream is in flight */
} GuiSession;

static GuiSession g_gs;

/* -------------------------------------------------------------------------
 * Streaming callbacks
 * ---------------------------------------------------------------------- */

static void
on_token(const char *token, size_t len, void *ctx)
{
    GuiSession *gs = ctx;
    gui_post_output(gs->win, token, len);
    buf_append(&gs->resp_buf, token, len);
}

static void
on_tool(const char *id, const char *name, const char *input, void *ctx)
{
    /* Phase 1: echo tool call as plain text; no execution. */
    GuiSession *gs = ctx;
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "\n[tool: %s(%s)]\n", name, id);
    gui_post_output(gs->win, hdr, strlen(hdr));
    (void)input;
}

static void
on_done(int error, const char *stop_reason, void *ctx)
{
    (void)stop_reason;
    GuiSession *gs = ctx;
    gs->streaming = 0;

    if (error) {
        const char *msg = "\n[error: provider stream failed]\n";
        gui_post_output(gs->win, msg, strlen(msg));
    } else {
        /* Commit the accumulated assistant reply to the conversation. */
        const char *reply = buf_str(&gs->resp_buf);
        if (reply && gs->resp_buf.len > 0)
            conv_add(gs->conv, "assistant", reply);

        /* Append a blank line as turn separator. */
        gui_post_output(gs->win, "\n", 1);
    }

    buf_reset(&gs->resp_buf);

    /* Update title with latest token totals. */
    /* Token counting is not exposed by the provider in Phase 1;
     * we track output characters as a rough proxy. */
    gui_update_title(gs->win, gs->pcfg->model, 0, 0);
}

/* -------------------------------------------------------------------------
 * Input callback — called when user presses Enter or Send
 * ---------------------------------------------------------------------- */

static void
gui_on_input(const char *text, size_t len, void *ctx)
{
    GuiSession *gs = ctx;
    if (gs->streaming) {
        /* Ignore input while a stream is already in flight. */
        return;
    }

    /* Echo the user's message in the output area. */
    {
        const char *you = "You: ";
        gui_post_output(gs->win, you, strlen(you));
        gui_post_output(gs->win, text, len);
        gui_post_output(gs->win, "\n\nnanocode: ", 11);
    }

    /* Append user turn to the conversation. */
    conv_add(gs->conv, "user", text);

    /* Build message array and start streaming. */
    int nmsg = 0;
    Message *msgs = conv_to_messages(gs->conv, &nmsg, gs->arena);
    if (!msgs || nmsg == 0) {
        const char *err = "\n[internal error: failed to build messages]\n";
        gui_post_output(gs->win, err, strlen(err));
        return;
    }

    buf_reset(&gs->resp_buf);
    gs->streaming = 1;

    if (provider_stream(gs->provider, msgs, nmsg,
                        on_token, on_tool, on_done, gs) != 0) {
        gs->streaming = 0;
        const char *err = "\n[error: failed to start stream]\n";
        gui_post_output(gs->win, err, strlen(err));
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void
gui_coordinator_setup(GuiWindow *win,
                      Loop *loop,
                      const ProviderConfig *pcfg,
                      const SandboxConfig  *sc,
                      Arena *arena)
{
    memset(&g_gs, 0, sizeof(g_gs));
    g_gs.win   = win;
    g_gs.loop  = loop;
    g_gs.pcfg  = pcfg;
    g_gs.arena = arena;
    buf_init(&g_gs.resp_buf);

    /* Build system prompt. */
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0';

    const char *sysprompt = prompt_build(arena, cwd[0] ? cwd : ".", NULL, sc, 0);
    if (!sysprompt)
        sysprompt = "You are a helpful AI coding assistant.";

    /* Create conversation and seed system prompt. */
    g_gs.conv = conv_new(arena);
    if (g_gs.conv)
        conv_add(g_gs.conv, "system", sysprompt);

    /* Create provider. */
    g_gs.provider = provider_new(loop, pcfg);

    /* Wire input callback. */
    gui_set_input_callback(win, gui_on_input, &g_gs);

    /* Initial window title. */
    gui_update_title(win, pcfg->model, 0, 0);
}

void
gui_coordinator_teardown(void)
{
    if (g_gs.provider) {
        provider_free(g_gs.provider);
        g_gs.provider = NULL;
    }
    buf_destroy(&g_gs.resp_buf);
    memset(&g_gs, 0, sizeof(g_gs));
}
