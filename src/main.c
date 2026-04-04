/*
 * nanocode — a zero-dependency AI coding agent in C
 *
 * Entry point. Handles signals, argument parsing, and top-level lifecycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "../include/audit.h"
#include "../include/benchmark.h"
#include "../include/config.h"
#include "../include/profile.h"
#include "../include/history.h"
#include "../include/profile.h"
#include "../include/editor.h"
#include "../include/json_output.h"
#include "../include/oneshot.h"
#include "../include/sandbox.h"
#include "../include/status_file.h"
#include "../include/daemon.h"
#include "../src/tui/renderer.h"
#include "../src/agent/conversation.h"
#include "../src/agent/prompt.h"
#include "../src/api/provider.h"
#include "../src/core/loop.h"
#include "../src/util/arena.h"
#include "../src/util/buf.h"
#include "../src/util/duration.h"
#include "../src/tools/bash.h"
#include "../src/tools/executor.h"
#include "../src/tools/fileops.h"
#include "../src/tools/grep.h"
#include "../src/tools/memory.h"
#include "../src/agent/git.h"
#include "../include/pet.h"
#include "../src/tui/repl_state.h"
#include "../src/tui/spinner.h"
#include "../src/tui/statusbar.h"
#include "../src/agent/tool_protocol.h"
#include "../src/tui/commands.h"
#include "../src/tui/input.h"
#include "../src/tui/renderer.h"
#include "repl_coordinator.h"
#include "pipe.h"

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_winch   = 0;  /* set by SIGWINCH handler */
static Loop      *g_loop       = NULL;
static Loop      *g_os_loop    = NULL;  /* set during one-shot; cleared after */
static StatusBar *g_statusbar  = NULL;
static Renderer  *g_renderer   = NULL;  /* set when renderer is created */
static Pet        g_pet;
static Spinner    g_spinner;
static ReplCtx    g_repl_ctx;
static int        g_sb_in_tok  = 0;
static int        g_sb_out_tok = 0;
static int        g_sb_turn    = 1;

/* -------------------------------------------------------------------------
 * Animation timer — fires at 16 ms (streaming) or 100 ms (idle).
 * Ticks the spinner / pet and redraws the status bar.
 * ---------------------------------------------------------------------- */

static int anim_timer_cb(int timer_id, int events, void *ctx);

static void rearm_anim_timer(Loop *l, int streaming)
{
    int ms = streaming ? 16 : 100;
    loop_add_timer(l, ms, anim_timer_cb, l);
}

static int anim_timer_cb(int timer_id, int events, void *ctx)
{
    (void)timer_id; (void)events;
    Loop *l = ctx;
    /* Deferred SIGWINCH dispatch — re-query terminal width. */
    if (g_winch) {
        g_winch = 0;
        if (g_renderer)
            renderer_update_width(g_renderer);
    }
    repl_tick(&g_repl_ctx);
    if (g_statusbar)
        statusbar_update(g_statusbar, g_sb_in_tok, g_sb_out_tok, g_sb_turn);
    ReplState st = repl_get_state(&g_repl_ctx);
    int streaming = (st == REPL_STREAMING || st == REPL_THINKING ||
                     st == REPL_TOOL_EXEC);
    rearm_anim_timer(l, streaming);
    return -1;
}

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_loop)
        loop_stop(g_loop);
    if (g_os_loop)
        loop_stop(g_os_loop);
}

static void handle_sigwinch(int sig)
{
    (void)sig;
    g_winch = 1;
}

static int session_timeout_cb(int timer_id, int events, void *ctx)
{
    (void)timer_id; (void)events; (void)ctx;
    fprintf(stderr, "[nanocode] session timeout reached. shutting down.\n");
    loop_stop(g_loop);
    return -1;  /* removes the timer */
}

/* Stub dispatch — placeholder until agent wiring is complete. */
static const char *daemon_dispatch_stub(const char *prompt, const char *cwd,
                                         void *ctx)
{
    (void)prompt; (void)cwd; (void)ctx;
    return "ok";
}

/* -------------------------------------------------------------------------
 * REPL coordinator — wires input → provider → renderer
 *
 * ReplSession holds all per-session state.  Callbacks are registered with
 * the provider and input layer; they drive state transitions, token
 * rendering, tool dispatch, and re-streaming until the model reaches
 * end_turn.
 * ---------------------------------------------------------------------- */

typedef struct {
    Loop                 *loop;
    Provider             *provider;
    Conversation         *conv;
    Arena                *arena;        /* session-long arena (not owned) */
    const ProviderConfig *pcfg;
    Buf                   resp_buf;     /* accumulates text tokens per turn */
    ToolCall             *pending;      /* heap-allocated tool-call array */
    int                   npending;
    int                   pending_cap;
    InputCtx             *input_ctx;
} ReplSession;

static ReplSession g_rs;

