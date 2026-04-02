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

#include "../include/config.h"
#include "../include/editor.h"
#include "../include/json_output.h"
#include "../include/pet.h"
#include "../include/sandbox.h"
#include "../include/status_file.h"
#include "../include/daemon.h"
#include "../src/api/provider.h"
#include "../src/core/loop.h"
#include "../src/tui/statusbar.h"
#include "../src/util/arena.h"
#include "../src/util/duration.h"
#include "../src/tools/bash.h"
#include "../src/tools/executor.h"
#include "../src/tools/fileops.h"

static volatile sig_atomic_t g_running = 1;
static Loop *g_loop = NULL;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_loop)
        loop_stop(g_loop);
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

/* Tool event callback — transitions pet state based on tool execution. */
static void pet_tool_event_cb(ToolEvent ev, void *ctx)
{
    Pet *pet = ctx;
    switch (ev) {
        case TOOL_EVENT_START: pet_transition(pet, PET_ACTIVE); break;
        case TOOL_EVENT_DONE:  pet_transition(pet, PET_DONE);   break;
        case TOOL_EVENT_ERROR: pet_transition(pet, PET_ERROR);  break;
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

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
     * -------------------------------------------------------------------- */
    int         cli_json            = 0;
    int         cli_sandbox         = 0;
    int         cli_daemon          = 0;
    int         cli_dry_run         = 0;
    int         cli_readonly        = 0;
    int         cli_no_pet          = 0;
    const char *cli_sandbox_profile = NULL;
    const char *cli_pet_name        = NULL;
    char        cli_timeout_arg[64] = "";

    /* Single-pass flag parse so all flags are known before env-based autodetect. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            cli_json = 1;
        } else if (strcmp(argv[i], "--daemon") == 0) {
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
        } else if (strcmp(argv[i], "--no-pet") == 0) {
            cli_no_pet = 1;
        } else if (strcmp(argv[i], "--pet") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "nanocode: --pet requires a name "
                        "(cat | crab | dog | off)\n");
                return 1;
            }
            cli_pet_name = argv[++i];
        }
    }

    /* Non-TTY JSON autodetect: skip when --daemon is set to avoid early exit
     * before the daemon has a chance to start. */
    if (!cli_json && !cli_daemon && !isatty(STDOUT_FILENO)) {
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
        return NC_EXIT_OK;
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

    /* Resolve pet selection.
     * Priority (high → low): --no-pet / --pet flag, implicit disable, config.
     * --json mode is already handled above (early exit), so no check needed. */
    {
        /* Apply CLI pet flags (highest priority). */
        if (cli_no_pet)
            config_set(cfg, "pet", "off");
        else if (cli_pet_name)
            config_set(cfg, "pet", cli_pet_name);

        /* Implicit disable: --dry-run mode turns off pet output. */
        if (cli_dry_run)
            config_set(cfg, "pet", "off");

        const char *pet_val = config_get_str(cfg, "pet");

        /* First run: empty pet → default to cat (TODO: replace with
         * pet_kind_random() once subtask A lands). */
        if (!pet_val || pet_val[0] == '\0') {
            config_set(cfg, "pet", "cat");
            /* Persist the chosen pet so subsequent runs are consistent. */
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char save_path[512];
                snprintf(save_path, sizeof(save_path),
                         "%s/.nanocode/config.toml", home);
                config_save(cfg, save_path);
            }
            pet_val = config_get_str(cfg, "pet");
        }

        /* Validate pet name; warn and fall back to "cat" for unknown values. */
        if (pet_val && strcmp(pet_val, "cat")  != 0
                    && strcmp(pet_val, "crab") != 0
                    && strcmp(pet_val, "dog")  != 0
                    && strcmp(pet_val, "off")  != 0) {
            fprintf(stderr,
                    "nanocode: warning: unknown pet '%s', using 'cat'\n",
                    pet_val);
            config_set(cfg, "pet", "cat");
        }
    }

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
     *
     * Reads provider.type, provider.base_url, provider.port, provider.model,
     * provider.api_key from config and maps them to a ProviderConfig struct.
     *
     * Port defaulting:
     *   - claude:       443 (TLS)
     *   - openai/ollama: 11434 (Ollama default; LM Studio uses 1234)
     *
     * TLS defaulting: off for localhost regardless of type.
     * -------------------------------------------------------------------- */
    ProviderConfig provider_cfg;
    {
        const char *type_str   = config_get_str(cfg, "provider.type");
        const char *base_url   = config_get_str(cfg, "provider.base_url");
        const char *model      = config_get_str(cfg, "provider.model");
        const char *api_key    = config_get_str(cfg, "provider.api_key");
        int         port_cfg   = config_get_int(cfg, "provider.port");

        ProviderType ptype = PROVIDER_CLAUDE;
        if (type_str && strcmp(type_str, "openai") == 0)
            ptype = PROVIDER_OPENAI;
        else if (type_str && strcmp(type_str, "ollama") == 0)
            ptype = PROVIDER_OLLAMA;

        /* Determine default port for type if not overridden. */
        int port = port_cfg;
        if (port <= 0) {
            port = (ptype == PROVIDER_CLAUDE) ? 443 : 11434;
        }

        /* Disable TLS for localhost regardless of type. */
        int use_tls = 1;
        if (base_url && (strcmp(base_url, "localhost") == 0 ||
                         strncmp(base_url, "127.", 4) == 0 ||
                         strcmp(base_url, "::1") == 0)) {
            use_tls = 0;
        }
        /* Cloud Claude endpoints always use TLS. */
        if (ptype == PROVIDER_CLAUDE && use_tls == 0 && port == 443)
            use_tls = 1;

        provider_cfg.type     = ptype;
        provider_cfg.base_url = base_url ? base_url : "api.anthropic.com";
        provider_cfg.port     = port;
        provider_cfg.use_tls  = use_tls;
        provider_cfg.api_key  = api_key;
        provider_cfg.model    = model ? model : "claude-opus-4-6";
    }
    (void)provider_cfg; /* TODO: pass to agent/loop once wiring is complete */

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
     * Phase 3.6: Initialize pet and wire TUI/executor observers.
     * -------------------------------------------------------------------- */
    Pet         g_pet = {0};
    StatusBar  *g_statusbar = NULL;

    {
        const char *pet_val = config_get_str(cfg, "pet");
        PetKind kind = pet_kind_from_str(pet_val ? pet_val : "off");
        g_pet = pet_new(kind);

        /* Status bar — only when writing to a TTY in interactive mode. */
        if (!cli_json && !cli_daemon && isatty(STDERR_FILENO)) {
            g_statusbar = statusbar_new(STDERR_FILENO, &provider_cfg, 0);
            if (g_statusbar)
                statusbar_set_pet(g_statusbar, &g_pet);
        }

        /* Tool event hook — pet reacts to tool execution. */
        if (kind != PET_OFF)
            executor_set_tool_event_cb(pet_tool_event_cb, &g_pet);
    }

    /* -----------------------------------------------------------------------
     * Phase 4: Main loop.
     * -------------------------------------------------------------------- */
    g_loop = loop_new();
    if (!g_loop) {
        fprintf(stderr, "nanocode: failed to create event loop\n");
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

    /* Expand relative defaults to ~/.nanocode/ so the daemon files are not
     * created in whatever the current working directory happens to be. */
    char default_status_path[512];
    char default_sock_path[512];
    if (!status_path || !status_path[0]) {
        const char *home = getenv("HOME");
        if (home && home[0])
            snprintf(default_status_path, sizeof(default_status_path),
                     "%s/.nanocode/nanocode.status.json", home);
        else
            snprintf(default_status_path, sizeof(default_status_path),
                     "nanocode.status.json");
        status_path = default_status_path;
    }
    if (!sock_path || !sock_path[0]) {
        const char *home = getenv("HOME");
        if (home && home[0])
            snprintf(default_sock_path, sizeof(default_sock_path),
                     "%s/.nanocode/nanocode.sock", home);
        else
            snprintf(default_sock_path, sizeof(default_sock_path),
                     "nanocode.sock");
        sock_path = default_sock_path;
    }

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
            arena_free(arena);
            return 1;
        }
        printf("nanocode daemon listening on %s\n", sock_path);
    }

    printf("nanocode v0.1-dev\n");
    loop_run(g_loop);

    if (cli_daemon) {
        daemon_stop(g_daemon);
        status_file_remove(status_path);
    }

    if (g_statusbar) {
        statusbar_clear(g_statusbar);
        statusbar_free(g_statusbar);
    }

    loop_free(g_loop);
    g_loop = NULL;
    arena_free(arena);
    return 0;
}
