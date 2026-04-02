/*
 * test_commands.c — unit tests for the slash-command system
 */

#include "test.h"
#include "../src/tui/commands.h"
#include "../src/util/arena.h"
#include "../src/agent/conversation.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------
 * Helpers: capture output via a pipe
 * ---------------------------------------------------------------------- */

static int g_pipe_r = -1;
static int g_pipe_w = -1;

static void pipe_open(void)
{
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); return; }
    g_pipe_r = fds[0];
    g_pipe_w = fds[1];
    fcntl(g_pipe_r, F_SETFL, O_NONBLOCK);
}

static void pipe_close(void)
{
    if (g_pipe_r >= 0) { close(g_pipe_r); g_pipe_r = -1; }
    if (g_pipe_w >= 0) { close(g_pipe_w); g_pipe_w = -1; }
}

static const char *pipe_read_all(void)
{
    static char buf[4096];
    close(g_pipe_w); g_pipe_w = -1;

    ssize_t total = 0, n;
    while ((n = read(g_pipe_r, buf + total,
                     sizeof(buf) - 1 - (size_t)total)) > 0)
        total += n;
    buf[total] = '\0';
    return buf;
}

static CmdContext make_ctx(void)
{
    CmdContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd_out = g_pipe_w;
    return ctx;
}

/* -------------------------------------------------------------------------
 * cmd_is_command
 * ---------------------------------------------------------------------- */

TEST(test_is_command_slash_prefix) {
    ASSERT_EQ(cmd_is_command("/help"), 1);
    ASSERT_EQ(cmd_is_command("/model foo"), 1);
    ASSERT_EQ(cmd_is_command("/"), 1);
}

TEST(test_is_command_no_slash) {
    ASSERT_EQ(cmd_is_command("hello"), 0);
    ASSERT_EQ(cmd_is_command(""),      0);
    ASSERT_EQ(cmd_is_command(NULL),    0);
}

/* -------------------------------------------------------------------------
 * cmd_dispatch — routing
 * ---------------------------------------------------------------------- */

TEST(test_dispatch_not_a_command) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ASSERT_EQ(cmd_dispatch("hello world", &ctx), -1);
    pipe_close();
}

TEST(test_dispatch_null_input) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ASSERT_EQ(cmd_dispatch(NULL, &ctx), -1);
    pipe_close();
}

TEST(test_dispatch_unknown_command) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ASSERT_EQ(cmd_dispatch("/xyzzy", &ctx), 1);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "xyzzy") != NULL);
    ASSERT_TRUE(strstr(out, "/help") != NULL);
    pipe_close();
}

TEST(test_dispatch_bare_slash_shows_help) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ASSERT_EQ(cmd_dispatch("/", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "Available commands") != NULL);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /help
 * ---------------------------------------------------------------------- */

TEST(test_help_lists_all_commands) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ASSERT_EQ(cmd_dispatch("/help", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "/help")    != NULL);
    ASSERT_TRUE(strstr(out, "/model")   != NULL);
    ASSERT_TRUE(strstr(out, "/clear")   != NULL);
    ASSERT_TRUE(strstr(out, "/compact") != NULL);
    ASSERT_TRUE(strstr(out, "/cost")    != NULL);
    ASSERT_TRUE(strstr(out, "/init")    != NULL);
    ASSERT_TRUE(strstr(out, "/diff")    != NULL);
    ASSERT_TRUE(strstr(out, "/apply")   != NULL);
    ASSERT_TRUE(strstr(out, "/reject")  != NULL);
    ASSERT_TRUE(strstr(out, "/mcp")     != NULL);
    pipe_close();
}

TEST(test_help_null_ctx) {
    pipe_open();
    int devnull = open("/dev/null", O_WRONLY);
    CmdContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd_out = devnull;
    ASSERT_EQ(cmd_dispatch("/help", &ctx), 0);
    close(devnull);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /model
 * ---------------------------------------------------------------------- */

TEST(test_model_set) {
    pipe_open();
    CmdContext ctx = make_ctx();
    char *model_name = NULL;
    ctx.model = &model_name;
    ASSERT_EQ(cmd_dispatch("/model claude-opus-4-6", &ctx), 0);
    ASSERT_NOT_NULL(model_name);
    ASSERT_STR_EQ(model_name, "claude-opus-4-6");
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "claude-opus-4-6") != NULL);
    pipe_close();
}

TEST(test_model_print_current) {
    pipe_open();
    CmdContext ctx = make_ctx();
    char *model_name = (char *)"qwen2.5:9b";
    ctx.model = &model_name;
    ASSERT_EQ(cmd_dispatch("/model", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "qwen2.5:9b") != NULL);
    pipe_close();
}

TEST(test_model_no_ctx) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.model = NULL;
    ASSERT_EQ(cmd_dispatch("/model newmodel", &ctx), 1);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "error") != NULL);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /cost
 * ---------------------------------------------------------------------- */

TEST(test_cost_shows_token_counts) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.in_tok  = 12450;
    ctx.out_tok = 892;
    ASSERT_EQ(cmd_dispatch("/cost", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "12450") != NULL);
    ASSERT_TRUE(strstr(out, "892")   != NULL);
    pipe_close();
}

TEST(test_cost_zero_ctx) {
    pipe_open();
    int devnull = open("/dev/null", O_WRONLY);
    CmdContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd_out = devnull;
    ASSERT_EQ(cmd_dispatch("/cost", &ctx), 0);
    close(devnull);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /clear
 * ---------------------------------------------------------------------- */

TEST(test_clear_no_conv) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.conv = NULL;
    ASSERT_EQ(cmd_dispatch("/clear", &ctx), 1);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "error") != NULL);
    pipe_close();
}

