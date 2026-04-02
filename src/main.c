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
#include "../include/sandbox.h"
#include "../include/status_file.h"
#include "../include/daemon.h"
#include "../src/core/loop.h"
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
    const char *cli_sandbox_profile = NULL;
    char        cli_timeout_arg[64] = "";

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
        }
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
        const char *cfg_mode = config_get_str(cfg, "sandbox.mode");
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

    loop_free(g_loop);
    g_loop = NULL;
    arena_free(arena);
    return 0;
}
