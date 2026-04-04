/*
 * repl_coordinator.c — interactive REPL session coordinator
 *
 * Wires input -> conversation -> provider streaming -> renderer.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "repl_coordinator.h"
#include "agent/conversation.h"
#include "agent/prompt.h"
#include "agent/tool_protocol.h"
#include "api/provider.h"
#include "core/loop.h"
#include "tui/commands.h"
#include "tui/input.h"
#include "tui/repl_state.h"
#include "tui/renderer.h"
#include "util/arena.h"
#include "util/buf.h"

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

typedef struct {
    Loop                 *loop;
    Provider             *provider;
    Conversation         *conv;
    Arena                *arena;
    const ProviderConfig *pcfg;
    Buf                   resp_buf;
    ToolCall             *pending;
    int                   npending;
    int                   pending_cap;
    InputCtx             *input_ctx;
    ReplGlobals           g;
} ReplSession;

static ReplSession g_rs;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static char *rs_strdup(Arena *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static void repl_start_stream(ReplSession *rs);
static void repl_on_done(int error, const char *stop_reason, void *ctx);

/* -------------------------------------------------------------------------
 * Provider callbacks
 * ---------------------------------------------------------------------- */

static void repl_on_token(const char *tok, size_t len, void *ctx)
{
    ReplSession *rs = ctx;
    repl_transition(rs->g.repl_ctx, REPL_STREAMING);
    if (rs->g.renderer && *rs->g.renderer)
        renderer_token(*rs->g.renderer, tok, len);
    buf_append(&rs->resp_buf, tok, len);
    (*rs->g.sb_out_tok)++;
}

static void repl_on_tool(const char *id, const char *name,
                         const char *input, void *ctx)
{
    ReplSession *rs = ctx;
    if (rs->npending >= rs->pending_cap) {
        int new_cap = rs->pending_cap ? rs->pending_cap * 2 : 4;
        ToolCall *t = arena_alloc(rs->arena,
                                  (size_t)new_cap * sizeof(ToolCall));
        if (!t) return;
        if (rs->pending)
            memcpy(t, rs->pending,
                   (size_t)rs->npending * sizeof(ToolCall));
        rs->pending     = t;
        rs->pending_cap = new_cap;
    }
    ToolCall *tc  = &rs->pending[rs->npending++];
    tc->id    = rs_strdup(rs->arena, id);
    tc->name  = rs_strdup(rs->arena, name);
    tc->input = rs_strdup(rs->arena, input);
}

static void repl_on_done(int error, const char *stop_reason, void *ctx)
{
    ReplSession *rs = ctx;

    if (rs->g.renderer && *rs->g.renderer)
        renderer_flush(*rs->g.renderer);

    const char *text = buf_str(&rs->resp_buf);
    if (text && text[0])
        conv_add(rs->conv, "assistant", text);
    buf_reset(&rs->resp_buf);

    if (error) {
        fprintf(stderr, "\n[nanocode] stream error %d\n", error);
        repl_transition(rs->g.repl_ctx, REPL_DONE);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

    if (rs->npending > 0) {
        repl_transition(rs->g.repl_ctx, REPL_TOOL_EXEC);
        Arena *tool_arena = arena_new(256 * 1024);
        if (tool_arena) {
            tool_dispatch_all(rs->pending, rs->npending,
                              rs->conv, tool_arena);
            arena_free(tool_arena);
        }
        rs->pending     = NULL;
        rs->npending    = 0;
        rs->pending_cap = 0;
    }

    int is_end = !stop_reason ||
                 strcmp(stop_reason, "end_turn") == 0 ||
                 strcmp(stop_reason, "stop") == 0;

    if (!is_end) {
        repl_start_stream(rs);
        return;
    }

    repl_transition(rs->g.repl_ctx, REPL_DONE);
    (*rs->g.sb_turn)++;

    if (rs->input_ctx)
        input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
    repl_transition(rs->g.repl_ctx, REPL_PROMPT);
}

/* -------------------------------------------------------------------------
 * Stream initiation
 * ---------------------------------------------------------------------- */

static void repl_start_stream(ReplSession *rs)
{
    repl_transition(rs->g.repl_ctx, REPL_THINKING);

    Arena *msg_arena = arena_new(256 * 1024);
    if (!msg_arena) {
        fprintf(stderr, "[nanocode] OOM building messages\n");
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

    int nmsg = 0;
    Message *msgs = conv_to_messages(rs->conv, &nmsg, msg_arena);
    if (!msgs || nmsg == 0) {
        arena_free(msg_arena);
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

    if (provider_stream(rs->provider, msgs, nmsg,
                        repl_on_token,
                        repl_on_tool,
                        repl_on_done,
                        rs) != 0) {
        fprintf(stderr, "[nanocode] failed to start stream\n");
        arena_free(msg_arena);
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

    arena_free(msg_arena);
}

/* -------------------------------------------------------------------------
 * Input callback
 * ---------------------------------------------------------------------- */

static void repl_line_cb(InputLine line, void *userdata)
{
    ReplSession *rs = userdata;

    if (line.is_eof) {
        loop_stop(rs->loop);
        return;
    }

    if (!line.text || line.len == 0) {
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        return;
    }

    if (cmd_is_command(line.text)) {
        CmdContext cctx;
        memset(&cctx, 0, sizeof(cctx));
        cctx.conv    = rs->conv;
        cctx.in_tok  = *rs->g.sb_in_tok;
        cctx.out_tok = *rs->g.sb_out_tok;
        cmd_dispatch(line.text, &cctx);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        return;
    }

    input_history_add(line.text);
    conv_add(rs->conv, "user", line.text);
    *rs->g.sb_in_tok += (int)line.len;
    repl_start_stream(rs);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void repl_coordinator_setup(Loop *loop,
                             ProviderConfig *provider_cfg,
                             const SandboxConfig *sc,
                             Arena *arena,
                             ReplGlobals *globals)
{
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0';

    PromptParts parts = prompt_build_parts(
        arena, cwd[0] ? cwd : ".", NULL, sc, 0);
    provider_cfg->system_cache_static = parts.static_part;

    memset(&g_rs, 0, sizeof(g_rs));
    g_rs.g = *globals;

    g_rs.conv = conv_new(arena);
    conv_add(g_rs.conv, "system", parts.dynamic_part);

    g_rs.loop     = loop;
    g_rs.arena    = arena;
    g_rs.pcfg     = provider_cfg;
    g_rs.provider = provider_new(loop, provider_cfg);
    buf_init(&g_rs.resp_buf);

    Renderer *rend = renderer_new(STDOUT_FILENO, arena);
    if (globals->renderer)
        *globals->renderer = rend;

    g_rs.input_ctx = input_ctx_new(arena, "You: ", repl_line_cb, &g_rs);
    if (g_rs.input_ctx)
        loop_add_fd(loop, STDIN_FILENO, LOOP_READ,
                    input_on_readable, g_rs.input_ctx);

    repl_transition(globals->repl_ctx, REPL_PROMPT);
}

void repl_coordinator_teardown(void)
{
    if (g_rs.input_ctx) {
        input_ctx_free(g_rs.input_ctx);
        g_rs.input_ctx = NULL;
    }
    if (g_rs.provider) {
        provider_free(g_rs.provider);
        g_rs.provider = NULL;
    }
    buf_destroy(&g_rs.resp_buf);
}
