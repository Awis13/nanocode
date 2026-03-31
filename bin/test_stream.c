/*
 * bin/test_stream.c — Phase 1 validation binary
 *
 * Streams a response from Ollama (localhost:11434) using the provider
 * abstraction, printing tokens to stdout as they arrive.
 *
 * Usage:
 *   ./test_stream [model]
 *
 * Defaults to "qwen2.5:9b" if no model is specified.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "../src/core/loop.h"
#include "../src/api/provider.h"

static volatile sig_atomic_t g_done = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_done = 1;
}

static void on_token(const char *token, size_t len, void *ctx)
{
    (void)ctx;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
}

static void on_done(int error, void *ctx)
{
    Loop *loop = ctx;
    if (error)
        fprintf(stderr, "\n[stream error]\n");
    else
        printf("\n[stream complete]\n");
    fflush(stdout);
    loop_stop(loop);
    g_done = 1;  /* unblock the main loop */
}

int main(int argc, char **argv)
{
    const char *model = (argc > 1) ? argv[1] : "qwen2.5:9b";

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("Connecting to Ollama at localhost:11434 with model '%s'...\n", model);
    fflush(stdout);

    Loop *loop = loop_new();
    if (!loop) {
        fprintf(stderr, "test_stream: failed to create event loop\n");
        return 1;
    }

    ProviderConfig cfg = {
        .type     = PROVIDER_OPENAI,
        .base_url = "localhost",
        .port     = 11434,
        .use_tls  = 0,
        .api_key  = NULL,
        .model    = model,
    };

    Provider *provider = provider_new(loop, &cfg);
    if (!provider) {
        fprintf(stderr, "test_stream: failed to create provider\n");
        loop_free(loop);
        return 1;
    }

    /* /no_think disables extended thinking in qwen3/qwen3.5 models */
    Message msgs[] = {
        { "user", "Say hello in exactly one short sentence. /no_think" },
    };

    int r = provider_stream(provider, msgs, 1, on_token, on_done, loop);
    if (r < 0) {
        fprintf(stderr, "test_stream: failed to start stream\n");
        provider_free(provider);
        loop_free(loop);
        return 1;
    }

    /* Run event loop until stream completes or signal received */
    while (!g_done) {
        if (loop_step(loop, 5000) < 0)
            break;
        /* Check if the provider finished without the loop stopping */
        if (g_done)
            break;
    }

    provider_free(provider);
    loop_free(loop);
    return 0;
}
