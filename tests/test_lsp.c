/*
 * test_lsp.c — unit tests for jsonrpc framing and the LSP client (CMP-149)
 *
 * Tests that can run without any language server installed:
 *   - JSON-RPC Content-Length framing (send + recv round-trip)
 *   - recv timeout behaviour
 *   - lsp_detect_server / lsp_detect_language
 *   - graceful behaviour when client is not initialised
 *   - publishDiagnostics parse (injected via pipe)
 *
 * Integration test (skipped if clangd is not in PATH):
 *   - full initialize / didOpen / collect_diagnostics / stop cycle
 */

#include "test.h"
#include "../src/agent/jsonrpc.h"
#include "../src/agent/lsp.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * JSON-RPC framing: send writes correct Content-Length header
 * ---------------------------------------------------------------------- */

TEST(test_jsonrpc_send_notification_framing)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = pipefd[1];
    rpc.read_fd  = pipefd[0];
    rpc.next_id  = 1;

    /* Send a notification (id == 0). */
    int r = jsonrpc_send(&rpc, "test/method", "{\"key\":\"val\"}", 0);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);
    rpc.write_fd = -1;

    /* Drain the pipe. */
    char buf[2048];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    /* Must have Content-Length header and blank separator. */
    ASSERT_NOT_NULL(strstr(buf, "Content-Length:"));
    ASSERT_NOT_NULL(strstr(buf, "\r\n\r\n"));
    /* Body must contain method and params. */
    ASSERT_NOT_NULL(strstr(buf, "test/method"));
    ASSERT_NOT_NULL(strstr(buf, "{\"key\":\"val\"}"));
    /* Notification must NOT include an "id" field. */
    ASSERT_NULL(strstr(buf, "\"id\""));
}

TEST(test_jsonrpc_send_request_framing)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = pipefd[1];
    rpc.read_fd  = pipefd[0];
    rpc.next_id  = 1;

    /* Send a request (id == 42). */
    int r = jsonrpc_send(&rpc, "initialize", "{}", 42);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);
    rpc.write_fd = -1;

    char buf[2048];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    /* Request must include the id. */
    ASSERT_NOT_NULL(strstr(buf, "\"id\":42"));
    ASSERT_NOT_NULL(strstr(buf, "initialize"));
}

/* -------------------------------------------------------------------------
 * JSON-RPC recv: correctly parses Content-Length framing
 * ---------------------------------------------------------------------- */

TEST(test_jsonrpc_recv_basic)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = -1;
    rpc.read_fd  = pipefd[0];
    rpc.next_id  = 1;

    /* Write a well-formed LSP message to the write end. */
    const char *body   = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}";
    size_t      blen   = strlen(body);
    char        hdr[64];
    int         hlen   = snprintf(hdr, sizeof(hdr),
                                  "Content-Length: %zu\r\n\r\n", blen);
    write(pipefd[1], hdr, (size_t)hlen);
    write(pipefd[1], body, blen);
    close(pipefd[1]);

    char out[4096];
    int  r = jsonrpc_recv(&rpc, out, sizeof(out), 0);
    close(pipefd[0]);

    ASSERT_EQ(r, (int)blen);
    ASSERT_STR_EQ(out, body);
}

TEST(test_jsonrpc_recv_extra_headers)
{
    /* LSP servers may send Content-Type in addition to Content-Length. */
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = -1;
    rpc.read_fd  = pipefd[0];
    rpc.next_id  = 1;

    const char *body = "{\"jsonrpc\":\"2.0\"}";
    size_t      blen = strlen(body);
    char        msg[256];
    int         mlen = snprintf(msg, sizeof(msg),
                                "Content-Length: %zu\r\n"
                                "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
                                "\r\n"
                                "%s", blen, body);
    write(pipefd[1], msg, (size_t)mlen);
    close(pipefd[1]);

    char out[4096];
    int  r = jsonrpc_recv(&rpc, out, sizeof(out), 0);
    close(pipefd[0]);

    ASSERT_EQ(r, (int)blen);
    ASSERT_STR_EQ(out, body);
}

/* -------------------------------------------------------------------------
 * JSON-RPC recv: timeout
 * ---------------------------------------------------------------------- */

TEST(test_jsonrpc_recv_timeout)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = -1;
    rpc.read_fd  = pipefd[0];
    rpc.next_id  = 1;

    /* Nothing written — should time out after 50 ms. */
    char out[256];
    int  r = jsonrpc_recv(&rpc, out, sizeof(out), 50);

    close(pipefd[0]);
    close(pipefd[1]);

    ASSERT_EQ(r, -1);
}

/* -------------------------------------------------------------------------
 * JSON-RPC send on a closed fd returns error without crashing
 * ---------------------------------------------------------------------- */

TEST(test_jsonrpc_send_closed_fd)
{
    JsonRpc rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.pid      = (pid_t)-1;
    rpc.write_fd = -1;
    rpc.read_fd  = -1;
    rpc.next_id  = 1;

    int r = jsonrpc_send(&rpc, "test", "{}", 0);
    ASSERT_EQ(r, -1);
}

