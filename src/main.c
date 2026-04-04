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

#include "../include/audit.h"
#include "../include/config.h"
#include "../include/history.h"
#include "../include/profile.h"
#include "../include/editor.h"
#include "../include/json_output.h"
#include "../include/oneshot.h"
#include "../include/sandbox.h"
#include "../include/status_file.h"
#include "../include/daemon.h"
#include "../src/agent/conversation.h"
#include "../src/agent/prompt.h"
#include "../src/api/provider.h"
#include "../src/core/loop.h"
#include "../src/util/arena.h"
#include "../src/util/duration.h"
#include "../src/tools/bash.h"
#include "../src/tools/executor.h"
#include "../src/tools/fileops.h"
#include "../include/pet.h"
#include "../src/tui/repl_state.h"
#include "../src/tui/spinner.h"
#include "../src/tui/statusbar.h"
#include "../src/agent/tool_protocol.h"
#include "../src/tui/commands.h"
#include "../src/tui/input.h"
#include "../src/tui/renderer.h"
#include "pipe.h"

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_winch   = 0;  /* set by SIGWINCH handler */
static Loop      *g_loop       = NULL;
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
 * One-shot streaming callbacks and execution
 * ---------------------------------------------------------------------- */

typedef struct {
    Loop  *loop;
    int    done;
    int    error;
} OneshotStreamCtx;

static void oneshot_on_token(const char *token, size_t len, void *ctx)
{
    (void)ctx;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
}

static void oneshot_on_done(int error, const char *stop_reason, void *ctx)
{
    (void)stop_reason;
    OneshotStreamCtx *oc = ctx;
    oc->error = error;
    oc->done  = 1;
    loop_stop(oc->loop);
}

/*
 * run_oneshot — execute the -c instruction via the streaming API,
 * write plain-text output to stdout, and return 0 on success or 1 on error.
 */
static int run_oneshot(const OneShotFlags *flags, const ProviderConfig *pcfg,
                       const SandboxConfig *sc, Arena *arena)
{
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0';

    const char *system_prompt = prompt_build(arena, cwd[0] ? cwd : ".", NULL, sc);
    if (!system_prompt) {
        fprintf(stderr, "nanocode: failed to build system prompt\n");
        return 1;
    }

    Conversation *conv = conv_new(arena);
    if (!conv) {
        fprintf(stderr, "nanocode: failed to create conversation\n");
        return 1;
    }
    conv_add(conv, "system", system_prompt);
    conv_add(conv, "user",   flags->command);

    int nmsg = 0;
    Message *msgs = conv_to_messages(conv, &nmsg, arena);
    if (!msgs || nmsg == 0) {
        fprintf(stderr, "nanocode: failed to build message array\n");
        return 1;
    }

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

    OneshotStreamCtx oc = {os_loop, 0, 0};

    if (provider_stream(prov, msgs, nmsg,
                        oneshot_on_token,
                        NULL,
                        oneshot_on_done,
                        &oc) != 0) {
        fprintf(stderr, "nanocode: failed to start stream\n");
        provider_free(prov);
        loop_free(os_loop);
        return 1;
    }

    loop_run(os_loop);

    if (!oc.error)
        fputc('\n', stdout);

    provider_free(prov);
    loop_free(os_loop);

    return oneshot_exit_code(oc.error, 0);
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
        } else {
            char err[256];
            int rc = oneshot_parse_arg(argc, argv, &i, &oneshot,
                                       err, sizeof(err));
            if (rc == -1) {
                fprintf(stderr, "%s\n", err);
                return 1;
            }
            /* Capture first positional arg as instruction (for pipe mode). */
            if (rc == 0 && argv[i][0] != '-' && !cli_instruction)
                cli_instruction = argv[i];
        }
    }

    /* Auto-detect pipe mode when stdin is not a TTY. */
    if (!cli_pipe && !isatty(STDIN_FILENO))
        cli_pipe = 1;

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
     * Phase 3.7: One-shot dispatch — skip TUI and run a single instruction.
     * -------------------------------------------------------------------- */
    if (oneshot.enabled) {
        int rc = run_oneshot(&oneshot, &provider_cfg, &sc, arena);
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
    (void)resume_conv; /* wired into session coordinator once TUI is complete */

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

    printf("nanocode v0.1-dev\n");
    loop_run(g_loop);

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