TEST(test_clear_confirm_y) {
    pipe_open();

    int in_fds[2];
    pipe(in_fds);
    write(in_fds[1], "y\n", 2);
    close(in_fds[1]);

    Arena        *a    = arena_new(65536);
    Conversation *conv = conv_new(a);
    conv_add(conv, "user",      "hello");
    conv_add(conv, "assistant", "hi");
    ASSERT_EQ(conv->nturn, 2);

    CmdContext ctx = make_ctx();
    ctx.conv  = conv;
    ctx.fd_in = in_fds[0];

    ASSERT_EQ(cmd_dispatch("/clear", &ctx), 0);
    ASSERT_EQ(conv->nturn, 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "cleared") != NULL);

    close(in_fds[0]);
    arena_free(a);
    pipe_close();
}

TEST(test_clear_confirm_n) {
    pipe_open();

    int in_fds[2];
    pipe(in_fds);
    write(in_fds[1], "n\n", 2);
    close(in_fds[1]);

    Arena        *a    = arena_new(65536);
    Conversation *conv = conv_new(a);
    conv_add(conv, "user", "hello");
    ASSERT_EQ(conv->nturn, 1);

    CmdContext ctx = make_ctx();
    ctx.conv  = conv;
    ctx.fd_in = in_fds[0];

    ASSERT_EQ(cmd_dispatch("/clear", &ctx), 0);
    ASSERT_EQ(conv->nturn, 1);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "Cancelled") != NULL);

    close(in_fds[0]);
    arena_free(a);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /compact
 * ---------------------------------------------------------------------- */

TEST(test_compact_reduces_turns) {
    pipe_open();

    Arena        *a    = arena_new(65536);
    Conversation *conv = conv_new(a);
    for (int i = 0; i < 10; i++)
        conv_add(conv, i % 2 == 0 ? "user" : "assistant", "turn content");
    ASSERT_EQ(conv->nturn, 10);

    CmdContext ctx = make_ctx();
    ctx.conv = conv;

    ASSERT_EQ(cmd_dispatch("/compact", &ctx), 0);
    /* 4 kept + 1 omission marker = 5 */
    ASSERT_EQ(conv->nturn, 5);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "Compacted") != NULL);

    arena_free(a);
    pipe_close();
}

TEST(test_compact_already_small) {
    pipe_open();

    Arena        *a    = arena_new(65536);
    Conversation *conv = conv_new(a);
    conv_add(conv, "user",      "hi");
    conv_add(conv, "assistant", "hello");
    ASSERT_EQ(conv->nturn, 2);

    CmdContext ctx = make_ctx();
    ctx.conv = conv;

    ASSERT_EQ(cmd_dispatch("/compact", &ctx), 0);
    ASSERT_EQ(conv->nturn, 2);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "already compact") != NULL);

    arena_free(a);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /mcp
 * ---------------------------------------------------------------------- */

TEST(test_mcp_no_servers) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.nmcp      = 0;
    ctx.mcp_names = NULL;
    ASSERT_EQ(cmd_dispatch("/mcp", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "No MCP") != NULL);
    pipe_close();
}

TEST(test_mcp_lists_servers) {
    pipe_open();
    CmdContext ctx = make_ctx();
    const char *names[] = { "filesystem", "github" };
    ctx.mcp_names = names;
    ctx.nmcp      = 2;
    ASSERT_EQ(cmd_dispatch("/mcp", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "filesystem") != NULL);
    ASSERT_TRUE(strstr(out, "github")     != NULL);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * /diff, /apply, /reject — no-sandbox path
 * ---------------------------------------------------------------------- */

TEST(test_diff_no_sandbox) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.sandbox = NULL;
    ASSERT_EQ(cmd_dispatch("/diff", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "No pending") != NULL);
    pipe_close();
}

TEST(test_apply_no_sandbox) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.sandbox = NULL;
    ASSERT_EQ(cmd_dispatch("/apply", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "No pending") != NULL);
    pipe_close();
}

TEST(test_reject_no_sandbox) {
    pipe_open();
    CmdContext ctx = make_ctx();
    ctx.sandbox = NULL;
    ASSERT_EQ(cmd_dispatch("/reject", &ctx), 0);
    const char *out = pipe_read_all();
    ASSERT_TRUE(strstr(out, "No pending") != NULL);
    pipe_close();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_commands ===\n");

    RUN_TEST(test_is_command_slash_prefix);
    RUN_TEST(test_is_command_no_slash);

    RUN_TEST(test_dispatch_not_a_command);
    RUN_TEST(test_dispatch_null_input);
    RUN_TEST(test_dispatch_unknown_command);
    RUN_TEST(test_dispatch_bare_slash_shows_help);

    RUN_TEST(test_help_lists_all_commands);
    RUN_TEST(test_help_null_ctx);

    RUN_TEST(test_model_set);
    RUN_TEST(test_model_print_current);
    RUN_TEST(test_model_no_ctx);

    RUN_TEST(test_cost_shows_token_counts);
    RUN_TEST(test_cost_zero_ctx);

    RUN_TEST(test_clear_no_conv);
    RUN_TEST(test_clear_confirm_y);
    RUN_TEST(test_clear_confirm_n);

    RUN_TEST(test_compact_reduces_turns);
    RUN_TEST(test_compact_already_small);

    RUN_TEST(test_mcp_no_servers);
    RUN_TEST(test_mcp_lists_servers);

    RUN_TEST(test_diff_no_sandbox);
    RUN_TEST(test_apply_no_sandbox);
    RUN_TEST(test_reject_no_sandbox);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