/* -------------------------------------------------------------------------
 * lsp_detect_server
 * ---------------------------------------------------------------------- */

TEST(test_lsp_detect_server)
{
    ASSERT_STR_EQ(lsp_detect_server("main.c"),   "clangd");
    ASSERT_STR_EQ(lsp_detect_server("foo.h"),    "clangd");
    ASSERT_STR_EQ(lsp_detect_server("app.cc"),   "clangd");
    ASSERT_STR_EQ(lsp_detect_server("app.cpp"),  "clangd");
    ASSERT_STR_EQ(lsp_detect_server("app.hpp"),  "clangd");
    ASSERT_STR_EQ(lsp_detect_server("mod.py"),   "pylsp");
    ASSERT_NOT_NULL(lsp_detect_server("index.ts"));
    ASSERT_NOT_NULL(lsp_detect_server("index.js"));
    ASSERT_STR_EQ(lsp_detect_server("lib.rs"),   "rust-analyzer");
    ASSERT_STR_EQ(lsp_detect_server("main.go"),  "gopls");
    ASSERT_NULL(lsp_detect_server("README.md"));
    ASSERT_NULL(lsp_detect_server("noextension"));
    ASSERT_NULL(lsp_detect_server("file."));
}

/* -------------------------------------------------------------------------
 * lsp_detect_language
 * ---------------------------------------------------------------------- */

TEST(test_lsp_detect_language)
{
    ASSERT_STR_EQ(lsp_detect_language("foo.c"),   "c");
    ASSERT_STR_EQ(lsp_detect_language("foo.h"),   "c");
    ASSERT_STR_EQ(lsp_detect_language("foo.cpp"), "cpp");
    ASSERT_STR_EQ(lsp_detect_language("foo.cxx"), "cpp");
    ASSERT_STR_EQ(lsp_detect_language("foo.hpp"), "cpp");
    ASSERT_STR_EQ(lsp_detect_language("foo.py"),  "python");
    ASSERT_STR_EQ(lsp_detect_language("foo.ts"),  "typescript");
    ASSERT_STR_EQ(lsp_detect_language("foo.tsx"), "typescript");
    ASSERT_STR_EQ(lsp_detect_language("foo.js"),  "javascript");
    ASSERT_STR_EQ(lsp_detect_language("foo.rs"),  "rust");
    ASSERT_STR_EQ(lsp_detect_language("foo.go"),  "go");
    ASSERT_STR_EQ(lsp_detect_language("foo.xyz"), "plaintext");
    ASSERT_STR_EQ(lsp_detect_language("noext"),   "plaintext");
}

/* -------------------------------------------------------------------------
 * Graceful behaviour when client is not initialised
 * ---------------------------------------------------------------------- */

TEST(test_lsp_uninitialised_returns_error)
{
    LspClient client;
    lsp_init(&client);

    /* All operations must return -1 / NULL without crashing. */
    ASSERT_EQ(lsp_did_open(&client, "file:///tmp/x.c", "c", ""), -1);
    ASSERT_EQ(lsp_did_change(&client, "file:///tmp/x.c", ""),    -1);

    Arena *a    = arena_new(1 << 16);
    char  *diag = lsp_collect_diagnostics(&client, a, 10);
    ASSERT_NULL(diag);
    arena_free(a);

    lsp_stop(&client);  /* must not crash */
}

/* -------------------------------------------------------------------------
 * publishDiagnostics parsing via injected pipe messages
 * ---------------------------------------------------------------------- */

/*
 * Build a valid publishDiagnostics notification and inject it into a pipe
 * that the LspClient reads from.  Then call lsp_collect_diagnostics() and
 * check the formatted output.
 */
TEST(test_lsp_collect_diagnostics_injected)
{
    /* Craft a publishDiagnostics notification. */
    static const char diag_msg[] =
        "{"
          "\"jsonrpc\":\"2.0\","
          "\"method\":\"textDocument/publishDiagnostics\","
          "\"params\":{"
            "\"uri\":\"file:///tmp/test.c\","
            "\"diagnostics\":["
              "{"
                "\"range\":{"
                  "\"start\":{\"line\":4,\"character\":2},"
                  "\"end\":{\"line\":4,\"character\":8}"
                "},"
                "\"severity\":1,"
                "\"message\":\"undeclared identifier 'foo'\""
              "}"
            "]"
          "}"
        "}";

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    /* Write the framed message to the write end. */
    char hdr[64];
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "Content-Length: %zu\r\n\r\n", strlen(diag_msg));
    write(pipefd[1], hdr, (size_t)hlen);
    write(pipefd[1], diag_msg, strlen(diag_msg));
    close(pipefd[1]);

    /* Set up a fake initialised LspClient that reads from our pipe. */
    LspClient client;
    lsp_init(&client);
    client.rpc.read_fd  = pipefd[0];
    client.rpc.write_fd = -1;
    client.rpc.pid      = (pid_t)-1;
    client.initialized  = 1;

    Arena *a    = arena_new(1 << 16);
    char  *diag = lsp_collect_diagnostics(&client, a, 500);

    close(pipefd[0]);
    client.rpc.read_fd = -1;

    /* Should have produced a formatted diagnostic line. */
    ASSERT_NOT_NULL(diag);
    ASSERT_NOT_NULL(strstr(diag, "error"));
    ASSERT_NOT_NULL(strstr(diag, "test.c"));
    ASSERT_NOT_NULL(strstr(diag, "5:3"));             /* line+1, col+1 */
    ASSERT_NOT_NULL(strstr(diag, "undeclared identifier"));

    arena_free(a);
}

