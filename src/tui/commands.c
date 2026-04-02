/*
 * commands.c — slash-command parser, registry, and handlers
 *
 * Command line format:  /name [arg1 [arg2 ...]]
 * Tokens split on ASCII whitespace; at most CMD_MAX_ARGC tokens total.
 */

#include "commands.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------- */

#define CMD_MAX_ARGC  16   /* max tokens per command line (name + args) */
#define CMD_BUF       256  /* scratch buffer size for output lines */

/* -------------------------------------------------------------------------
 * Handler forward declarations
 * ---------------------------------------------------------------------- */

typedef int (*cmd_fn)(int argc, const char **argv, CmdContext *ctx);

static int cmd_help(int argc, const char **argv, CmdContext *ctx);
static int cmd_model(int argc, const char **argv, CmdContext *ctx);
static int cmd_clear(int argc, const char **argv, CmdContext *ctx);
static int cmd_compact(int argc, const char **argv, CmdContext *ctx);
static int cmd_cost(int argc, const char **argv, CmdContext *ctx);
static int cmd_init(int argc, const char **argv, CmdContext *ctx);
static int cmd_diff(int argc, const char **argv, CmdContext *ctx);
static int cmd_apply(int argc, const char **argv, CmdContext *ctx);
static int cmd_reject(int argc, const char **argv, CmdContext *ctx);
static int cmd_mcp(int argc, const char **argv, CmdContext *ctx);

/* -------------------------------------------------------------------------
 * Static registry
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *description;
    cmd_fn      fn;
} CmdEntry;

static const CmdEntry g_cmds[] = {
    { "help",    "show this help message",                   cmd_help    },
    { "model",   "switch model for this session",            cmd_model   },
    { "clear",   "clear conversation history (y/n prompt)",  cmd_clear   },
    { "compact", "summarize conversation to save context",   cmd_compact },
    { "cost",    "show token usage for this session",        cmd_cost    },
    { "init",    "generate .nanocode.md project file",       cmd_init    },
    { "diff",    "show pending sandbox changes",             cmd_diff    },
    { "apply",   "apply pending sandbox changes to disk",    cmd_apply   },
    { "reject",  "discard pending sandbox changes",          cmd_reject  },
    { "mcp",     "list configured MCP servers and status",   cmd_mcp     },
};

#define NCMDS ((int)(sizeof(g_cmds) / sizeof(g_cmds[0])))

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int ofd(CmdContext *ctx)
{
    if (!ctx || ctx->fd_out <= 0) return STDOUT_FILENO;
    return ctx->fd_out;
}

static int ifd(CmdContext *ctx)
{
    if (!ctx || ctx->fd_in <= 0) return STDIN_FILENO;
    return ctx->fd_in;
}

/* Duplicate model text into arena when available, else heap. */
static char *cmd_dup_model(CmdContext *ctx, const char *model)
{
    if (!model) return NULL;
    size_t len = strlen(model) + 1;
    char *dst = NULL;
    if (ctx && ctx->conv && ctx->conv->arena)
        dst = arena_alloc(ctx->conv->arena, len);
    else
        dst = malloc(len);
    if (!dst) return NULL;
    memcpy(dst, model, len);
    return dst;
}

/* Write a NUL-terminated string to fd. */
static void cmd_write(int fd, const char *s)
{
    size_t len     = strlen(s);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, s + written, len - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int cmd_is_command(const char *input)
{
    return input && input[0] == '/';
}

int cmd_dispatch(const char *input, CmdContext *ctx)
{
    if (!input || input[0] != '/') return -1;

    /* Tokenize a copy of input (skip the leading '/'). */
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", input + 1);

    const char *argv[CMD_MAX_ARGC];
    int argc = 0;

    char *p = copy;
    while (*p && argc < CMD_MAX_ARGC) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }

    if (argc == 0) {
        argv[argc++] = "help";
    }

    for (int i = 0; i < NCMDS; i++) {
        if (strcmp(argv[0], g_cmds[i].name) == 0)
            return g_cmds[i].fn(argc, argv, ctx);
    }

    char msg[CMD_BUF];
    snprintf(msg, sizeof(msg),
             "Unknown command: /%s  (type /help for a list of commands)\n",
             argv[0]);
    cmd_write(ofd(ctx), msg);
    return 1;
}

