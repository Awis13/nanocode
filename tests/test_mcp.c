/*
 * test_mcp.c — unit tests for the MCP client module (CMP-150)
 *
 * Tests cover:
 *   - Null/error handling for all public API functions
 *   - mcp_client_free(NULL) is safe
 *   - mcp_tools_register() with no config file does not crash
 *   - mcp_tools_register() with a valid config but non-existent server gracefully fails
 *   - Config JSON parsing helper (via mcp_tools_register with temp file)
 *   - Integration tests with mock_mcp_server.py (skipped if python3 unavailable)
 */

#include "test.h"
#include "../src/agent/mcp.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

/* Path to the mock server script relative to project root. */
#define MOCK_SERVER "tests/mock_mcp_server.py"

/* Check if python3 is available and the mock server exists. */
static int python3_available(void)
{
    FILE *fp = popen("python3 --version 2>/dev/null", "r");
    if (!fp) return 0;
    char buf[64] = "";
    fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    if (strncmp(buf, "Python", 6) != 0) return 0;
    /* Check that mock server exists. */
    return access(MOCK_SERVER, R_OK) == 0;
}

/* =========================================================================
 * Null-safety / error-path tests (always run, no subprocess)
 * ====================================================================== */

TEST(test_client_new_null_cmd)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    MCPClient *c = mcp_client_new(a, NULL, NULL);
    ASSERT_NULL(c);
    mcp_client_free(c); /* must not crash */

    arena_free(a);
}

TEST(test_client_new_null_argv)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    MCPClient *c = mcp_client_new(a, "no-such-binary", NULL);
    ASSERT_NULL(c);

    arena_free(a);
}

TEST(test_client_free_null)
{
    /* Must be safe to call with NULL. */
    mcp_client_free(NULL);
}

TEST(test_client_init_null)
{
    int rc = mcp_client_init(NULL);
    ASSERT_TRUE(rc == -1);
}

TEST(test_list_tools_null_client)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ToolList *tl = mcp_list_tools(NULL, a);
    ASSERT_NULL(tl);

    arena_free(a);
}

TEST(test_list_tools_null_arena)
{
    ToolList *tl = mcp_list_tools(NULL, NULL);
    ASSERT_NULL(tl);
}

TEST(test_call_tool_null_client)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ToolResult r = mcp_call_tool(NULL, a, "tool", "{}");
    ASSERT_TRUE(r.error != 0);
    ASSERT_NOT_NULL(r.content);

    arena_free(a);
}

TEST(test_call_tool_null_name)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ToolResult r = mcp_call_tool(NULL, a, NULL, "{}");
    ASSERT_TRUE(r.error != 0);

    arena_free(a);
}

TEST(test_call_tool_null_arena)
{
    /* With null arena, must not crash (though result is undefined). */
    ToolResult r = mcp_call_tool(NULL, NULL, "tool", "{}");
    ASSERT_TRUE(r.error != 0);
}

/* =========================================================================
 * mcp_client_new with a non-existent binary
 * ====================================================================== */

TEST(test_client_new_bad_binary)
{
    /*
     * mcp_client_new succeeds (fork succeeds), but the child will _exit(127).
     * Under ASan the fork is suppressed and we get NULL directly.
     * Either outcome is acceptable — the important thing is no crash.
     */
#ifdef __SANITIZE_ADDRESS__
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);
    char *const argv[] = {"__no_such_mcp_binary__", NULL};
    MCPClient *c = mcp_client_new(a, "__no_such_mcp_binary__", argv);
    ASSERT_NULL(c); /* ASan always returns NULL */
    arena_free(a);
#else
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);
    char *const argv[] = {"__no_such_mcp_binary__", NULL};
    MCPClient *c = mcp_client_new(a, "__no_such_mcp_binary__", argv);
    /* fork succeeds; client is non-NULL; init will fail since child exits. */
    if (c) {
        int rc = mcp_client_init(c);
        ASSERT_TRUE(rc == -1); /* child exited immediately */
        mcp_client_free(c);
    }
    arena_free(a);
#endif
}

/* =========================================================================
 * mcp_tools_register: no config file present
 * ====================================================================== */

