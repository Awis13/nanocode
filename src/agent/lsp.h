/*
 * lsp.h — Language Server Protocol client (minimal coding-agent subset)
 *
 * Spawns a language server subprocess and communicates via JSON-RPC 2.0
 * over stdio (Content-Length framing), using the shared jsonrpc layer.
 *
 * Supported lifecycle:
 *   initialize / initialized    — handshake on lsp_start()
 *   textDocument/didOpen        — notify server of file open
 *   textDocument/didChange      — notify server after edits
 *   textDocument/publishDiagnostics — collected from server
 *   shutdown / exit             — graceful stop on lsp_stop()
 *
 * Agent integration: after write_file or edit_file, call lsp_did_change(),
 * then lsp_collect_diagnostics(). If non-NULL, prepend the result to the
 * next user message so the AI can auto-fix compilation errors.
 *
 * Language servers auto-detected by extension via lsp_detect_server():
 *   .c / .cc / .cpp / .h / .hpp  → clangd
 *   .py                           → pylsp
 *   .ts / .tsx / .js / .jsx       → typescript-language-server --stdio
 *   .rs                           → rust-analyzer
 *   .go                           → gopls
 */

#ifndef LSP_H
#define LSP_H

#include "jsonrpc.h"
#include "../util/arena.h"

/* Maximum message length of a single diagnostic. */
#define LSP_DIAG_MSG_MAX  512

/* Maximum diagnostics collected per lsp_collect_diagnostics() call. */
#define LSP_DIAG_MAX      32

/* Maximum URI / path length. */
#define LSP_PATH_MAX      512

/* Per-diagnostic data extracted from publishDiagnostics. */
typedef struct {
    int  severity;              /* 1=error, 2=warning, 3=info, 4=hint */
    int  line;                  /* 0-based line (LSP convention) */
    int  col;                   /* 0-based character (LSP convention) */
    char uri[LSP_PATH_MAX];     /* document URI from the params */
    char message[LSP_DIAG_MSG_MAX];
} LspDiag;

/* Persistent state for one language-server connection. */
typedef struct {
    JsonRpc rpc;
    int     initialized;  /* 1 after handshake complete */
    int     seq;          /* textDocument version counter */
} LspClient;

/*
 * Zero-initialise `client`.  Call before any other lsp_* function.
 */
void lsp_init(LspClient *client);

/*
 * Spawn `cmd` as the language server (e.g. "clangd" or
 * "typescript-language-server --stdio").
 * `root_uri` is the project root as a file:// URI (e.g. "file:///home/user/proj").
 * Performs the initialize / initialized handshake.
 * Returns 0 on success, -1 on failure.
 */
int lsp_start(LspClient *client, const char *cmd, const char *root_uri);

/*
 * Send textDocument/didOpen.
 * `language_id` — e.g. "c", "python", "typescript".
 * Returns 0 on success, -1 on error (server not started or write failure).
 */
int lsp_did_open(LspClient *client, const char *uri,
                 const char *language_id, const char *text);

/*
 * Send textDocument/didChange (full-text sync).
 * Returns 0 on success, -1 on error.
 */
int lsp_did_change(LspClient *client, const char *uri, const char *text);

/*
 * Poll for textDocument/publishDiagnostics notifications for up to
 * `timeout_ms` milliseconds.
 * Formats any diagnostics found as an arena-allocated NUL-terminated string:
 *   [compiler: error at /path/file.c:11:5: use of undeclared identifier 'x']
 *   [compiler: warning at /path/file.c:20:1: unused variable 'y']
 *   ...
 * Returns the formatted string, or NULL if no diagnostics were received.
 */
char *lsp_collect_diagnostics(LspClient *client, Arena *arena, int timeout_ms);

/*
 * Detect the appropriate language-server command for the given file path.
 * Returns a static string (e.g. "clangd") or NULL for unknown extensions.
 * The string may contain spaces (e.g. "typescript-language-server --stdio")
 * and is suitable for passing directly to lsp_start().
 */
const char *lsp_detect_server(const char *filepath);

/*
 * Detect the LSP languageId from a file path (e.g. "c", "python").
 * Returns "plaintext" for unknown extensions.
 */
const char *lsp_detect_language(const char *filepath);

/*
 * Send shutdown + exit notifications and close the connection.
 * Safe to call on an uninitialised or already-stopped client.
 */
void lsp_stop(LspClient *client);

#endif /* LSP_H */