/* -------------------------------------------------------------------------
 * Handlers
 * ---------------------------------------------------------------------- */

static int cmd_help(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd = ofd(ctx);
    cmd_write(fd, "Available commands:\n");
    for (int i = 0; i < NCMDS; i++) {
        char line[CMD_BUF];
        snprintf(line, sizeof(line), "  /%-10s  %s\n",
                 g_cmds[i].name, g_cmds[i].description);
        cmd_write(fd, line);
    }
    return 0;
}

static int cmd_model(int argc, const char **argv, CmdContext *ctx)
{
    int fd = ofd(ctx);
    if (argc < 2) {
        const char *cur = (ctx && ctx->model && *ctx->model) ? *ctx->model : "(none)";
        char line[CMD_BUF];
        snprintf(line, sizeof(line), "Current model: %s\n", cur);
        cmd_write(fd, line);
        return 0;
    }
    if (!ctx || !ctx->model) {
        cmd_write(fd, "error: model context unavailable\n");
        return 1;
    }
    char *model_copy = cmd_dup_model(ctx, argv[1]);
    if (!model_copy) {
        cmd_write(fd, "error: out of memory\n");
        return 1;
    }
    *ctx->model = model_copy;
    char line[CMD_BUF];
    snprintf(line, sizeof(line), "Model set to: %s\n", argv[1]);
    cmd_write(fd, line);
    return 0;
}

static int cmd_clear(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd_o = ofd(ctx);
    if (!ctx || !ctx->conv) {
        cmd_write(fd_o, "error: no active conversation\n");
        return 1;
    }
    cmd_write(fd_o, "Clear conversation history? [y/N] ");
    char    ans[8] = {0};
    int     fd_i   = ifd(ctx);
    ssize_t n      = read(fd_i, ans, sizeof(ans) - 1);
    if (n > 0) ans[n] = '\0';
    if (n < 1 || (ans[0] != 'y' && ans[0] != 'Y')) {
        cmd_write(fd_o, "Cancelled.\n");
        return 0;
    }
    ctx->conv->nturn = 0;
    cmd_write(fd_o, "Conversation cleared.\n");
    return 0;
}

static int cmd_compact(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd = ofd(ctx);
    if (!ctx || !ctx->conv) {
        cmd_write(fd, "error: no active conversation\n");
        return 1;
    }
    Conversation *conv = ctx->conv;

    int nsys  = 0;
    int nbody = 0;
    for (int i = 0; i < conv->nturn; i++) {
        if (strcmp(conv->turns[i].role, "system") == 0) nsys++;
        else nbody++;
    }

    int keep_body  = nbody < 4 ? nbody : 4;
    int dropped    = nbody - keep_body;

    if (dropped <= 0) {
        cmd_write(fd, "Conversation is already compact.\n");
        return 0;
    }

    int   new_cap  = nsys + keep_body + 1; /* +1 omission marker */
    Turn *new_turns = (Turn *)arena_alloc(conv->arena,
                                          (size_t)new_cap * sizeof(Turn));
    if (!new_turns) {
        cmd_write(fd, "error: out of memory\n");
        return 1;
    }

    int ni = 0;

    /* System turns first. */
    for (int i = 0; i < conv->nturn; i++) {
        if (strcmp(conv->turns[i].role, "system") == 0)
            new_turns[ni++] = conv->turns[i];
    }

    /* Omission marker. */
    char *marker = (char *)arena_alloc(conv->arena, 64);
    if (!marker) {
        cmd_write(fd, "error: out of memory\n");
        return 1;
    }
    snprintf(marker, 64, "[%d earlier turns omitted by /compact]", dropped);
    new_turns[ni].role    = "user";
    new_turns[ni].content = marker;
    new_turns[ni].is_tool = 0;
    ni++;

    /* Last keep_body body turns. */
    int body_seen = 0;
    int body_skip = nbody - keep_body;
    for (int i = 0; i < conv->nturn; i++) {
        if (strcmp(conv->turns[i].role, "system") == 0) continue;
        if (body_seen < body_skip) { body_seen++; continue; }
        new_turns[ni++] = conv->turns[i];
        body_seen++;
    }

    conv->turns = new_turns;
    conv->nturn = ni;
    conv->cap   = new_cap;

    char line[CMD_BUF];
    snprintf(line, sizeof(line),
             "Compacted: removed %d turns, kept %d (+ omission marker).\n",
             dropped, keep_body);
    cmd_write(fd, line);
    return 0;
}

