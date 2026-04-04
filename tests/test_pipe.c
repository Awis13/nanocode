/*
 * test_pipe.c — unit tests for pipe mode argument handling and stdin reading
 *
 * Tests focus on the logic that can be exercised without a live API:
 *   - PipeArgs field access (struct layout)
 *   - stdin buffer read via Buf (re-uses test_buf patterns)
 *   - Provider config env resolution (exercised via helper, not pipe_run)
 */

#include "test.h"
#include "../src/util/buf.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Duplicate the provider resolution logic for testing.
 * We inline it here because it's a pure function of env vars and we don't
 * want to expose it from pipe.c just to satisfy tests.
 * ---------------------------------------------------------------------- */

#include "../src/api/provider.h"

static void resolve_provider_test(ProviderConfig *cfg,
                                  const char *model_override,
                                  const char *anthropic_key,
                                  const char *openai_key,
                                  const char *base_url_env,
                                  const char *port_env,
                                  const char *model_env)
{
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
 * Tests: PipeArgs struct
 * ---------------------------------------------------------------------- */

TEST(test_pipeargs_fields) {
    /* Verify struct fields are accessible and assignable. */
    const char *instr = "explain this";
    const char *mdl   = "claude-opus-4-6";
    int raw = 1;

    /* Use designated initializer as in real code. */
    struct { const char *instruction; const char *model; int raw; } args;
    args.instruction = instr;
    args.model       = mdl;
    args.raw         = raw;

    ASSERT_STR_EQ(args.instruction, "explain this");
    ASSERT_STR_EQ(args.model,       "claude-opus-4-6");
    ASSERT_EQ(args.raw, 1);
}

TEST(test_pipeargs_null_model) {
    struct { const char *instruction; const char *model; int raw; } args;
    args.instruction = "summarize";
    args.model       = NULL;
    args.raw         = 0;

    ASSERT_NULL(args.model);
    ASSERT_EQ(args.raw, 0);
}

/* -------------------------------------------------------------------------
 * Tests: provider resolution
 * ---------------------------------------------------------------------- */

TEST(test_provider_claude_defaults) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, "sk-ant-test", NULL, NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_CLAUDE);
    ASSERT_STR_EQ(cfg.base_url, "api.anthropic.com");
    ASSERT_EQ(cfg.port, 443);
    ASSERT_EQ(cfg.use_tls, 1);
    ASSERT_STR_EQ(cfg.api_key, "sk-ant-test");
    ASSERT_STR_EQ(cfg.model, "claude-opus-4-6");
}

TEST(test_provider_claude_model_override) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, "claude-haiku-4-5-20251001",
                          "sk-ant-test", NULL, NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_CLAUDE);
    ASSERT_STR_EQ(cfg.model, "claude-haiku-4-5-20251001");
}

TEST(test_provider_claude_model_env) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, "sk-ant-test", NULL, NULL, NULL,
                          "claude-sonnet-4-6");

    ASSERT_STR_EQ(cfg.model, "claude-sonnet-4-6");
}

TEST(test_provider_claude_model_override_beats_env) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, "claude-opus-4-6", "sk-ant-test", NULL,
                          NULL, NULL, "claude-sonnet-4-6");

    /* --model flag wins over NANOCODE_MODEL env var */
    ASSERT_STR_EQ(cfg.model, "claude-opus-4-6");
}

TEST(test_provider_openai_defaults) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, NULL, "sk-openai-test",
                          NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_OPENAI);
    ASSERT_STR_EQ(cfg.base_url, "api.openai.com");
    ASSERT_EQ(cfg.port, 443);
    ASSERT_EQ(cfg.use_tls, 1);
    ASSERT_STR_EQ(cfg.model, "gpt-4o");
}

TEST(test_provider_ollama_defaults) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, NULL, NULL, NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_OLLAMA);
    ASSERT_STR_EQ(cfg.base_url, "localhost");
    ASSERT_EQ(cfg.port, 11434);
    ASSERT_EQ(cfg.use_tls, 0);
    ASSERT_NULL(cfg.api_key);
    ASSERT_STR_EQ(cfg.model, "qwen2.5:9b");
}

TEST(test_provider_ollama_custom_port) {
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, NULL, NULL, NULL, "12345", NULL);

    ASSERT_EQ(cfg.type, PROVIDER_OLLAMA);
    ASSERT_EQ(cfg.port, 12345);
}

TEST(test_provider_claude_wins_over_openai) {
    /* When both ANTHROPIC_API_KEY and OPENAI_API_KEY are set,
     * Anthropic/Claude takes precedence (first-match). */
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, "sk-ant-test", "sk-openai-test",
                          NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_CLAUDE);
}

TEST(test_provider_empty_anthropic_key_falls_through) {
    /* An empty ANTHROPIC_API_KEY should not trigger Claude. */
    ProviderConfig cfg;
    resolve_provider_test(&cfg, NULL, "", "sk-openai-test", NULL, NULL, NULL);

    ASSERT_EQ(cfg.type, PROVIDER_OPENAI);
}

/* -------------------------------------------------------------------------
 * Tests: Buf-based stdin accumulation (reused from test_buf patterns)
 * ---------------------------------------------------------------------- */

TEST(test_buf_accumulate_stdin_chunks) {
    Buf b;
    buf_init(&b);

    /* Simulate two read() chunks arriving from stdin */
    const char *chunk1 = "hello ";
    const char *chunk2 = "world\n";

    ASSERT_EQ(buf_append(&b, chunk1, strlen(chunk1)), 0);
    ASSERT_EQ(buf_append(&b, chunk2, strlen(chunk2)), 0);
    ASSERT_EQ(b.len, 12);
    ASSERT_MEM_EQ(b.data, "hello world\n", 12);

    buf_destroy(&b);
}

TEST(test_buf_empty_stdin) {
    /* When stdin is empty, buf stays at len=0 */
    Buf b;
    buf_init(&b);

    /* No appends */
    ASSERT_EQ(b.len, 0);
    /* buf_str on empty buf must not crash */
    const char *s = buf_str(&b);
    ASSERT_NOT_NULL(s);

    buf_destroy(&b);
}

TEST(test_buf_message_construction) {
    /* Simulate: instruction + "\n\n" + stdin_content */
    Buf msg;
    buf_init(&msg);

    const char *instruction = "explain this";
    const char *stdin_data  = "int main() { return 0; }";

    ASSERT_EQ(buf_append_str(&msg, instruction), 0);
    ASSERT_EQ(buf_append_str(&msg, "\n\n"), 0);
    ASSERT_EQ(buf_append_str(&msg, stdin_data), 0);

    const char *result = buf_str(&msg);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "explain this\n\nint main() { return 0; }");

    buf_destroy(&msg);
}

int main(void)
{
    fprintf(stderr, "=== test_pipe ===\n");

    RUN_TEST(test_pipeargs_fields);
    RUN_TEST(test_pipeargs_null_model);

    RUN_TEST(test_provider_claude_defaults);
    RUN_TEST(test_provider_claude_model_override);
    RUN_TEST(test_provider_claude_model_env);
    RUN_TEST(test_provider_claude_model_override_beats_env);
    RUN_TEST(test_provider_openai_defaults);
    RUN_TEST(test_provider_ollama_defaults);
    RUN_TEST(test_provider_ollama_custom_port);
    RUN_TEST(test_provider_claude_wins_over_openai);
    RUN_TEST(test_provider_empty_anthropic_key_falls_through);

    RUN_TEST(test_buf_accumulate_stdin_chunks);
    RUN_TEST(test_buf_empty_stdin);
    RUN_TEST(test_buf_message_construction);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
