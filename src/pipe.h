/*
 * pipe.h — Unix pipe mode: non-interactive stdin-to-stdout LLM processing
 *
 * Activated by --pipe flag or when stdin is not a TTY.
 * Reads all of stdin, fires a single API request, streams response to stdout.
 * No TUI, no ANSI codes, no interactive input.
 */

#ifndef PIPE_H
#define PIPE_H

/*
 * Arguments for pipe mode.
 *
 * instruction — the user-supplied prompt (CLI argument after --pipe, or
 *               remaining positional argv).  Must not be NULL.
 * model       — model name override; NULL means use env/default.
 * raw         — if non-zero, emit raw LLM output with no formatting markers.
 */
typedef struct {
    const char *instruction;
    const char *model;
    int         raw;
} PipeArgs;

/*
 * Run pipe mode synchronously.
 *
 * Provider settings are resolved from environment variables:
 *   ANTHROPIC_API_KEY  → Claude (api.anthropic.com:443, TLS)
 *   OPENAI_API_KEY     → OpenAI-compatible (api.openai.com:443, TLS)
 *   (default)          → Ollama (localhost:11434, plain)
 *
 * Additional env vars:
 *   NANOCODE_MODEL     — default model when args->model is NULL
 *   NANOCODE_BASE_URL  — override base URL (host only, no scheme)
 *   NANOCODE_PORT      — override port
 *
 * Returns 0 on success, 1 on error.
 */
int pipe_run(const PipeArgs *args);

#endif /* PIPE_H */