static int cmd_cost(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd      = ofd(ctx);
    int in_tok  = ctx ? ctx->in_tok  : 0;
    int out_tok = ctx ? ctx->out_tok : 0;
    char line[CMD_BUF];
    snprintf(line, sizeof(line),
             "Token usage — input: %d  output: %d  total: %d\n",
             in_tok, out_tok, in_tok + out_tok);
    cmd_write(fd, line);
    return 0;
}

static const char *NANOCODE_MD_TEMPLATE =
    "# nanocode project instructions\n"
    "\n"
    "This file is loaded as the system prompt when nanocode starts in this directory.\n"
    "\n"
    "## Project overview\n"
    "\n"
    "<!-- Describe your project here. -->\n"
    "\n"
    "## Coding conventions\n"
    "\n"
    "<!-- List style guides, naming conventions, or anything the agent should know. -->\n"
    "\n"
    "## Frequently used commands\n"
    "\n"
    "<!-- e.g. make, ./run.sh, etc. -->\n";

static int cmd_init(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd_o = ofd(ctx);
    if (access(".nanocode.md", F_OK) == 0) {
        cmd_write(fd_o, ".nanocode.md already exists — not overwriting.\n");
        return 1;
    }
    FILE *f = fopen(".nanocode.md", "w");
    if (!f) {
        cmd_write(fd_o, "error: could not create .nanocode.md\n");
        return 1;
    }
    fputs(NANOCODE_MD_TEMPLATE, f);
    fclose(f);
    cmd_write(fd_o, "Created .nanocode.md\n");
    return 0;
}

static int cmd_diff(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    if (!ctx || !ctx->sandbox) {
        cmd_write(ofd(ctx), "No pending sandbox changes.\n");
        return 0;
    }
    diff_sandbox_show(ctx->sandbox);
    return 0;
}

static int cmd_apply(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd_o = ofd(ctx);
    if (!ctx || !ctx->sandbox) {
        cmd_write(fd_o, "No pending sandbox changes to apply.\n");
        return 0;
    }
    if (diff_sandbox_apply(ctx->sandbox) == 0) {
        cmd_write(fd_o, "Changes applied.\n");
        return 0;
    }
    cmd_write(fd_o, "error: apply failed — check stderr for details.\n");
    return 1;
}

static int cmd_reject(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd_o = ofd(ctx);
    if (!ctx || !ctx->sandbox) {
        cmd_write(fd_o, "No pending sandbox changes to reject.\n");
        return 0;
    }
    diff_sandbox_discard(ctx->sandbox);
    cmd_write(fd_o, "Changes discarded.\n");
    return 0;
}

static int cmd_mcp(int argc, const char **argv, CmdContext *ctx)
{
    (void)argc; (void)argv;
    int fd_o = ofd(ctx);
    if (!ctx || ctx->nmcp == 0 || !ctx->mcp_names) {
        cmd_write(fd_o, "No MCP servers configured.\n");
        return 0;
    }
    cmd_write(fd_o, "MCP servers:\n");
    for (int i = 0; i < ctx->nmcp; i++) {
        char line[CMD_BUF];
        snprintf(line, sizeof(line), "  %s\n", ctx->mcp_names[i]);
        cmd_write(fd_o, line);
    }
    return 0;
}
