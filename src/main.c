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
#include "../include/sandbox.h"
#include "../include/status_file.h"
#include "../include/daemon.h"
#include "../src/core/loop.h"
#include "../src/util/arena.h"
#include "../src/util/duration.h"
#include "../src/tools/fileops.h"
#include "../src/tools/bash.h"

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
     * --sandbox                  force sandbox.enabled = true
     * --sandbox-profile <name>   override sandbox.profile
     * --timeout <duration>       session timeout (e.g. 30m, 1h, 90s)
     * --daemon                   run as Unix socket daemon
     * -------------------------------------------------------------------- */
    int         cli_sandbox         = 0;
    int         cli_daemon          = 0;
    const char *cli_sandbox_profile = NULL;
    char        cli_timeout_arg[64] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            cli_daemon = 1;
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

    printf("nanocode v0.1-dev\n");
    loop_run(g_loop);

    loop_free(g_loop);
    g_loop = NULL;
    arena_free(arena);
    return 0;
}
