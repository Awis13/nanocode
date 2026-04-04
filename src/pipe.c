/*
 * pipe.c — Unix pipe mode implementation
 *
 * Reads all of stdin into a buffer, constructs a single LLM request combining
 * the piped content with the user's instruction, streams the response to
 * stdout.  No TUI, no ANSI escape codes.  Errors go to stderr.
 *
 * Provider selection (first match wins):
 *   ANTHROPIC_API_KEY set  → PROVIDER_CLAUDE  (api.anthropic.com:443)
 *   OPENAI_API_KEY set     → PROVIDER_OPENAI  (api.openai.com:443)
 *   default                → PROVIDER_OLLAMA  (localhost:11434)
 */

#include "pipe.h"
#include "core/loop.h"
#include "api/provider.h"
#include "util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum stdin size accepted (64 MB). */
#define PIPE_STDIN_MAX (64 * 1024 * 1024)

/* Read chunk size for stdin. */
#define READ_CHUNK 4096

/* -------------------------------------------------------------------------
 * Stream state shared between callbacks and main loop
 * ---------------------------------------------------------------------- */

typedef struct {
    Loop *loop;
    int   error;
    int   done;
} PipeState;

static void on_token(const char *token, size_t len, void *ctx)
{
    (void)ctx;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
}

static void on_done(int error, const char *stop_reason, void *ctx)
{
    (void)stop_reason;
    PipeState *s = ctx;
    s->error = error;
    s->done  = 1;
    loop_stop(s->loop);
}

/* -------------------------------------------------------------------------
 * Stdin reader
 * ---------------------------------------------------------------------- */

static int read_stdin(Buf *out)
{
    char chunk[READ_CHUNK];
    size_t total = 0;

    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), stdin);
        if (n == 0) {
            if (feof(stdin))
                break;
            /* ferror */
            fprintf(stderr, "nanocode: error reading stdin\n");
            return -1;
        }
        total += n;
        if (total > PIPE_STDIN_MAX) {
            fprintf(stderr, "nanocode: stdin exceeds %d MB limit\n",
                    PIPE_STDIN_MAX / (1024 * 1024));
            return -1;
        }
        if (buf_append(out, chunk, n) < 0) {
            fprintf(stderr, "nanocode: out of memory reading stdin\n");
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Provider config resolution from environment
 * ---------------------------------------------------------------------- */

static void resolve_provider(ProviderConfig *cfg, const char *model_override)
{
    const char *anthropic_key = getenv("ANTHROPIC_API_KEY");
    const char *openai_key    = getenv("OPENAI_API_KEY");
    const char *base_url_env  = getenv("NANOCODE_BASE_URL");
    const char *port_env      = getenv("NANOCODE_PORT");
    const char *model_env     = getenv("NANOCODE_MODEL");

    memset(cfg, 0, sizeof(*cfg));

    if (anthropic_key && anthropic_key[0]) {
        cfg->type    = PROVIDER_CLAUDE;
        cfg->base_url = base_url_env ? base_url_env : "api.anthropic.com";
        cfg->port    = port_env ? atoi(port_env) : 443;
        cfg->use_tls = (cfg->port == 443 && !base_url_env) ? 1
                     : (!port_env && !base_url_env) ? 1 : 0;
        cfg->api_key = anthropic_key;
        cfg->model   = model_override ? model_override
                     : (model_env    ? model_env : "claude-opus-4-6");
    } else if (openai_key && openai_key[0]) {
        cfg->type    = PROVIDER_OPENAI;
        cfg->base_url = base_url_env ? base_url_env : "api.openai.com";
        cfg->port    = port_env ? atoi(port_env) : 443;
        cfg->use_tls = (cfg->port == 443 && !base_url_env) ? 1
                     : (!port_env && !base_url_env) ? 1 : 0;
        cfg->api_key = openai_key;
        cfg->model   = model_override ? model_override
                     : (model_env    ? model_env : "gpt-4o");
    } else {
        /* Default: Ollama on localhost */
        cfg->type    = PROVIDER_OLLAMA;
        cfg->base_url = base_url_env ? base_url_env : "localhost";
        cfg->port    = port_env ? atoi(port_env) : 11434;
        cfg->use_tls = 0;
        cfg->api_key = NULL;
        cfg->model   = model_override ? model_override
                     : (model_env    ? model_env : "qwen2.5:9b");
    }
}

/* -------------------------------------------------------------------------
 * pipe_run — public entry point
 * ---------------------------------------------------------------------- */

int pipe_run(const PipeArgs *args)
{
    /* Read all stdin first (before opening network connections). */
    Buf stdin_buf;
    buf_init(&stdin_buf);
    if (read_stdin(&stdin_buf) < 0) {
        buf_destroy(&stdin_buf);
        return 1;
    }

    /* Build user message: instruction + newlines + stdin content.
     * If stdin was empty, send only the instruction. */
    Buf msg_buf;
    buf_init(&msg_buf);
    if (buf_append_str(&msg_buf, args->instruction) < 0 ||
        (stdin_buf.len > 0 &&
         (buf_append_str(&msg_buf, "\n\n") < 0 ||
          buf_append(&msg_buf, stdin_buf.data, stdin_buf.len) < 0))) {
        fprintf(stderr, "nanocode: out of memory building request\n");
        buf_destroy(&stdin_buf);
        buf_destroy(&msg_buf);
        return 1;
    }
    buf_destroy(&stdin_buf);

    const char *user_content = buf_str(&msg_buf);
    if (!user_content) {
        buf_destroy(&msg_buf);
        return 1;
    }

    /* Resolve provider settings. */
    ProviderConfig cfg;
    resolve_provider(&cfg, args->model);

    if (!args->raw) {
        fprintf(stderr, "nanocode: using %s model %s\n",
                cfg.type == PROVIDER_CLAUDE ? "claude" :
                cfg.type == PROVIDER_OPENAI ? "openai" : "ollama",
                cfg.model);
    }

    /* Set up event loop and provider. */
    Loop *loop = loop_new();
    if (!loop) {
        fprintf(stderr, "nanocode: failed to create event loop\n");
        buf_destroy(&msg_buf);
        return 1;
    }

    Provider *provider = provider_new(loop, &cfg);
    if (!provider) {
        fprintf(stderr, "nanocode: failed to create provider\n");
        loop_free(loop);
        buf_destroy(&msg_buf);
        return 1;
    }

    Message msgs[] = {
        { "user", user_content },
    };

    PipeState state = { loop, 0, 0 };

    int r = provider_stream(provider, msgs, 1, on_token, NULL, on_done, &state);
    if (r < 0) {
        fprintf(stderr, "nanocode: failed to start stream\n");
        provider_free(provider);
        loop_free(loop);
        buf_destroy(&msg_buf);
        return 1;
    }

    /* Run event loop until stream completes. */
    while (!state.done) {
        if (loop_step(loop, 5000) < 0)
            break;
    }

    /* Ensure stdout ends with a newline when not in raw mode. */
    if (!args->raw)
        fputc('\n', stdout);

    provider_free(provider);
    loop_free(loop);
    buf_destroy(&msg_buf);

    return state.error ? 1 : 0;
}