/* Forward declarations */
static void repl_on_done(int error, const char *stop_reason, void *ctx);
static int  repl_start_stream(ReplSession *rs);

static void repl_free_pending(ReplSession *rs)
{
    for (int i = 0; i < rs->npending; i++) {
        free(rs->pending[i].id);
        free(rs->pending[i].name);
        free(rs->pending[i].input);
    }
    rs->npending = 0;
}

static void repl_on_token(const char *tok, size_t len, void *ctx)
{
    ReplSession *rs = ctx;
    repl_transition(&g_repl_ctx, REPL_STREAMING);
    if (g_renderer)
        renderer_token(g_renderer, tok, len);
    buf_append(&rs->resp_buf, tok, len);
    g_sb_out_tok++;
}

static void repl_on_tool(const char *id, const char *name,
                          const char *input, void *ctx)
{
    ReplSession *rs = ctx;
    if (rs->npending >= rs->pending_cap) {
        int new_cap = rs->pending_cap ? rs->pending_cap * 2 : 4;
        ToolCall *arr = realloc(rs->pending,
                                (size_t)new_cap * sizeof(ToolCall));
        if (!arr)
            return;
        rs->pending     = arr;
        rs->pending_cap = new_cap;
    }
    ToolCall *tc = &rs->pending[rs->npending++];
    tc->id    = strdup(id);
    tc->name  = strdup(name);
    tc->input = strdup(input);
}

static void repl_on_done(int error, const char *stop_reason, void *ctx)
{
    (void)stop_reason;
    ReplSession *rs = ctx;

    /* Flush any partial rendered output. */
    if (g_renderer)
        renderer_flush(g_renderer);

    if (error) {
        fprintf(stderr, "\nnanocode: stream error\n");
        repl_transition(&g_repl_ctx, REPL_PROMPT);
        buf_reset(&rs->resp_buf);
        repl_free_pending(rs);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        return;
    }

    /* Record accumulated text as assistant turn. */
    if (rs->resp_buf.len > 0) {
        const char *text = buf_str(&rs->resp_buf);
        conv_add(rs->conv, "assistant", text);
        buf_reset(&rs->resp_buf);
    }

    /* Tool calls pending — dispatch synchronously, then re-stream. */
    if (rs->npending > 0) {
        repl_transition(&g_repl_ctx, REPL_TOOL_EXEC);
        /* tool_dispatch_all records tool_use + tool_result turns in conv.
         * The pending strings are strdup'd; they survive the call and are
         * freed immediately after. */
        tool_dispatch_all(rs->pending, rs->npending, rs->conv, rs->arena);
        repl_free_pending(rs);
        if (repl_start_stream(rs) != 0) {
            repl_transition(&g_repl_ctx, REPL_PROMPT);
            if (rs->input_ctx)
                input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        }
        return;
    }

    /* Turn complete — return to prompt. */
    repl_transition(&g_repl_ctx, REPL_DONE);
    repl_transition(&g_repl_ctx, REPL_PROMPT);
    if (rs->input_ctx)
        input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
}

static int repl_start_stream(ReplSession *rs)
{
    repl_transition(&g_repl_ctx, REPL_THINKING);

    /*
     * Build the message array from the conversation.  Use an ephemeral
     * arena so the array doesn't permanently occupy the session arena.
     * provider_stream() copies everything it needs before returning.
     */
    Arena *tmp = arena_new(256 * 1024);
    if (!tmp)
        return -1;

    int      nmsg = 0;
    Message *msgs = conv_to_messages(rs->conv, &nmsg, tmp);
    if (!msgs || nmsg == 0) {
        arena_free(tmp);
        return -1;
    }

    int rc = provider_stream(rs->provider, msgs, nmsg,
                             repl_on_token, repl_on_tool, repl_on_done, rs);
    arena_free(tmp);
    return rc;
}

static void repl_line_cb(InputLine line, void *userdata)
{
    ReplSession *rs = userdata;

    /* EOF (Ctrl+D) — stop the loop. */
    if (line.is_eof) {
        loop_stop(rs->loop);
        return;
    }

    /* Empty line — re-show prompt. */
    if (line.len == 0) {
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        return;
    }

    input_history_add(line.text);

    /* Slash commands. */
    if (cmd_is_command(line.text)) {
        char *model_ptr = (char *)(uintptr_t)rs->pcfg->model;
        CmdContext cctx = {
            .conv    = rs->conv,
            .model   = &model_ptr,
            .in_tok  = g_sb_in_tok,
            .out_tok = g_sb_out_tok,
            .fd_out  = STDOUT_FILENO,
            .fd_in   = STDIN_FILENO,
        };
        cmd_dispatch(line.text, &cctx);
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
        return;
    }

    /* Normal user message — add to conversation and stream. */
    conv_add(rs->conv, "user", line.text);
    g_sb_in_tok++;

    if (repl_start_stream(rs) != 0) {
        fprintf(stderr, "nanocode: failed to start stream\n");
        if (rs->input_ctx)
            input_ctx_reset(rs->input_ctx, rs->arena, "You: ");
    }
}