TEST(test_tools_register_no_config)
{
    /*
     * mcp_tools_register() reads $HOME/.nanocode/mcp.json.  If it doesn't
     * exist the function must return silently without crashing or registering
     * any tools.
     */
    tool_registry_reset();

    /* Temporarily redirect HOME to a dir without mcp.json. */
    char *orig_home = getenv("HOME");
    setenv("HOME", "/tmp", 1);

    mcp_tools_register();

    const char *names[64];
    int count = tool_list_names(names, 64);
    ASSERT_EQ(count, 0);

    /* Restore HOME. */
    if (orig_home) setenv("HOME", orig_home, 1);

    tool_registry_reset();
}

/* =========================================================================
 * mcp_tools_register: config with non-existent server
 * ====================================================================== */

TEST(test_tools_register_bad_server)
{
    /*
     * Write a mcp.json pointing at a non-existent server.
     * mcp_tools_register must handle it gracefully (no crash, no tools).
     */
    tool_registry_reset();

    /* Create a temp dir with a mcp.json. */
    char tmpdir[] = "/tmp/test_mcp_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        /* Can't create tmpdir — skip. */
        tool_registry_reset();
        return;
    }

    /* Create .nanocode dir. */
    char nanodir[256];
    snprintf(nanodir, sizeof(nanodir), "%s/.nanocode", tmpdir);
    mkdir(nanodir, 0700);

    /* Write mcp.json. */
    char mcppath[512];
    snprintf(mcppath, sizeof(mcppath), "%s/mcp.json", nanodir);
    FILE *fp = fopen(mcppath, "w");
    if (fp) {
        fprintf(fp,
            "{\"servers\":[{\"name\":\"bad\",\"command\":\"__no_such_mcp_binary__\",\"args\":[]}]}");
        fclose(fp);
    }

    char *orig_home = getenv("HOME");
    setenv("HOME", tmpdir, 1);

    mcp_tools_register(); /* must not crash */

    int count = tool_list_names(NULL, 0);
    ASSERT_EQ(count, 0); /* bad server should not register any tools */

    if (orig_home) setenv("HOME", orig_home, 1);
    tool_registry_reset();

    /* Cleanup (best-effort). */
    remove(mcppath);
    rmdir(nanodir);
    rmdir(tmpdir);
}

/* =========================================================================
 * Integration tests — require python3 + mock_mcp_server.py
 * ====================================================================== */

/*
 * Spawn the mock server, run initialize + tools/list, verify the
 * discovered tool has the expected name.
 */
TEST(test_integration_list_tools)
{
#ifdef __SANITIZE_ADDRESS__
    /* fork suppressed under ASan — skip. */
    return;
#else
    if (!python3_available()) return; /* skip */

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *const argv[] = {"python3", (char *)MOCK_SERVER, NULL};
    MCPClient *c = mcp_client_new(a, "python3", argv);
    ASSERT_NOT_NULL(c);
    if (!c) { arena_free(a); return; }

    int rc = mcp_client_init(c);
    ASSERT_EQ(rc, 0);
    if (rc != 0) { mcp_client_free(c); arena_free(a); return; }

    ToolList *tl = mcp_list_tools(c, a);
    ASSERT_NOT_NULL(tl);
    if (!tl) { mcp_client_free(c); arena_free(a); return; }

    ASSERT_TRUE(tl->count >= 1);
    ASSERT_STR_EQ(tl->tools[0].name, "mock_echo");
    ASSERT_TRUE(strlen(tl->tools[0].description) > 0);
    ASSERT_TRUE(strlen(tl->tools[0].schema_json) > 0);
    /* schema_json should be a JSON object containing the tool name. */
    ASSERT_TRUE(strstr(tl->tools[0].schema_json, "mock_echo") != NULL);

    mcp_client_free(c);
    arena_free(a);
#endif
}

/*
 * Spawn the mock server and call the mock_echo tool.
 */
