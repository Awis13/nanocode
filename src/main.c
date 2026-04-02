/*
 * nanocode — a zero-dependency AI coding agent in C
 *
 * Entry point. Handles signals, argument parsing, and top-level lifecycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "../include/config.h"
#include "../include/sandbox.h"
#include "../src/util/arena.h"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* -----------------------------------------------------------------------
     * Phase 1: Parse CLI flags that must be applied before / after config.
     *
     * --sandbox                  force sandbox.enabled = true
     * --sandbox-profile <name>   override sandbox.profile
     * -------------------------------------------------------------------- */
    int         cli_sandbox         = 0;       /* 1 if --sandbox seen          */
    const char *cli_sandbox_profile = NULL;    /* value from --sandbox-profile */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sandbox") == 0) {
            cli_sandbox = 1;
        } else if (strcmp(argv[i], "--sandbox-profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "nanocode: --sandbox-profile requires a name "
                        "(strict | permissive | custom)\n");
                return 1;
            }
            cli_sandbox_profile = argv[++i];
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
     * Phase 4: Main loop (stub — event loop + TUI wired in later milestones).
     * -------------------------------------------------------------------- */
    printf("nanocode v0.1-dev\n");
    printf("TODO: event loop, API client, TUI\n");

    arena_free(arena);
    return 0;
}
