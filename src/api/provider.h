/*
 * provider.h — AI provider abstraction
 *
 * Supports:
 *   PROVIDER_CLAUDE  — Anthropic Messages API (POST /v1/messages)
 *   PROVIDER_OPENAI  — OpenAI-compatible API  (POST /v1/chat/completions)
 *   PROVIDER_OLLAMA  — Ollama native API (POST /api/chat), NDJSON stream,
 *                      supports think:false to disable extended reasoning
 */

#ifndef PROVIDER_H
#define PROVIDER_H

#include <stddef.h>
#include "../core/loop.h"
#include "message.h"

typedef enum {
    PROVIDER_CLAUDE = 0,
    PROVIDER_OPENAI,
    PROVIDER_OLLAMA,      /* Ollama native /api/chat — supports think:false */
} ProviderType;

typedef struct {
    ProviderType  type;
    const char   *base_url;  /* e.g. "api.anthropic.com" or "localhost" */
    int           port;      /* 443 for cloud, 11434 for Ollama */
    int           use_tls;   /* 1 for cloud endpoints, 0 for localhost */
    const char   *api_key;   /* may be NULL for local models */
    const char   *model;         /* e.g. "claude-opus-4-6" or "qwen2.5:9b" */
    int           thinking_budget; /* >0 enables extended thinking (Claude only) */
} ProviderConfig;

/*
 * Called for each streamed text token.
 * `token` is NOT NUL-terminated; `len` bytes are valid.
 */
typedef void (*provider_token_cb)(const char *token, size_t len, void *ctx);

/*
 * Called when a complete tool_use block has been streamed.
 * `id`    — tool_use_id (NUL-terminated)
 * `name`  — tool name  (NUL-terminated)
 * `input` — accumulated JSON input string (NUL-terminated)
 */
typedef void (*provider_tool_cb)(const char *id, const char *name,
                                 const char *input, void *ctx);

/*
 * Called once when the stream is complete.
 * `error`       — 0 on success, non-zero on HTTP/connection error
 * `stop_reason` — NUL-terminated stop reason string from the provider
 *                 (e.g. "end_turn", "max_tokens", "tool_use"), or NULL on
 *                 error or when not applicable (non-Claude providers).
 */
typedef void (*provider_done_cb)(int error, const char *stop_reason, void *ctx);

typedef struct Provider Provider;

/* Create a provider bound to `loop`. Returns NULL on failure. */
Provider *provider_new(Loop *loop, const ProviderConfig *cfg);

/* Destroy provider (does NOT free the loop). */
void      provider_free(Provider *p);

/*
 * Returns 1 if the model name indicates extended thinking support.
 * Currently recognises claude-opus-4 and claude-sonnet-4 families.
 * Returns 0 for all other models (OpenAI, Ollama, older Claude).
 */
int provider_model_supports_thinking(const char *model);

/*
 * Start a streaming request.
 * `msgs`    — array of messages
 * `nmsg`    — message count
 * `on_token` — called for each text token as it arrives
 * `on_tool`  — called for each completed tool_use block
 * `on_done`  — called on completion or error
 * `ctx`      — passed to all callbacks
 *
 * Returns 0 if the request was started, -1 on immediate error.
 */
int provider_stream(Provider *p,
                    const Message *msgs, int nmsg,
                    provider_token_cb on_token,
                    provider_tool_cb  on_tool,
                    provider_done_cb  on_done,
                    void *ctx);

#endif /* PROVIDER_H */