TEST(test_integration_call_tool)
{
#ifdef __SANITIZE_ADDRESS__
    return;
#else
    if (!python3_available()) return; /* skip */

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *const argv[] = {"python3", (char *)MOCK_SERVER, NULL};
    MCPClient *c = mcp_client_new(a, "python3", argv);
    ASSERT_NOT_NULL(c);
    if (!c) { arena_free(a); return; }

    int rc = mcp_client_init(c);
    ASSERT_EQ(rc, 0);
    if (rc != 0) { mcp_client_free(c); arena_free(a); return; }

    ToolResult r = mcp_call_tool(c, a, "mock_echo", "{\"text\":\"hello mcp\"}");
    ASSERT_EQ(r.error, 0);
    ASSERT_NOT_NULL(r.content);
    ASSERT_STR_EQ(r.content, "hello mcp");

    mcp_client_free(c);
    arena_free(a);
#endif
}

/*
 * Call an unknown tool — expect an error ToolResult (not a crash).
 */
TEST(test_integration_call_unknown_tool)
{
#ifdef __SANITIZE_ADDRESS__
    return;
#else
    if (!python3_available()) return; /* skip */

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *const argv[] = {"python3", (char *)MOCK_SERVER, NULL};
    MCPClient *c = mcp_client_new(a, "python3", argv);
    ASSERT_NOT_NULL(c);
    if (!c) { arena_free(a); return; }

    int rc = mcp_client_init(c);
    ASSERT_EQ(rc, 0);
    if (rc != 0) { mcp_client_free(c); arena_free(a); return; }

    ToolResult r = mcp_call_tool(c, a, "no_such_tool", "{}");
    ASSERT_TRUE(r.error != 0);
    ASSERT_NOT_NULL(r.content);

    mcp_client_free(c);
    arena_free(a);
#endif
}

/*
 * Multiple sequential tool calls on the same client.
 */
TEST(test_integration_multiple_calls)
{
#ifdef __SANITIZE_ADDRESS__
    return;
#else
    if (!python3_available()) return; /* skip */

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *const argv[] = {"python3", (char *)MOCK_SERVER, NULL};
    MCPClient *c = mcp_client_new(a, "python3", argv);
    ASSERT_NOT_NULL(c);
    if (!c) { arena_free(a); return; }

    ASSERT_EQ(mcp_client_init(c), 0);

    ToolResult r1 = mcp_call_tool(c, a, "mock_echo", "{\"text\":\"first\"}");
    ToolResult r2 = mcp_call_tool(c, a, "mock_echo", "{\"text\":\"second\"}");

    ASSERT_EQ(r1.error, 0);
    ASSERT_EQ(r2.error, 0);
    ASSERT_STR_EQ(r1.content, "first");
    ASSERT_STR_EQ(r2.content, "second");

    mcp_client_free(c);
    arena_free(a);
#endif
}

/*
 * mcp_call_tool on a client that is not yet initialized should return error.
 */
TEST(test_call_tool_uninitialized)
{
#ifdef __SANITIZE_ADDRESS__
    return;
#else
    if (!python3_available()) return;

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *const argv[] = {"python3", (char *)MOCK_SERVER, NULL};
    MCPClient *c = mcp_client_new(a, "python3", argv);
    ASSERT_NOT_NULL(c);
    if (!c) { arena_free(a); return; }

    /* Do NOT call mcp_client_init. */
    ToolResult r = mcp_call_tool(c, a, "mock_echo", "{\"text\":\"x\"}");
    ASSERT_TRUE(r.error != 0);

    mcp_client_free(c);
    arena_free(a);
#endif
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_mcp ===\n");

    RUN_TEST(test_client_new_null_cmd);
    RUN_TEST(test_client_new_null_argv);
    RUN_TEST(test_client_free_null);
    RUN_TEST(test_client_init_null);
    RUN_TEST(test_list_tools_null_client);
    RUN_TEST(test_list_tools_null_arena);
    RUN_TEST(test_call_tool_null_client);
    RUN_TEST(test_call_tool_null_name);
    RUN_TEST(test_call_tool_null_arena);
    RUN_TEST(test_client_new_bad_binary);
    RUN_TEST(test_tools_register_no_config);
    RUN_TEST(test_tools_register_bad_server);
    RUN_TEST(test_integration_list_tools);
    RUN_TEST(test_integration_call_tool);
    RUN_TEST(test_integration_call_unknown_tool);
    RUN_TEST(test_integration_multiple_calls);
    RUN_TEST(test_call_tool_uninitialized);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