/* -------------------------------------------------------------------------
 * One-shot streaming callbacks and execution
 *
 * Implements a full agentic loop: stream → tool dispatch → stream → ...
 * until the model reaches end_turn or ONESHOT_MAX_TURNS is exhausted.
 * ---------------------------------------------------------------------- */

#define ONESHOT_MAX_TOOLS  32
#define ONESHOT_MAX_TURNS  20

typedef struct {
    Loop      *loop;
    int        done;
    int        error;
    int        needs_tools;   /* stop_reason was "tool_use" */

    /* Tool calls accumulated during this stream turn. */
    ToolCall   tool_calls[ONESHOT_MAX_TOOLS];
    int        ntool_calls;

    Arena     *arena;
} OneshotCtx;

/* Arena string copy — avoids heap allocation for per-turn tool args. */
static char *oneshot_arena_dup(Arena *a, const char *s)
{
    if (!s || !a) return NULL;
    size_t n = strlen(s) + 1;
    char  *d = arena_alloc(a, n);
    if (d) memcpy(d, s, n);
    return d;
}

static void oneshot_on_token(const char *token, size_t len, void *ctx)
{
    (void)ctx;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
}

static void oneshot_on_tool(const char *id, const char *name,
                             const char *input, void *ctx)
{
    OneshotCtx *oc = ctx;
    if (oc->ntool_calls >= ONESHOT_MAX_TOOLS) return;
    int i = oc->ntool_calls++;
    oc->tool_calls[i].id    = oneshot_arena_dup(oc->arena, id    ? id    : "");
    oc->tool_calls[i].name  = oneshot_arena_dup(oc->arena, name  ? name  : "");
    oc->tool_calls[i].input = oneshot_arena_dup(oc->arena, input ? input : "{}");
}

static void oneshot_on_done(int error, const char *stop_reason, void *ctx)
{
    OneshotCtx *oc = ctx;
    oc->error       = error;
    oc->needs_tools = (!error && stop_reason &&
                       strcmp(stop_reason, "tool_use") == 0);
    oc->done        = 1;
    loop_stop(oc->loop);
}

/*
 * Fileops confirm callback for oneshot mode.
 * Logs each file change to stdout and auto-applies (returns 1 = apply,
 * keep callback active for subsequent writes).
 */
static int oneshot_fileops_cb(const char *path,
                               const char *old_content,
                               const char *new_content,
                               void *ctx)
{
    (void)ctx; (void)new_content;
    const char *action = old_content ? "modified" : "created";
    printf("[%s] %s\n", action, path);
    fflush(stdout);
    return 1;
}

/*
 * run_oneshot — run a single instruction through the full agentic loop:
 *   1. Read instruction (or read from stdin when command == "-").
 *   2. Register tools and wire fileops auto-apply callback.
 *   3. Stream provider response.
 *   4. If stop_reason == "tool_use": dispatch tools, add results to conv, repeat.
 *   5. On end_turn: exit 0.  On error: exit 1.  On bad args: exit 2.
 *
 * g_os_loop is set before loop_run() so SIGINT/SIGTERM can stop the loop.
 */
