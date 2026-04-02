/*
 * git.h — git integration: repo detection, status, diff, log, tools
 *
 * All allocations go through the caller-supplied Arena.
 * Uses popen() / fork+exec — no libgit2, no external dependencies.
 *
 * Sub-process calls are guarded by #ifndef __SANITIZE_ADDRESS__ to
 * prevent popen()/fork() hangs under AddressSanitizer on macOS.
 */

#ifndef GIT_H
#define GIT_H

#include "../util/arena.h"

/* Maximum file entries returned by git_status(). */
#define GIT_STATUS_MAX 64

/* A single file entry from `git status --porcelain`. */
typedef struct {
    char xy[3];       /* two-char status code (XY), NUL-terminated */
    char path[256];   /* file path */
} GitFileStatus;

/* Result of git_status(). */
typedef struct {
    GitFileStatus files[GIT_STATUS_MAX];
    int           count; /* number of valid entries */
} GitStatusResult;

/*
 * Return 1 if `cwd` (or any ancestor directory) contains a .git entry.
 * Returns 0 if not a git repo or on error.
 */
int git_detect(const char *cwd);

/*
 * Parse `git status --porcelain` output for `cwd`.
 * Returns arena-allocated GitStatusResult (possibly with count=0).
 * Returns NULL on OOM or if `cwd` is not a git repo.
 */
GitStatusResult *git_status(Arena *arena, const char *cwd);

/*
 * Run `git diff [path]` in `cwd`.  If `path` is NULL or empty, runs
 * `git diff` with no path argument (shows all unstaged changes).
 * Returns arena-allocated NUL-terminated string, or NULL on failure.
 * Returns empty string (never NULL) when the repo is clean.
 */
char *git_diff(Arena *arena, const char *cwd, const char *path);

/*
 * Run `git log --oneline -<n>` in `cwd`.
 * Returns arena-allocated NUL-terminated string, or NULL on failure.
 */
char *git_log(Arena *arena, const char *cwd, int n);

/*
 * Return the current branch name for `cwd` (trailing newline stripped).
 * Returns arena-allocated NUL-terminated string, or NULL on failure.
 */
char *git_branch(Arena *arena, const char *cwd);

/*
 * Build a compact git context block for system prompt injection.
 *
 * Format:
 *   ## Git Status
 *   ```
 *   M  src/file.c
 *   ?? new_file.c
 *   ```
 *
 *   ## Recent Commits
 *   ```
 *   abc1234 commit message
 *   ```
 *
 * Returns arena-allocated NUL-terminated string.
 * Returns NULL if `cwd` is not a git repo or has nothing to show.
 */
char *git_context_summary(Arena *arena, const char *cwd);

/*
 * Register git tools with the executor:
 *   "git_commit"        — stage files and create a commit
 *   "git_branch_create" — create (and optionally checkout) a branch
 */
void git_tools_register(void);

#endif /* GIT_H */
