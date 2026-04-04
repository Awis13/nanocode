/*
 * history.h — conversation persistence: auto-save, resume, search, export
 *
 * Storage: ~/.nanocode/conversations/<timestamp>-<title>.jsonl
 * Format:  one JSON object per line
 *          {"role":"user","content":"...","tokens":N,"timestamp":"ISO8601"}
 *
 * Typical call sequence:
 *   1. history_open(first_user_message)  → hx
 *   2. history_append_turn(hx, ...)      after each turn
 *   3. history_close(hx)                 at session end
 *
 * Resume:  history_list_recent() → user picks → history_load()
 * Search:  history_search(term, fd_out)
 * Export:  history_export(conv, path, fd_out)
 */

#ifndef HISTORY_H
#define HISTORY_H

#include "agent/conversation.h"  /* Conversation, Turn — found via -Isrc */
#include "util/arena.h"           /* Arena              — found via -Isrc */

/* Maximum number of recent conversations to list. */
#define HISTORY_LIST_MAX 10

/* Opaque handle for the currently open history file. */
typedef struct HistoryCtx HistoryCtx;

/*
 * Open (or create) a JSONL history file for a new conversation.
 * `title` is typically the first user message; it is sanitized to a safe
 * filename fragment (first 50 chars, non-alphanumeric replaced with '-').
 * Creates ~/.nanocode/conversations/ if it does not exist.
 * Returns NULL on failure — all subsequent calls on a NULL ctx are no-ops
 * (graceful degradation if the directory is not writable).
 */
HistoryCtx *history_open(const char *title);

/*
 * Append one turn to the open history file.
 * Writes: {"role":"...","content":"...","tokens":N,"timestamp":"..."}
 * `tokens` may be 0 if token counting is not available.
 * Safe to call with hx=NULL (no-op).
 */
void history_append_turn(HistoryCtx *hx, const char *role,
                         const char *content, int tokens, int is_tool);

/* Flush and close the history context. Safe to call with NULL. */
void history_close(HistoryCtx *hx);

/* Return the absolute path of the open history file. NULL if hx=NULL. */
const char *history_path(const HistoryCtx *hx);

/*
 * List up to HISTORY_LIST_MAX recent conversation files sorted by
 * modification time (newest first).
 * Fills paths[0..n-1] with malloc'd strings; caller must free() each.
 * Returns the number of entries found (0 if directory does not exist).
 */
int history_list_recent(char **paths, int maxn);

/*
 * Load a JSONL history file into an arena-allocated Conversation.
 * Each line {"role":"...","content":"...",...} is added via conv_add().
 * Returns NULL on I/O or parse failure.
 */
Conversation *history_load(Arena *arena, const char *path);

/*
 * Stream-search all history files for `term` (case-insensitive).
 * Reads files line by line — does NOT load full files into memory.
 * Prints each match as:  <filename>:<linenum>: <content>
 * with one line of context before and after the match.
 * Returns the total number of matching lines found.
 */
int history_search(const char *term, int fd_out);

/*
 * Export a Conversation to Markdown.
 * If `path` is non-NULL, creates/overwrites that file.
 * If `path` is NULL, writes to `fd_out` (pass STDOUT_FILENO for stdout).
 * System and tool turns are skipped.
 * Returns 0 on success, -1 on failure.
 */
int history_export(const Conversation *conv, const char *path, int fd_out);

#endif /* HISTORY_H */
