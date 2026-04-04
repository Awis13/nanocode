/*
 * oneshot.h — helpers for one-shot CLI mode (-c/--command)
 */

#ifndef ONESHOT_H
#define ONESHOT_H

#include <stddef.h>

typedef struct {
    int         enabled;
    const char *command;
    int         auto_apply;
} OneShotFlags;

/*
 * Parse one-shot related flags at argv[*index].
 *
 * Recognized flags:
 *   -c <instruction>
 *   --command <instruction>
 *   --auto-apply
 *
 * Returns:
 *   1  if argv[*index] was handled
 *   0  if argv[*index] is not a one-shot flag
 *  -1  on parse error (err is filled when provided)
 *
 * On success, *index may be advanced to consume a flag argument.
 */
int oneshot_parse_arg(int argc, char **argv, int *index,
                      OneShotFlags *flags, char *err, size_t errcap);

/* Returns 1 when one-shot tool calls should be executed automatically. */
int oneshot_should_auto_apply(const OneShotFlags *flags);

/* One-shot mode exits with 0 on success and 1 on any error/timeout. */
int oneshot_exit_code(int stream_error, int timed_out);

#endif /* ONESHOT_H */