static int run_oneshot(const OneShotFlags *flags, const ProviderConfig *pcfg,
                       const SandboxConfig *sc, Arena *arena)
{
    /* Resolve instruction: "-" means read from stdin. */
    const char *instruction = flags->command;
    char *stdin_buf = NULL;
    if (instruction && strcmp(instruction, "-") == 0) {
        size_t cap = 4096, used = 0;
        stdin_buf = malloc(cap);
        if (!stdin_buf) {
            fprintf(stderr, "nanocode: out of memory reading stdin\n");
            return 1;
        }
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (used + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(stdin_buf, cap);
                if (!nb) { free(stdin_buf); return 1; }
                stdin_buf = nb;
            }
            stdin_buf[used++] = (char)c;
        }
        stdin_buf[used] = '\0';
        if (used == 0) {
            fprintf(stderr, "nanocode: empty instruction from stdin\n");
            free(stdin_buf);
            return 2;
        }
        instruction = stdin_buf;
    }

    if (!instruction || !instruction[0]) {
        fprintf(stderr, "nanocode: empty instruction\n");
        free(stdin_buf);
        return 2;
    }

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0';

    const char *system_prompt = prompt_build(arena, cwd[0] ? cwd : ".", NULL, sc, 0);
    if (!system_prompt) {
        fprintf(stderr, "nanocode: failed to build system prompt\n");
        free(stdin_buf);
        return 1;
    }

    Conversation *conv = conv_new(arena);
    if (!conv) {
        fprintf(stderr, "nanocode: failed to create conversation\n");
        free(stdin_buf);
        return 1;
    }
    conv_add(conv, "system", system_prompt);
    conv_add(conv, "user",   instruction);
    free(stdin_buf);
    stdin_buf = NULL;

    /* Wire file-change tracking callback (auto-applies and logs to stdout).
     * In dry-run mode the executor won't write but the cb is harmless. */
    fileops_set_confirm_cb(oneshot_fileops_cb, NULL);

    Loop *os_loop = loop_new();
    if (!os_loop) {
        fprintf(stderr, "nanocode: failed to create event loop\n");
        return 1;
    }

    Provider *prov = provider_new(os_loop, pcfg);
    if (!prov) {
        fprintf(stderr, "nanocode: failed to create provider\n");
        loop_free(os_loop);
        return 1;
    }

    /* Expose loop to signal handler so SIGINT/SIGTERM cleanly stop the run. */
    g_os_loop = os_loop;

    int exit_rc = 1;

    /* Agentic loop: stream → tool dispatch → stream → ... */
    for (int turn = 0; turn < ONESHOT_MAX_TURNS; turn++) {
        int nmsg = 0;
        Message *msgs = conv_to_messages(conv, &nmsg, arena);
        if (!msgs || nmsg == 0) {
            fprintf(stderr, "nanocode: failed to build message array\n");
            break;
        }

        OneshotCtx oc;
        memset(&oc, 0, sizeof(oc));
        oc.loop  = os_loop;
        oc.arena = arena;

        if (provider_stream(prov, msgs, nmsg,
                            oneshot_on_token,
                            oneshot_on_tool,
                            oneshot_on_done,
                            &oc) != 0) {
            fprintf(stderr, "nanocode: failed to start stream\n");
            break;
        }

        loop_run(os_loop);

        if (oc.error) {
            exit_rc = 1;
            break;
        }

        if (!oc.needs_tools) {
            /* Natural end_turn — success. */
            fputc('\n', stdout);
            exit_rc = 0;
            break;
        }

        /* Dispatch tool calls and append results to conversation. */
        if (oc.ntool_calls > 0)
            tool_dispatch_all(oc.tool_calls, oc.ntool_calls, conv, arena);
    }

    g_os_loop = NULL;
    provider_free(prov);
    loop_free(os_loop);

    return oneshot_exit_code(exit_rc, 0);
}

