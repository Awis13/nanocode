/*
 * diff_sandbox.h — patch buffer, cumulative diff, confirm/reject
 *
 * Safe-by-default file writes: accumulate all changes as patches,
 * show a unified diff, and require explicit confirmation before applying.
 *
 * Usage:
 *   DiffSandbox *sb = diff_sandbox_new(arena);
 *   diff_sandbox_queue(sb, "/path/to/file", old_content, new_content);
 *   diff_sandbox_show(sb);          // print unified diff to stdout
 *   if (user_confirms())
 *       diff_sandbox_apply(sb);     // write all files atomically
 *   else
 *       diff_sandbox_discard(sb);   // no files touched
 */

#ifndef DIFF_SANDBOX_H
#define DIFF_SANDBOX_H

#include "../util/arena.h"

/* Opaque sandbox handle. */
typedef struct DiffSandbox DiffSandbox;

/*
 * Create a new diff sandbox backed by `arena`.
 * All patch metadata and string copies are arena-allocated.
 */
DiffSandbox *diff_sandbox_new(Arena *arena);

/*
 * Queue a file change.
 *   path        — filesystem path to write
 *   old_content — current file content, or NULL if the file is new
 *   new_content — desired new content (must not be NULL)
 *
 * Ignored if the sandbox has already been discarded.
 */
void diff_sandbox_queue(DiffSandbox *sb, const char *path,
                        const char *old_content, const char *new_content);

/*
 * Print a unified diff of all queued changes to stdout.
 * Uses ANSI colors when stdout is a terminal.
 */
void diff_sandbox_show(DiffSandbox *sb);

/*
 * Atomically write all queued changes to disk.
 *
 * Implementation:
 *   Phase 1 — write each file to a temporary path (original + ".nanosandbox")
 *   Phase 2 — rename all temporary files to their final paths
 *
 * If Phase 1 fails for any file, all temporary files are deleted and -1 is
 * returned; no original files are touched.
 *
 * Returns 0 on success, -1 on error.
 */
int diff_sandbox_apply(DiffSandbox *sb);

/*
 * Discard all queued changes without writing anything.
 * No files are modified. The sandbox must not be used after this call.
 */
void diff_sandbox_discard(DiffSandbox *sb);

/*
 * Print a unified diff for a single in-memory change to `fd`.
 * Useful for inline confirmation prompts before writing a file.
 *   path        — label shown in the diff header
 *   old_content — current content, or NULL for a new file
 *   new_content — proposed new content
 *   fd          — output file descriptor (e.g. STDOUT_FILENO or a pipe write-end)
 *   arena       — scratch allocator (diff metadata is arena-allocated)
 */
void diff_sandbox_show_one(const char *path,
                           const char *old_content,
                           const char *new_content,
                           int fd,
                           Arena *arena);

#endif /* DIFF_SANDBOX_H */
