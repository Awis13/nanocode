/*
 * diff_confirm.h — interactive diff confirmation for write_file / edit_file
 *
 * Provides a fileops_confirm_cb implementation that:
 *   1. Renders a (possibly truncated) unified diff to the terminal
 *   2. Prompts: [y]es / [n]o / [e]dit / [a]ll
 *   3. Returns > 0 (apply), 0 (reject); tracks apply-all via DiffConfirmCtx.auto_apply
 */

#ifndef DIFF_CONFIRM_H
#define DIFF_CONFIRM_H

/*
 * Context passed to diff_confirm_cb via fileops_set_confirm_cb.
 *
 *   auto_apply — if non-zero, silently apply every change (like pressing 'a')
 *   fd_out     — file descriptor for output (usually STDOUT_FILENO)
 *   fd_in      — file descriptor for input  (usually STDIN_FILENO)
 */
typedef struct {
    int auto_apply;
    int fd_out;
    int fd_in;
} DiffConfirmCtx;

/*
 * fileops_confirm_cb-compatible function.
 * Show the diff for (old_content -> new_content) for `path` and ask the user.
 *
 * Return values follow the fileops_confirm_cb contract:
 *   > 0  apply this change (using *out_replacement if set by [e]dit)
 *     0  reject this change
 *
 * When the user chooses [e]dit and confirms, *out_replacement is set to a
 * heap-allocated edited string; fileops writes it instead of new_content and
 * then free()s it.  The callback uses DiffConfirmCtx.auto_apply to track
 * apply-all state rather than returning < 0 and nulling the global callback.
 */
int diff_confirm_cb(const char *path,
                    const char *old_content,
                    const char *new_content,
                    char **out_replacement,
                    void *ctx);

#endif /* DIFF_CONFIRM_H */