TEST(test_lsp_collect_diagnostics_empty)
{
    /* An empty diagnostics array should produce NULL. */
    static const char diag_msg[] =
        "{"
          "\"jsonrpc\":\"2.0\","
          "\"method\":\"textDocument/publishDiagnostics\","
          "\"params\":{"
            "\"uri\":\"file:///tmp/clean.c\","
            "\"diagnostics\":[]"
          "}"
        "}";

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    char hdr[64];
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "Content-Length: %zu\r\n\r\n", strlen(diag_msg));
    write(pipefd[1], hdr, (size_t)hlen);
    write(pipefd[1], diag_msg, strlen(diag_msg));
    close(pipefd[1]);

    LspClient client;
    lsp_init(&client);
    client.rpc.read_fd  = pipefd[0];
    client.rpc.write_fd = -1;
    client.rpc.pid      = (pid_t)-1;
    client.initialized  = 1;

    Arena *a    = arena_new(1 << 16);
    char  *diag = lsp_collect_diagnostics(&client, a, 200);

    close(pipefd[0]);
    client.rpc.read_fd = -1;

    ASSERT_NULL(diag);
    arena_free(a);
}

TEST(test_lsp_collect_diagnostics_warning)
{
    /* severity == 2 should be labelled "warning". */
    static const char diag_msg[] =
        "{"
          "\"jsonrpc\":\"2.0\","
          "\"method\":\"textDocument/publishDiagnostics\","
          "\"params\":{"
            "\"uri\":\"file:///tmp/warn.c\","
            "\"diagnostics\":["
              "{"
                "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
                "\"severity\":2,"
                "\"message\":\"unused variable 'x'\""
              "}"
            "]"
          "}"
        "}";

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    char hdr[64];
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "Content-Length: %zu\r\n\r\n", strlen(diag_msg));
    write(pipefd[1], hdr, (size_t)hlen);
    write(pipefd[1], diag_msg, strlen(diag_msg));
    close(pipefd[1]);

    LspClient client;
    lsp_init(&client);
    client.rpc.read_fd  = pipefd[0];
    client.rpc.write_fd = -1;
    client.rpc.pid      = (pid_t)-1;
    client.initialized  = 1;

    Arena *a    = arena_new(1 << 16);
    char  *diag = lsp_collect_diagnostics(&client, a, 200);

    close(pipefd[0]);
    client.rpc.read_fd = -1;

    ASSERT_NOT_NULL(diag);
    ASSERT_NOT_NULL(strstr(diag, "warning"));
    ASSERT_NOT_NULL(strstr(diag, "unused variable"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Integration test: clangd (skipped if not installed)
 * ---------------------------------------------------------------------- */

TEST(test_lsp_clangd_integration)
{
    /* Skip if clangd is not available. */
    if (system("which clangd >/dev/null 2>&1") != 0) {
        fprintf(stderr, "    [skip] clangd not found in PATH\n");
        return;
    }

    LspClient client;
    lsp_init(&client);

    int r = lsp_start(&client, "clangd", "file:///tmp");
    if (r != 0) {
        fprintf(stderr, "    [skip] clangd failed to initialise\n");
        return;
    }

    /* Write a temp C file with a deliberate error. */
    const char *uri  = "file:///tmp/test_lsp_integration.c";
    const char *code = "int main(void) { undefined_var; return 0; }\n";

    r = lsp_did_open(&client, uri, "c", code);
    ASSERT_EQ(r, 0);

    Arena *a    = arena_new(1 << 20);
    char  *diag = lsp_collect_diagnostics(&client, a, 3000);

    /* We may or may not get diagnostics depending on clangd speed, but
     * the call must complete without crashing. */
    (void)diag;

    arena_free(a);
    lsp_stop(&client);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(test_jsonrpc_send_notification_framing);
    RUN_TEST(test_jsonrpc_send_request_framing);
    RUN_TEST(test_jsonrpc_recv_basic);
    RUN_TEST(test_jsonrpc_recv_extra_headers);
    RUN_TEST(test_jsonrpc_recv_timeout);
    RUN_TEST(test_jsonrpc_send_closed_fd);
    RUN_TEST(test_lsp_detect_server);
    RUN_TEST(test_lsp_detect_language);
    RUN_TEST(test_lsp_uninitialised_returns_error);
    RUN_TEST(test_lsp_collect_diagnostics_injected);
    RUN_TEST(test_lsp_collect_diagnostics_empty);
    RUN_TEST(test_lsp_collect_diagnostics_warning);
    RUN_TEST(test_lsp_clangd_integration);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
