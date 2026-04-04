/*
 * oneshot.c — one-shot CLI mode (-c/--command) helpers
 *
 * Implements flag parsing and exit-code logic for non-interactive
 * single-command execution.
 */

#include "../../include/oneshot.h"

#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * oneshot_parse_arg
 * ---------------------------------------------------------------------- */

int oneshot_parse_arg(int argc, char **argv, int *index,
                      OneShotFlags *flags, char *err, size_t errcap)
{
    const char *arg = argv[*index];

    if (strcmp(arg, "--auto-apply") == 0) {
        flags->auto_apply = 1;
        return 1;
    }

    if (strcmp(arg, "-c") == 0 || strcmp(arg, "--command") == 0) {
        if (*index + 1 >= argc) {
            if (err && errcap > 0)
                snprintf(err, errcap,
                         "nanocode: %s requires an instruction argument", arg);
            return -1;
        }
        flags->command = argv[++(*index)];
        flags->enabled = 1;
        return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * oneshot_should_auto_apply
 * ---------------------------------------------------------------------- */

int oneshot_should_auto_apply(const OneShotFlags *flags)
{
    if (!flags)
        return 0;
    return flags->auto_apply;
}

/* -------------------------------------------------------------------------
 * oneshot_exit_code
 * ---------------------------------------------------------------------- */

int oneshot_exit_code(int stream_error, int timed_out)
{
    return (stream_error || timed_out) ? 1 : 0;
}
