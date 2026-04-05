/*
 * repl_coordinator.c -- interactive REPL session coordinator
 *
 * Wires input -> conversation -> provider streaming -> renderer.
 *
 * CMP-403: OOM recovery + incremental JSONL persistence.
 *   - On arena OOM mid-turn, flush partial response, reset arena, continue.
 *   - Each completed turn is written to ~/.local/share/nanocode/sessions/<id>.jsonl
 *   - arena->used is used directly as a checkpoint (no API change to arena.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

    /* CMP-403: OOM recovery */
    size_t  oom_checkpoint;
    int     oom_in_turn;
    char   *system_backup;

    /* CMP-403: JSONL persistence */
    FILE   *jsonl_fp;
    int     jsonl_turn_idx;
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
 * CMP-403: JSONL persistence helpers
 * ---------------------------------------------------------------------- */

static void jsonl_write_escaped(FILE *fp, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n",  fp); break;
        case '\r': fputs("\\r",  fp); break;
        case '\t': fputs("\\t",  fp); break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", (unsigned)c);
            else          fputc(c, fp);
        }
    }
}

static void jsonl_write_turn(FILE *fp, int idx,
                             const char *role, const char *content)
{
    if (!fp) return;
    fprintf(fp, "{\"idx\":%d,\"role\":\"", idx);
    jsonl_write_escaped(fp, role);
    fprintf(fp, "\",\"content\":\"");
    jsonl_write_escaped(fp, content);
    fprintf(fp, "\"}\n");
    fflush(fp);
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static FILE *jsonl_open(void)
{
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.local/share/nanocode/sessions", home);
    mkdir_p(dir);
    time_t now = time(NULL);
    char path[1200];
    snprintf(path, sizeof(path), "%s/%lld.jsonl", dir, (long long)now);
    return fopen(path, "a");
}

static void repl_recover_oom(ReplSession *rs)
{
    fprintf(stderr, "\n[nanocode] OOM mid-turn -- resetting arena and recovering\n");
    buf_reset(&rs->resp_buf);
    rs->pending     = NULL;
    rs->npending    = 0;
    rs->pending_cap = 0;
    rs->arena->used = rs->oom_checkpoint;
    rs->conv = conv_new(rs->arena);
    if (rs->conv && rs->system_backup)
        conv_add(rs->conv, "system", rs->system_backup);
    rs->oom_in_turn = 0;
}

static void repl_start_stream(ReplSession *rs);
static void repl_on_done(int error, const char *stop_reason, void *ctx);

static void repl_on_token(const char *tok, size_t len, void *ctx)
{
    ReplSession *rs = ctx;
    repl_transition(rs->g.repl_ctx, REPL_STREAMING);
    if (rs->g.renderer && *rs->g.renderer)
        renderer_token(*rs->g.renderer, tok, len);
    if (buf_append(&rs->resp_buf, tok, len) != 0) {
        rs->oom_in_turn = 1;
        return;
    }
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
        if (!t) { rs->oom_in_turn = 1; return; }
        if (rs->pending)
            memcpy(t, rs->pending, (size_t)rs->npending * sizeof(ToolCall));
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

    if (rs->oom_in_turn) {
        repl_recover_oom(rs);
        repl_transition(rs->g.repl_ctx, REPL_DONE);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

    const char *text = buf_str(&rs->resp_buf);
    if (text && text[0] && !error)
        jsonl_write_turn(rs->jsonl_fp, rs->jsonl_turn_idx++, "assistant", text);

    int nturn_before = rs->conv ? rs->conv->nturn : 0;
    if (text && text[0])
        conv_add(rs->conv, "assistant", text);

    if (rs->conv && rs->conv->nturn == nturn_before && text && text[0]) {
        buf_reset(&rs->resp_buf);
        rs->oom_in_turn = 1;
        repl_recover_oom(rs);
        repl_transition(rs->g.repl_ctx, REPL_DONE);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }

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
            tool_dispatch_all(rs->pending, rs->npending, rs->conv, tool_arena);
            arena_free(tool_arena);
        }
        rs->pending = NULL; rs->npending = 0; rs->pending_cap = 0;
    }

    int is_end = !stop_reason ||
                 strcmp(stop_reason, "end_turn") == 0 ||
                 strcmp(stop_reason, "stop") == 0;

    if (!is_end) { repl_start_stream(rs); return; }

    repl_transition(rs->g.repl_ctx, REPL_DONE);
    (*rs->g.sb_turn)++;
    if (rs->input_ctx)
        input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
    repl_transition(rs->g.repl_ctx, REPL_PROMPT);
}

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
                        repl_on_token, repl_on_tool, repl_on_done, rs) != 0) {
        fprintf(stderr, "[nanocode] failed to start stream\n");
        arena_free(msg_arena);
        repl_transition(rs->g.repl_ctx, REPL_PROMPT);
        return;
    }
    arena_free(msg_arena);
}

static void repl_line_cb(InputLine line, void *userdata)
{
    ReplSession *rs = userdata;
    if (line.is_eof) { loop_stop(rs->loop); return; }
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
    jsonl_write_turn(rs->jsonl_fp, rs->jsonl_turn_idx++, "user", line.text);
    int nturn_before = rs->conv ? rs->conv->nturn : 0;
    conv_add(rs->conv, "user", line.text);
    if (rs->conv && rs->conv->nturn == nturn_before) {
        rs->oom_in_turn = 1;
        repl_recover_oom(rs);
        conv_add(rs->conv, "user", line.text);
    }
    *rs->g.sb_in_tok += (int)line.len;
    repl_start_stream(rs);
}

void repl_coordinator_setup(Loop *loop,
                             ProviderConfig *provider_cfg,
                             const SandboxConfig *sc,
                             Arena *arena,
                             ReplGlobals *globals)
{
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
    PromptParts parts = prompt_build_parts(
        arena, cwd[0] ? cwd : ".", NULL, sc, 0, 0);
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
    g_rs.oom_checkpoint = arena->used;
    g_rs.system_backup  = parts.dynamic_part ? strdup(parts.dynamic_part) : NULL;
    g_rs.jsonl_fp       = jsonl_open();
    g_rs.jsonl_turn_idx = 0;
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
    if (g_rs.input_ctx) { input_ctx_free(g_rs.input_ctx); g_rs.input_ctx = NULL; }
    if (g_rs.provider)  { provider_free(g_rs.provider);   g_rs.provider  = NULL; }
    buf_destroy(&g_rs.resp_buf);
    free(g_rs.system_backup);
    g_rs.system_backup = NULL;
    if (g_rs.jsonl_fp) { fclose(g_rs.jsonl_fp); g_rs.jsonl_fp = NULL; }
}
