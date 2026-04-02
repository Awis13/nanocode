/*
 * editor.h — open files in the user's terminal editor
 *
 * Implements the $VISUAL → $EDITOR → vi fallback chain used by nanocode's
 * `edit` CLI subcommand and the TUI 'e' keybind.
 */

#ifndef EDITOR_H
#define EDITOR_H

/*
 * editor_open — fork/exec the user's preferred editor for path at line.
 *
 * Editor resolution order: $VISUAL → $EDITOR → "vi".
 * Line-jump flags per editor family:
 *   vim / nvim / nano / vi : +<line>
 *   emacs / emacsclient    : +<line>:0
 *   other                  : file only (line ignored)
 *
 * sandbox_allowed_paths: colon-separated absolute path prefixes from the
 * sandbox config, or NULL/"" to skip the check.  If the path does not start
 * with any allowed prefix the call prints an error to stderr and returns -1
 * without exec'ing.
 *
 * TODO: replace the inline prefix check with sandbox_check_path() once
 * CMP-198 is merged and that symbol is exported from include/sandbox.h.
 *
 * Returns  0 when the editor process exits with status 0.
 * Returns -1 on sandbox denial, fork failure, or non-zero editor exit.
 */
int editor_open(const char *path, int line, const char *sandbox_allowed_paths);

/*
 * editor_open_from_ref — parse a "path:line" reference and call editor_open.
 *
 * ref may be in any of these forms:
 *   "src/foo.c:42"   → path="src/foo.c", line=42
 *   "src/foo.c"      → path="src/foo.c", line=0  (no line jump)
 *
 * sandbox_allowed_paths is forwarded to editor_open unchanged.
 *
 * Returns  0 on success, -1 on parse error or editor_open failure.
 */
int editor_open_from_ref(const char *ref, const char *sandbox_allowed_paths);

#endif /* EDITOR_H */