int main(int argc, char **argv)
{
    signal(SIGINT,   handle_signal);
    signal(SIGTERM,  handle_signal);
    signal(SIGWINCH, handle_sigwinch);

    /* -----------------------------------------------------------------------
     * Phase 0: Handle `nanocode edit <path>:<line>` subcommand.
     *
     * Opens a file in the user's terminal editor ($VISUAL → $EDITOR → vi).
     * Usage: nanocode edit src/foo.c:42
     * -------------------------------------------------------------------- */
    if (argc >= 2 && strcmp(argv[1], "edit") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: nanocode edit <path>[:<line>]\n");
            return 1;
        }
        int rc = editor_open_from_ref(argv[2], NULL /* no sandbox check */);
        return rc == 0 ? 0 : 1;
    }

    /* -----------------------------------------------------------------------
     * Phase 1: Parse CLI flags that must be applied before / after config.
     *
     * --json                     emit a single JSON object on stdout and exit
     * --sandbox                  force sandbox.enabled = true
     * --sandbox-profile <name>   override sandbox.profile
     * --timeout <duration>       session timeout (e.g. 30m, 1h, 90s)
     * --daemon                   run as Unix socket daemon
     * -c <instruction>           one-shot non-interactive execution
     * --command <instruction>    one-shot non-interactive execution (long form)
     * --auto-apply               apply all file changes without prompting
     * --model <name>             override provider.model (CMP-382)
     * --profile <name>           force a specific provider profile (CMP-382)
     * --list-profiles            list available provider profiles and exit (CMP-382)
     * -------------------------------------------------------------------- */
    int         cli_json            = 0;
    int         cli_sandbox         = 0;
    int         cli_daemon          = 0;
    int         cli_dry_run         = 0;
    int         cli_readonly        = 0;
    int         cli_pipe            = 0;
    int         cli_raw             = 0;
    int         cli_resume          = 0;  /* --resume: pick and load a past conv */
    const char *cli_search          = NULL; /* --search <term>: grep history, exit */
    const char *cli_instruction     = NULL; /* positional arg for pipe mode */
    const char *cli_sandbox_profile = NULL;
    const char *cli_model           = NULL; /* --model: override provider.model (CMP-382) */
    const char *cli_profile_name    = NULL; /* --profile: force profile name (CMP-382) */
    int         cli_list_profiles   = 0;   /* --list-profiles: list and exit (CMP-382) */
    char        cli_timeout_arg[64] = "";
    OneShotFlags oneshot;
    memset(&oneshot, 0, sizeof(oneshot));
    BenchFlags bench_flags;
    memset(&bench_flags, 0, sizeof(bench_flags));
    int         cli_benchmark       = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) { cli_json = 1; break; }
    }
    if (!cli_json && !isatty(STDOUT_FILENO)) {
        const char *env = getenv("NANOCODE_JSON");
        if (env && strcmp(env, "1") == 0) cli_json = 1;
    }

    /* Short-circuit: JSON mode emits the envelope and exits without TUI. */
    if (cli_json) {
        JsonOutput jout;
        json_output_init(&jout);
        jout.status      = "done";
        jout.result      = "nanocode json mode active (stub)";
        jout.duration_ms = 0;
        json_output_print(&jout);
        json_output_free(&jout);
        return NC_EXIT_OK;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            cli_daemon = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            cli_dry_run = 1;
        } else if (strcmp(argv[i], "--readonly") == 0) {
            cli_readonly = 1;
        } else if (strcmp(argv[i], "--sandbox") == 0) {
            cli_sandbox = 1;
        } else if (strcmp(argv[i], "--sandbox-profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "nanocode: --sandbox-profile requires a name "
                        "(strict | permissive | custom)\n");
                return 1;
            }
            cli_sandbox_profile = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "nanocode: --timeout requires a duration "
                        "(e.g. 30m, 1h, 90s)\n");
                return 1;
            }
            strncpy(cli_timeout_arg, argv[++i], sizeof(cli_timeout_arg) - 1);
            cli_timeout_arg[sizeof(cli_timeout_arg) - 1] = '\0';
        } else if (strcmp(argv[i], "--pipe") == 0) {
            cli_pipe = 1;
        } else if (strcmp(argv[i], "--raw") == 0) {
            cli_raw = 1;
        } else if (strcmp(argv[i], "--resume") == 0) {
            cli_resume = 1;
        } else if (strcmp(argv[i], "--search") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nanocode: --search requires a search term\n");
                return 1;
            }
            cli_search = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nanocode: --model requires a model name\n");
                return 1;
            }
            cli_model = argv[++i];
        } else if (strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nanocode: --profile requires a profile name\n");
                return 1;
            }
            cli_profile_name = argv[++i];
        } else if (strcmp(argv[i], "--list-profiles") == 0) {
            cli_list_profiles = 1;
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            cli_benchmark = 1;
        } else if (strcmp(argv[i], "--compare") == 0) {
            bench_flags.do_compare = 1;
        } else if (strcmp(argv[i], "--tune") == 0) {
            bench_flags.do_tune = 1;
        } else {
            char err[256];
            int rc = oneshot_parse_arg(argc, argv, &i, &oneshot,
                                       err, sizeof(err));
            if (rc == -1) {
                fprintf(stderr, "%s\n", err);
                return 2;  /* exit 2 = invalid args */
            }
            /* Capture first positional arg as instruction (for pipe mode). */
            if (rc == 0 && argv[i][0] != '-' && !cli_instruction)
                cli_instruction = argv[i];
        }
    }

    /* Auto-detect pipe mode when stdin is not a TTY.
     * Exception: -c - explicitly requests stdin for the instruction; in that
     * case we must NOT activate pipe mode so the full config path is taken. */
    if (!cli_pipe && !isatty(STDIN_FILENO)) {
        int stdin_oneshot = (oneshot.enabled &&
                             oneshot.command && strcmp(oneshot.command, "-") == 0);
        if (!stdin_oneshot)
            cli_pipe = 1;
    }

    /* -----------------------------------------------------------------------
     * Phase 1.5: Pipe mode — early dispatch before full config load.
     *
     * pipe_run() resolves provider settings from env vars directly; it does
     * not need the full nanocode config machinery.
     * -------------------------------------------------------------------- */
    if (cli_pipe) {
        if (!cli_instruction) {
            fprintf(stderr,
                    "nanocode: pipe mode requires an instruction argument\n"
                    "  Example: cat file.c | nanocode --pipe \"explain this\"\n");
            return 1;
        }
        PipeArgs pargs;
        pargs.instruction = cli_instruction;
        pargs.model       = cli_model;
        pargs.raw         = cli_raw;
        return pipe_run(&pargs);
    }

    /* -----------------------------------------------------------------------
     * Phase 1.55: --list-profiles — list available provider profiles and exit.
     * -------------------------------------------------------------------- */
    if (cli_list_profiles) {
        profile_list();
        return 0;
    }

    /* -----------------------------------------------------------------------
     * Phase 1.6: --search — stream-search all history files and exit.
     *
     * Does not require the full config; uses $HOME directly.
     * -------------------------------------------------------------------- */
    if (cli_search) {
        int found = history_search(cli_search, STDOUT_FILENO);
        if (found == 0)
            fprintf(stderr, "nanocode: no matches found for '%s'\n", cli_search);
        return 0;
    }

    /* -----------------------------------------------------------------------
     * Phase 2: Load configuration.
     * -------------------------------------------------------------------- */
    Arena  *arena = arena_new(1 << 20);   /* 1 MB — covers config + session   */
    Config *cfg   = config_load(arena);
    if (!cfg) {
        fprintf(stderr, "nanocode: failed to load configuration\n");
        arena_free(arena);
        return 1;
    }

    /* Apply CLI overrides on top of the loaded config. */
    if (cli_sandbox)
        config_set(cfg, "sandbox.enabled", "true");
    if (cli_sandbox_profile)
        config_set(cfg, "sandbox.profile", cli_sandbox_profile);

    /* Resolve execution mode: config key first, then CLI flags (higher priority). */
    {
        ExecMode exec_mode = EXEC_MODE_NORMAL;
        const char *cfg_mode = config_get_str(cfg, "session.mode");
        if (cfg_mode && strcmp(cfg_mode, "dry-run") == 0)
            exec_mode = EXEC_MODE_DRY_RUN;
        else if (cfg_mode && strcmp(cfg_mode, "readonly") == 0)
            exec_mode = EXEC_MODE_READONLY;
        if (cli_dry_run)
            exec_mode = EXEC_MODE_DRY_RUN;
        if (cli_readonly)
            exec_mode = EXEC_MODE_READONLY;
        executor_set_mode(exec_mode);
    }


    /* -----------------------------------------------------------------------
     * Phase 2.5: Assemble ProviderConfig from config.
     * -------------------------------------------------------------------- */
    ProviderConfig provider_cfg;
    {
        const char *type_str   = config_get_str(cfg, "provider.type");
        const char *base_url   = config_get_str(cfg, "provider.base_url");
        const char *model      = config_get_str(cfg, "provider.model");
        const char *api_key    = config_get_str(cfg, "provider.api_key");
        int         port_cfg   = config_get_int(cfg, "provider.port");

        /* CLI --model overrides config (CMP-382) */
        if (cli_model)
            model = cli_model;

        ProviderType ptype = PROVIDER_CLAUDE;
        if (type_str && strcmp(type_str, "openai") == 0)
            ptype = PROVIDER_OPENAI;
        else if (type_str && strcmp(type_str, "ollama") == 0)
            ptype = PROVIDER_OLLAMA;

        int port = port_cfg;
        if (port <= 0)
            port = (ptype == PROVIDER_CLAUDE) ? 443 : 11434;

        int use_tls = 1;
        if (base_url && (strcmp(base_url, "localhost") == 0 ||
                         strncmp(base_url, "127.", 4) == 0 ||
                         strcmp(base_url, "::1") == 0))
            use_tls = 0;
        /* Cloud Claude endpoints always use TLS. */
        if (ptype == PROVIDER_CLAUDE && use_tls == 0 && port_cfg == 443)
            use_tls = 1;

        provider_cfg.type            = ptype;
        provider_cfg.base_url        = base_url ? base_url : "api.anthropic.com";
        provider_cfg.port            = port;
        provider_cfg.use_tls         = use_tls;
        provider_cfg.api_key         = api_key;
        provider_cfg.model           = model ? model : "claude-opus-4-6";
        provider_cfg.thinking_budget = 0;
        provider_cfg.system_cache_static = NULL;
        /* New sampling fields — defaults mean "unset / use provider default" */
        provider_cfg.temperature_x1000 = -1;
        provider_cfg.top_p_x1000       = -1;
        provider_cfg.max_output_tokens  = 0;
    }

    /* -----------------------------------------------------------------------
     * Phase 2.6: Load provider profile and apply to ProviderConfig (CMP-382).
     *
     * Priority: --profile flag > auto-select by model name
     * Profile fields are lower priority than any future explicit CLI flags.
     * -------------------------------------------------------------------- */
    {
        ProviderProfile *prof;
        if (cli_profile_name)
            prof = profile_load(arena, cli_profile_name);
        else
            prof = profile_for_model(arena, provider_cfg.model);

        if (prof) {
            /* Apply profile values that have not been overridden via CLI */
            if (prof->thinking_budget > 0 && provider_cfg.thinking_budget == 0)
                provider_cfg.thinking_budget = prof->thinking_budget;
            if (prof->temperature_x1000 >= 0 && provider_cfg.temperature_x1000 < 0)
                provider_cfg.temperature_x1000 = prof->temperature_x1000;
            if (prof->top_p_x1000 >= 0 && provider_cfg.top_p_x1000 < 0)
                provider_cfg.top_p_x1000 = prof->top_p_x1000;
            if (prof->max_output_tokens > 0 && provider_cfg.max_output_tokens == 0)
                provider_cfg.max_output_tokens = prof->max_output_tokens;
        }
    }

    /* -----------------------------------------------------------------------
     * Phase 3: Validate and activate sandbox.
     * -------------------------------------------------------------------- */
    SandboxConfig sc;
    sandbox_config_from_cfg(&sc, cfg);

    if (sc.enabled) {
        if (sandbox_validate(&sc) != 0) {
            arena_free(arena);
            return 1;
        }
        if (sandbox_activate(&sc) != 0) {
            arena_free(arena);
            return 1;
        }
    }

    /* -----------------------------------------------------------------------
     * Phase 3.5: Wire tool resource limits and command filter.
     * -------------------------------------------------------------------- */
    fileops_set_limits(
        (long)config_get_int(cfg, "sandbox.max_file_size"),
        config_get_int(cfg, "session.max_files_created")
    );
    bash_set_cmd_filter(
        config_get_str(cfg, "sandbox.allowed_commands"),
        config_get_str(cfg, "sandbox.denied_commands")
    );


    /* -----------------------------------------------------------------------
     * Phase 3.6: Open audit log and wire to executor, bash, and fileops.
     * -------------------------------------------------------------------- */
    AuditLog *g_audit = NULL;
    if (config_get_bool(cfg, "audit.enabled")) {
        const char *apath = config_get_str(cfg, "audit.path");
        char default_apath[512];
        if (!apath || !apath[0]) {
            const char *home = getenv("HOME");
            snprintf(default_apath, sizeof(default_apath),
                     "%s/.nanocode/audit.log",
                     home && home[0] ? home : ".");
            apath = default_apath;
        }
        g_audit = audit_open(apath,
                             (long)config_get_int(cfg, "audit.max_size_bytes"),
                             config_get_int(cfg, "audit.max_files"));
        if (g_audit) {
            const char *profile = config_get_str(cfg, "sandbox.profile");
            executor_set_audit(g_audit, NULL, profile, NULL);
            bash_set_audit(g_audit, NULL, profile);
            fileops_set_audit(g_audit, NULL, profile);
        }
    }

    /* -----------------------------------------------------------------------
     * Phase 3.55: Register tools with the executor.
     *
     * Must happen before any provider stream (oneshot or REPL) so that
     * tool_dispatch_all() can resolve tool names to handlers.
     * -------------------------------------------------------------------- */
    fileops_register_all();
    bash_tool_register();
    grep_register();
    memory_tool_register();
    git_tools_register();
    tool_search_register();

    /* -----------------------------------------------------------------------
     * Phase 3.7: One-shot dispatch — skip TUI and run a single instruction.
     * -------------------------------------------------------------------- */
    if (oneshot.enabled) {
        int rc = run_oneshot(&oneshot, &provider_cfg, &sc, arena);
        audit_close(g_audit);
        arena_free(arena);
        return rc;
    }

    /* -----------------------------------------------------------------------
     * Phase 3.8: Benchmark dispatch — run test suite and exit.
     *
     * --benchmark [--compare] [--tune]
     * -------------------------------------------------------------------- */
    if (cli_benchmark) {
        int rc = benchmark_run(&bench_flags, &provider_cfg, cfg);
        audit_close(g_audit);
        arena_free(arena);
        return rc;
    }

    /* -----------------------------------------------------------------------
     * Phase 4: Main loop.
     * -------------------------------------------------------------------- */
    g_loop = loop_new();
    if (!g_loop) {
        fprintf(stderr, "nanocode: failed to create event loop\n");
        audit_close(g_audit);
        arena_free(arena);
        return 1;
    }

    /* Resolve session timeout: --timeout flag overrides config value. */
    long timeout_secs = parse_duration(config_get_str(cfg, "session.timeout"));
    if (cli_timeout_arg[0]) {
        long cli_secs = parse_duration(cli_timeout_arg);
        if (cli_secs < 0) {
            fprintf(stderr,
                    "nanocode: invalid --timeout value: %s\n", cli_timeout_arg);
            loop_free(g_loop);
            g_loop = NULL;
            audit_close(g_audit);
            arena_free(arena);
            return 1;
        }
        timeout_secs = cli_secs;
    }

    if (timeout_secs > 0) {
        int timeout_ms = (int)((timeout_secs > (long)(INT_MAX / 1000))
                               ? INT_MAX : timeout_secs * 1000);
        loop_add_timer(g_loop, timeout_ms, session_timeout_cb, NULL);
    }

    /* -----------------------------------------------------------------------
     * Phase 4.5: Daemon mode — Unix socket + status file.
     * -------------------------------------------------------------------- */
    Daemon    *g_daemon     = NULL;
    StatusInfo g_status     = {0};
    char       g_started_at[32] = {0};
    const char *status_path = config_get_str(cfg, "daemon.status_path");
    const char *sock_path   = config_get_str(cfg, "daemon.sock_path");

    if (!status_path || !status_path[0])
        status_path = "nanocode.status.json";
    if (!sock_path || !sock_path[0])
        sock_path = "nanocode.sock";

    if (cli_daemon) {
        time_t now = time(NULL);
        struct tm *tm_utc = gmtime(&now);
        if (tm_utc)
            strftime(g_started_at, sizeof(g_started_at),
                     "%Y-%m-%dT%H:%M:%SZ", tm_utc);

        g_status.pid         = getpid();
        g_status.state       = "idle";
        g_status.task        = NULL;
        g_status.started_at  = g_started_at;
        g_status.last_action = NULL;
        g_status.tool_calls  = 0;
        status_file_write(status_path, &g_status);

        g_daemon = daemon_start(g_loop, sock_path, daemon_dispatch_stub, NULL);
        if (!g_daemon) {
            fprintf(stderr, "nanocode: failed to start daemon on %s\n",
                    sock_path);
            loop_free(g_loop);
            g_loop = NULL;
            status_file_remove(status_path);
            audit_close(g_audit);
            arena_free(arena);
            return 1;
        }
        printf("nanocode daemon listening on %s\n", sock_path);
    }

    /* -----------------------------------------------------------------------
     * Phase 4.8: --resume — list recent conversations and load a selection.
     *
     * Pre-loads a past Conversation for the upcoming TUI session.
     * Once session wiring is complete this will be passed to the coordinator;
     * for now the result is printed to inform the user and stored for future use.
     * -------------------------------------------------------------------- */
    Conversation *resume_conv = NULL;
    if (cli_resume) {
        char *paths[HISTORY_LIST_MAX];
        int n = history_list_recent(paths, HISTORY_LIST_MAX);
        if (n == 0) {
            fprintf(stderr, "nanocode: no saved conversations found\n");
        } else {
            printf("Recent conversations:\n");
            for (int i = 0; i < n; i++) {
                const char *base = strrchr(paths[i], '/');
                printf("  %d) %s\n", i + 1, base ? base + 1 : paths[i]);
            }
            printf("Enter number (or 0 to skip): ");
            fflush(stdout);
            int choice = 0;
            if (scanf("%d", &choice) == 1 && choice >= 1 && choice <= n)
                resume_conv = history_load(arena, paths[choice - 1]);
            for (int i = 0; i < n; i++) free(paths[i]);
            if (resume_conv)
                printf("Loaded %d turns from history.\n", resume_conv->nturn);
        }
    }
    /* -----------------------------------------------------------------------
     * Phase 5: TUI status bar + pet animation (interactive sessions only).
     * ---------------------------------------------------------------------- */
    if (isatty(STDERR_FILENO)) {
        const char *pet_cfg = config_get_str(cfg, "tui.pet");
        PetKind pet_kind;
        if (pet_cfg && pet_cfg[0])
            pet_kind = pet_kind_from_str(pet_cfg);
        else
            pet_kind = pet_kind_random();
        g_pet = pet_new(pet_kind);
        g_statusbar = statusbar_new(STDERR_FILENO, &provider_cfg,
                                    config_get_int(cfg, "session.max_turns"));
        if (g_statusbar) {
            statusbar_set_pet(g_statusbar, &g_pet);
            statusbar_set_session_start(g_statusbar);
        }
        repl_ctx_init(&g_repl_ctx, g_loop, &g_pet, g_statusbar, &g_spinner);
        rearm_anim_timer(g_loop, 0);
    }

    /* Create renderer for the interactive session.
     * g_renderer is used by the anim timer to reflow on SIGWINCH.
     * Not created in daemon mode (no interactive TUI). */
    if (!cli_daemon)
        g_renderer = renderer_new(STDOUT_FILENO, arena);

    /* -----------------------------------------------------------------------
     * Phase 6: Wire interactive REPL (non-daemon, TTY sessions only).
     * ---------------------------------------------------------------------- */
    if (!cli_daemon && isatty(STDIN_FILENO)) {
        ReplGlobals rg;
        rg.renderer   = &g_renderer;
        rg.repl_ctx   = &g_repl_ctx;
        rg.sb_in_tok  = &g_sb_in_tok;
        rg.sb_out_tok = &g_sb_out_tok;
        rg.sb_turn    = &g_sb_turn;
        repl_coordinator_setup(g_loop, &provider_cfg, &sc, arena, &rg);
    }

    printf("nanocode v0.1-dev\n");
    loop_run(g_loop);

    repl_coordinator_teardown();

    if (g_statusbar) {
        statusbar_clear(g_statusbar);
        statusbar_free(g_statusbar);
        g_statusbar = NULL;
    }

    if (cli_daemon) {
        daemon_stop(g_daemon);
        status_file_remove(status_path);
    }

    loop_free(g_loop);
    g_loop = NULL;
    audit_close(g_audit);
    arena_free(arena);
    return 0;
}
