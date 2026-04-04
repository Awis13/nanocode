/*
 * git.c — git integration: repo detection, status, diff, log, tools
 *
 * Read operations (git_status, git_diff, git_log, git_branch) use popen().
 * Mutating tool operations (git_commit, git_branch_create) use fork+execvp()
 * so commit messages and branch names are passed as argv without shell
 * interpretation.
 *
 * All popen()/fork() calls are guarded by #ifndef __SANITIZE_ADDRESS__ to
 * avoid hangs under AddressSanitizer on macOS.
 */

#include "git.h"
#include "../tools/executor.h"
#include "../util/buf.h"
#include "../util/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Shell-quote `src` into `dst` using single-quote style.
 * `dst` must be at least strlen(src)*4 + 3 bytes.
 * Returns number of bytes written (excluding NUL).
 */
static size_t shell_quote(char *dst, const char *src)
{
    size_t out = 0;
    dst[out++] = '\'';
    for (const char *p = src; *p; p++) {
        if (*p == '\'') {
            /* End quote, escaped quote, re-open quote: '\'' */
            dst[out++] = '\'';
            dst[out++] = '\\';
            dst[out++] = '\'';
            dst[out++] = '\'';
        } else {
            dst[out++] = *p;
        }
    }
    dst[out++] = '\'';
    dst[out]   = '\0';
    return out;
}

/*
 * Run `cmd` via popen, capture stdout into an arena-allocated string.
 * Returns NUL-terminated string (possibly empty), or NULL on OOM.
 */
static char *popen_to_arena(Arena *arena, const char *cmd)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        char *empty = arena_alloc(arena, 1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    Buf b;
    buf_init(&b);

    char chunk[256];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
        buf_append(&b, chunk, n);

    pclose(fp);

    size_t len    = b.len;
    char  *result = arena_alloc(arena, len + 1);
    if (result) {
        if (len > 0)
            memcpy(result, buf_str(&b), len);
        result[len] = '\0';
    }
    buf_destroy(&b);
    return result;
}

/* -------------------------------------------------------------------------
 * ToolResult error helper
 * ---------------------------------------------------------------------- */

static ToolResult make_error(Arena *arena, const char *msg)
{
    size_t len  = strlen(msg);
    char  *copy = arena_alloc(arena, len + 1);
    if (copy)
        memcpy(copy, msg, len + 1);
    ToolResult r;
    r.error   = 1;
    r.content = copy;
    r.len     = len;
    return r;
}

/* -------------------------------------------------------------------------
 * JsonCtx primitive-aware value extractor (mirrors bash.c pattern)
 *
 * json_get_str() only matches JSMN_STRING tokens.  Tool args from LLMs
 * may pass booleans as JSON primitives (JSMN_PRIMITIVE), so we need to
 * handle both.
 * ---------------------------------------------------------------------- */

typedef struct { int type; int start; int end; int size; } GTok;
#define GTOK_OBJECT    1
#define GTOK_ARRAY     2
#define GTOK_STRING    4
#define GTOK_PRIMITIVE 8

static int gctx_get_val(const JsonCtx *ctx, const char *json,
                        const char *key, char *buf, size_t cap)
{
    const GTok *t    = (const GTok *)(const void *)ctx->_tok;
    int         ntok = ctx->ntok;
    size_t      klen = strlen(key);

    if (ntok < 1 || t[0].type != GTOK_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (t[i].type != GTOK_STRING)
            continue;
        int tlen = t[i].end - t[i].start;
        if ((int)klen != tlen)
            continue;
        if (memcmp(json + t[i].start, key, klen) != 0)
            continue;
        int j = i + 1;
        if (j >= ntok)
            return -1;
        if (t[j].type != GTOK_STRING && t[j].type != GTOK_PRIMITIVE)
            return -1;
        int vlen = t[j].end - t[j].start;
        if ((size_t)(vlen + 1) > cap)
            return -1;
        memcpy(buf, json + t[j].start, (size_t)vlen);
        buf[vlen] = '\0';
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * git_detect
 * ---------------------------------------------------------------------- */

int git_detect(const char *cwd)
{
    if (!cwd || !cwd[0])
        return 0;

    /* Resolve to an absolute path so walk-up terminates at "/" correctly. */
    char abs_buf[4096];
    if (cwd[0] != '/') {
        if (!realpath(cwd, abs_buf))
            return 0;
        cwd = abs_buf;
    }

    char path[4096];
    size_t len = strlen(cwd);
    if (len >= sizeof(path) - 6)
        return 0;

    memcpy(path, cwd, len + 1);

    while (len > 0) {
        char probe[4096 + 6];
        int  n = snprintf(probe, sizeof(probe), "%s/.git", path);
        if (n > 0 && (size_t)n < sizeof(probe)) {
            struct stat st;
            if (stat(probe, &st) == 0)
                return 1;
        }
        /* Stop at filesystem root to prevent infinite loop. */
        if (len == 1 && path[0] == '/')
            break;
        /* Walk up: trim trailing path component. */
        while (len > 0 && path[len - 1] != '/')
            len--;
        /* Remove the trailing slash (preserve "/" for root). */
        if (len > 1)
            len--;
        path[len] = '\0';
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * git_status
 * ---------------------------------------------------------------------- */

GitStatusResult *git_status(Arena *arena, const char *cwd)
{
    if (!arena || !cwd || !git_detect(cwd))
        return NULL;

    GitStatusResult *r = arena_alloc(arena, sizeof(*r));
    if (!r)
        return NULL;
    r->count = 0;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return r;
    shell_quote(qcwd, cwd);

    size_t cmdlen = strlen("git -C  status --porcelain 2>/dev/null")
                  + strlen(qcwd) + 1;
    char *cmd = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return r;
    }
    snprintf(cmd, cmdlen, "git -C %s status --porcelain 2>/dev/null", qcwd);
    free(qcwd);

    FILE *fp = popen(cmd, "r");
    free(cmd);
    if (!fp)
        return r;

    char line[600];
    while (fgets(line, sizeof(line), fp) && r->count < GIT_STATUS_MAX) {
        /* Porcelain format: "XY path\n" */
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\n')
            line[--llen] = '\0';
        if (llen < 4) /* need at least "XY p" */
            continue;

        GitFileStatus *fs = &r->files[r->count];
        fs->xy[0] = line[0];
        fs->xy[1] = line[1];
        fs->xy[2] = '\0';
        /* line + 3 is the path start; truncate to buffer capacity. */
        strncpy(fs->path, line + 3, sizeof(fs->path) - 1);
        fs->path[sizeof(fs->path) - 1] = '\0';
        r->count++;
    }
    pclose(fp);
#endif /* __SANITIZE_ADDRESS__ */

    return r;
}

/* -------------------------------------------------------------------------
 * git_diff
 * ---------------------------------------------------------------------- */

char *git_diff(Arena *arena, const char *cwd, const char *path)
{
    if (!arena || !cwd)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    char *cmd;
    if (path && path[0]) {
        size_t pathlen = strlen(path);
        char  *qpath   = malloc(pathlen * 4 + 3);
        if (!qpath) {
            free(qcwd);
            return NULL;
        }
        shell_quote(qpath, path);
        size_t cmdlen = strlen("git -C  diff  2>/dev/null")
                      + strlen(qcwd) + strlen(qpath) + 1;
        cmd = malloc(cmdlen);
        if (cmd)
            snprintf(cmd, cmdlen, "git -C %s diff %s 2>/dev/null",
                     qcwd, qpath);
        free(qpath);
    } else {
        size_t cmdlen = strlen("git -C  diff 2>/dev/null")
                      + strlen(qcwd) + 1;
        cmd = malloc(cmdlen);
        if (cmd)
            snprintf(cmd, cmdlen, "git -C %s diff 2>/dev/null", qcwd);
    }
    free(qcwd);

    if (!cmd)
        return NULL;

    char *result = popen_to_arena(arena, cmd);
    free(cmd);
    return result;
#else
    (void)path;
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_log
 * ---------------------------------------------------------------------- */

char *git_log(Arena *arena, const char *cwd, int n)
{
    if (!arena || !cwd || n <= 0)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", n);

    size_t cmdlen = strlen("git -C  log --oneline - 2>/dev/null")
                  + strlen(qcwd) + strlen(nbuf) + 1;
    char *cmd = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return NULL;
    }
    snprintf(cmd, cmdlen, "git -C %s log --oneline -%s 2>/dev/null",
             qcwd, nbuf);
    free(qcwd);

    char *result = popen_to_arena(arena, cmd);
    free(cmd);
    return result;
#else
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_branch
 * ---------------------------------------------------------------------- */

char *git_branch(Arena *arena, const char *cwd)
{
    if (!arena || !cwd)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    size_t cmdlen = strlen("git -C  branch --show-current 2>/dev/null")
                  + strlen(qcwd) + 1;
    char *cmd = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return NULL;
    }
    snprintf(cmd, cmdlen, "git -C %s branch --show-current 2>/dev/null", qcwd);
    free(qcwd);

    char *result = popen_to_arena(arena, cmd);
    free(cmd);

    /* Trim trailing newline. */
    if (result) {
        size_t len = strlen(result);
        if (len > 0 && result[len - 1] == '\n')
            result[len - 1] = '\0';
    }
    return result;
#else
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_diff_cached
 * ---------------------------------------------------------------------- */

char *git_diff_cached(Arena *arena, const char *cwd, int stat_only)
{
    if (!arena || !cwd)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    const char *fmt = stat_only
        ? "git -C %s diff --cached --stat 2>/dev/null"
        : "git -C %s diff --cached 2>/dev/null";

    size_t cmdlen = strlen(fmt) + strlen(qcwd) + 1;
    char  *cmd    = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return NULL;
    }
    snprintf(cmd, cmdlen, fmt, qcwd);
    free(qcwd);

    char *result = popen_to_arena(arena, cmd);
    free(cmd);
    return result;
#else
    (void)stat_only;
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_diff_stat
 * ---------------------------------------------------------------------- */

char *git_diff_stat(Arena *arena, const char *cwd)
{
    if (!arena || !cwd)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    size_t cmdlen = strlen("git -C  diff --stat 2>/dev/null")
                  + strlen(qcwd) + 1;
    char *cmd = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return NULL;
    }
    snprintf(cmd, cmdlen, "git -C %s diff --stat 2>/dev/null", qcwd);
    free(qcwd);

    char *result = popen_to_arena(arena, cmd);
    free(cmd);
    return result;
#else
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_untracked
 * ---------------------------------------------------------------------- */

char *git_untracked(Arena *arena, const char *cwd)
{
    if (!arena || !cwd)
        return NULL;

#ifndef __SANITIZE_ADDRESS__
    size_t cwdlen = strlen(cwd);
    char  *qcwd   = malloc(cwdlen * 4 + 3);
    if (!qcwd)
        return NULL;
    shell_quote(qcwd, cwd);

    size_t cmdlen = strlen(
        "git -C  ls-files --others --exclude-standard 2>/dev/null")
                  + strlen(qcwd) + 1;
    char *cmd = malloc(cmdlen);
    if (!cmd) {
        free(qcwd);
        return NULL;
    }
    snprintf(cmd, cmdlen,
             "git -C %s ls-files --others --exclude-standard 2>/dev/null",
             qcwd);
    free(qcwd);

    char *result = popen_to_arena(arena, cmd);
    free(cmd);
    return result;
#else
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#endif
}

/* -------------------------------------------------------------------------
 * git_special_state
 * ---------------------------------------------------------------------- */

char *git_special_state(Arena *arena, const char *cwd)
{
    if (!arena || !cwd)
        return NULL;

    char dotgit[4096];
    snprintf(dotgit, sizeof(dotgit), "%s/.git", cwd);

    struct stat st;
    char probe[4096 + 32];

    snprintf(probe, sizeof(probe), "%s/MERGE_HEAD", dotgit);
    if (stat(probe, &st) == 0) {
        const char *msg = "mid-merge (MERGE_HEAD present)";
        size_t      len = strlen(msg);
        char       *r   = arena_alloc(arena, len + 1);
        if (r) memcpy(r, msg, len + 1);
        return r;
    }

    snprintf(probe, sizeof(probe), "%s/CHERRY_PICK_HEAD", dotgit);
    if (stat(probe, &st) == 0) {
        const char *msg = "mid-cherry-pick (CHERRY_PICK_HEAD present)";
        size_t      len = strlen(msg);
        char       *r   = arena_alloc(arena, len + 1);
        if (r) memcpy(r, msg, len + 1);
        return r;
    }

    snprintf(probe, sizeof(probe), "%s/rebase-merge", dotgit);
    if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *msg = "mid-rebase (rebase-merge in progress)";
        size_t      len = strlen(msg);
        char       *r   = arena_alloc(arena, len + 1);
        if (r) memcpy(r, msg, len + 1);
        return r;
    }

    snprintf(probe, sizeof(probe), "%s/rebase-apply", dotgit);
    if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *msg = "mid-rebase (rebase-apply in progress)";
        size_t      len = strlen(msg);
        char       *r   = arena_alloc(arena, len + 1);
        if (r) memcpy(r, msg, len + 1);
        return r;
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * count_lines — count newlines in a NUL-terminated string.
 * ---------------------------------------------------------------------- */

static int count_lines(const char *s)
{
    if (!s) return 0;
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

/* Maximum diff lines before falling back to stat-only. */
#define GIT_DIFF_MAX_LINES 500

/* -------------------------------------------------------------------------
 * git_context_summary
 * ---------------------------------------------------------------------- */

char *git_context_summary(Arena *arena, const char *cwd)
{
    if (!arena || !cwd)
        return NULL;
    if (!git_detect(cwd))
        return NULL;

#ifdef __SANITIZE_ADDRESS__
    /* Skip subprocess calls under ASan. */
    char *empty = arena_alloc(arena, 1);
    if (empty) empty[0] = '\0';
    return empty;
#else
    Buf b;
    buf_init(&b);

    /* --- Branch and special state --- */
    char *branch = git_branch(arena, cwd);
    if (branch && branch[0]) {
        buf_append_str(&b, "## Git Context\n");
        buf_append_str(&b, "Branch: ");
        buf_append_str(&b, branch);
        buf_append_str(&b, "\n");
    }

    char *state = git_special_state(arena, cwd);
    if (state) {
        buf_append_str(&b, "State: ");
        buf_append_str(&b, state);
        buf_append_str(&b, "\n");
    }

    if (b.len > 0)
        buf_append_str(&b, "\n");

    /* --- Staged changes --- */
    char *staged_diff = git_diff_cached(arena, cwd, 0);
    if (staged_diff && staged_diff[0]) {
        buf_append_str(&b, "## Staged Changes\n");
        if (count_lines(staged_diff) > GIT_DIFF_MAX_LINES) {
            char *stat = git_diff_cached(arena, cwd, 1);
            buf_append_str(&b,
                           "*(diff >500 lines — showing stat only)*\n```\n");
            if (stat && stat[0]) buf_append_str(&b, stat);
        } else {
            buf_append_str(&b, "```diff\n");
            buf_append_str(&b, staged_diff);
        }
        if (b.len > 0 && b.data[b.len - 1] != '\n')
            buf_append_str(&b, "\n");
        buf_append_str(&b, "```\n\n");
    }

    /* --- Unstaged changes --- */
    char *unstaged_diff = git_diff(arena, cwd, NULL);
    if (unstaged_diff && unstaged_diff[0]) {
        buf_append_str(&b, "## Unstaged Changes\n");
        if (count_lines(unstaged_diff) > GIT_DIFF_MAX_LINES) {
            char *stat = git_diff_stat(arena, cwd);
            buf_append_str(&b,
                           "*(diff >500 lines — showing stat only)*\n```\n");
            if (stat && stat[0]) buf_append_str(&b, stat);
        } else {
            buf_append_str(&b, "```diff\n");
            buf_append_str(&b, unstaged_diff);
        }
        if (b.len > 0 && b.data[b.len - 1] != '\n')
            buf_append_str(&b, "\n");
        buf_append_str(&b, "```\n\n");
    }

    /* --- Untracked files --- */
    char *untracked = git_untracked(arena, cwd);
    if (untracked && untracked[0]) {
        buf_append_str(&b, "## Untracked Files\n```\n");
        buf_append_str(&b, untracked);
        if (b.len > 0 && b.data[b.len - 1] != '\n')
            buf_append_str(&b, "\n");
        buf_append_str(&b, "```\n\n");
    }

    /* --- Git Status (porcelain) --- */
    GitStatusResult *st = git_status(arena, cwd);
    if (st && st->count > 0) {
        buf_append_str(&b, "## Git Status\n```\n");
        for (int i = 0; i < st->count; i++) {
            buf_append_str(&b, st->files[i].xy);
            buf_append_str(&b, " ");
            buf_append_str(&b, st->files[i].path);
            buf_append_str(&b, "\n");
        }
        buf_append_str(&b, "```\n\n");
    }

    /* --- Recent Commits (10) --- */
    char *log = git_log(arena, cwd, 10);
    if (log && log[0]) {
        buf_append_str(&b, "## Recent Commits\n```\n");
        buf_append_str(&b, log);
        if (b.len > 0 && b.data[b.len - 1] != '\n')
            buf_append_str(&b, "\n");
        buf_append_str(&b, "```\n\n");
    }

    if (b.len == 0) {
        buf_destroy(&b);
        return NULL;
    }

    char *result = arena_alloc(arena, b.len + 1);
    if (result)
        memcpy(result, buf_str(&b), b.len + 1);

    buf_destroy(&b);
    return result;
#endif
}

/* =========================================================================
 * Tool implementations
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * run_argv — fork+execvp helper for mutating git operations
 *
 * Runs argv[0] with the given args.  Captures combined stdout+stderr
 * into a heap buffer (caller must free *out).
 * Returns the process exit code (0 = success).
 * ---------------------------------------------------------------------- */

#ifndef __SANITIZE_ADDRESS__
static int run_argv(char *const argv[], char **out, size_t *outlen)
{
    *out    = NULL;
    *outlen = 0;

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr into pipe, then exec. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    /* Parent: drain pipe. */
    close(pipefd[1]);

    size_t cap = 4096;
    char  *buf = malloc(cap);
    size_t len = 0;

    if (buf) {
        ssize_t n;
        while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
            len += (size_t)n;
            if (len + 1 >= cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb)
                    break;
                buf = nb;
            }
        }
        buf[len] = '\0';
    }
    close(pipefd[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    *out    = buf;
    *outlen = len;

    if (WIFEXITED(wstatus))
        return WEXITSTATUS(wstatus);
    return -1;
}
#endif /* __SANITIZE_ADDRESS__ */

/* -------------------------------------------------------------------------
 * git_commit tool handler
 *
 * Expected args_json fields:
 *   "message"  (string,  required) : commit message
 *   "files"    (array,   optional) : paths to stage; if absent, stage all
 *   "cwd"      (string,  optional) : working directory (default ".")
 * ---------------------------------------------------------------------- */

static const char s_commit_schema[] =
    "{"
      "\"name\":\"git_commit\","
      "\"description\":"
        "\"Stage files and create a git commit. "
        "If files is omitted, stages everything with git add .\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"message\":{"
            "\"type\":\"string\","
            "\"description\":\"Commit message\""
          "},"
          "\"files\":{"
            "\"type\":\"array\","
            "\"items\":{\"type\":\"string\"},"
            "\"description\":\"Paths to stage. Omit to stage all changes.\""
          "},"
          "\"cwd\":{"
            "\"type\":\"string\","
            "\"description\":\"Working directory. Defaults to the inherited cwd.\""
          "}"
        "},"
        "\"required\":[\"message\"]"
      "}"
    "}";

static ToolResult git_commit_handler(Arena *arena, const char *args_json)
{
    JsonCtx ctx;
    if (json_parse_ctx(&ctx, args_json, strlen(args_json)) <= 0)
        return make_error(arena, "git_commit: invalid args JSON");

    char message[2048] = {0};
    if (json_get_str(&ctx, args_json, "message", message, sizeof(message)) != 0)
        return make_error(arena, "git_commit: missing required argument: message");

    char cwd_buf[1024] = {0};
    gctx_get_val(&ctx, args_json, "cwd", cwd_buf, sizeof(cwd_buf));
    const char *cwd = cwd_buf[0] ? cwd_buf : ".";

#ifdef __SANITIZE_ADDRESS__
    (void)cwd;
    return make_error(arena, "git_commit: disabled under ASan");
#else
    /*
     * Parse the optional "files" array from raw tokens.
     * jsmntok_t fields: type, start, end, size.
     */
    const GTok *t  = (const GTok *)(const void *)ctx._tok;
    int         nt = ctx.ntok;

    int files_tok_idx = -1; /* index of the GTOK_ARRAY token */
    int files_count   = 0;

    for (int i = 1; i < nt - 1; i++) {
        if (t[i].type != GTOK_STRING)
            continue;
        int klen = t[i].end - t[i].start;
        if (klen == 5 && memcmp(args_json + t[i].start, "files", 5) == 0) {
            int j = i + 1;
            if (j < nt && t[j].type == GTOK_ARRAY) {
                files_tok_idx = j;
                files_count   = t[j].size;
            }
            break;
        }
    }

    int rc;
    char  *out    = NULL;
    size_t outlen = 0;

    /* Step 1: git add */
    if (files_tok_idx < 0 || files_count == 0) {
        /* No files specified — stage everything. */
        char *const add_argv[] = {
            "git", "-C", (char *)(uintptr_t)cwd, "add", ".", NULL
        };
        rc = run_argv(add_argv, &out, &outlen);
        free(out);
        out = NULL;
        if (rc != 0)
            return make_error(arena, "git_commit: git add failed");
    } else {
        /* Stage each file individually. */
        int tok = files_tok_idx + 1;
        for (int fi = 0; fi < files_count && tok < nt; fi++, tok++) {
            if (t[tok].type != GTOK_STRING)
                continue;
            int vlen = t[tok].end - t[tok].start;
            if (vlen <= 0 || vlen >= 512)
                continue;

            char fpath[512];
            memcpy(fpath, args_json + t[tok].start, (size_t)vlen);
            fpath[vlen] = '\0';

            char *const add_argv[] = {
                "git", "-C", (char *)(uintptr_t)cwd, "add", "--", fpath, NULL
            };
            rc = run_argv(add_argv, &out, &outlen);
            free(out);
            out = NULL;
            if (rc != 0)
                return make_error(arena, "git_commit: git add failed");
        }
    }

    /* Step 2: git commit -m <message> */
    char *const commit_argv[] = {
        "git", "-C", (char *)(uintptr_t)cwd, "commit", "-m", message, NULL
    };
    rc = run_argv(commit_argv, &out, &outlen);

    if (rc != 0) {
        size_t msglen = outlen + 32;
        char  *errmsg = arena_alloc(arena, msglen);
        if (errmsg)
            snprintf(errmsg, msglen, "git commit failed:\n%s",
                     out ? out : "");
        free(out);
        ToolResult r;
        r.error   = 1;
        r.content = errmsg;
        r.len     = errmsg ? strlen(errmsg) : 0;
        return r;
    }

    /* Return git output (contains commit hash and summary line). */
    char *result = arena_alloc(arena, outlen + 1);
    if (result && out) {
        memcpy(result, out, outlen);
        result[outlen] = '\0';
    } else if (result) {
        result[0] = '\0';
    }
    free(out);

    ToolResult r;
    r.error   = 0;
    r.content = result;
    r.len     = result ? outlen : 0;
    return r;
#endif /* __SANITIZE_ADDRESS__ */
}

/* -------------------------------------------------------------------------
 * git_branch_create tool handler
 *
 * Expected args_json fields:
 *   "name"      (string,  required) : branch name
 *   "checkout"  (boolean, optional) : checkout the new branch (default true)
 *   "cwd"       (string,  optional) : working directory (default ".")
 * ---------------------------------------------------------------------- */

static const char s_branch_schema[] =
    "{"
      "\"name\":\"git_branch_create\","
      "\"description\":"
        "\"Create a new git branch. By default also checks it out.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"name\":{"
            "\"type\":\"string\","
            "\"description\":\"Branch name to create\""
          "},"
          "\"checkout\":{"
            "\"type\":\"boolean\","
            "\"description\":\"Check out the new branch (default true)\""
          "},"
          "\"cwd\":{"
            "\"type\":\"string\","
            "\"description\":\"Working directory. Defaults to the inherited cwd.\""
          "}"
        "},"
        "\"required\":[\"name\"]"
      "}"
    "}";

static ToolResult git_branch_create_handler(Arena *arena, const char *args_json)
{
    JsonCtx ctx;
    if (json_parse_ctx(&ctx, args_json, strlen(args_json)) <= 0)
        return make_error(arena, "git_branch_create: invalid args JSON");

    char name[256] = {0};
    if (json_get_str(&ctx, args_json, "name", name, sizeof(name)) != 0)
        return make_error(arena,
                          "git_branch_create: missing required argument: name");

    /* "checkout" is a JSON boolean — use gctx_get_val to read the primitive. */
    char checkout_str[8] = {0};
    int  do_checkout     = 1; /* default true */
    if (gctx_get_val(&ctx, args_json, "checkout",
                     checkout_str, sizeof(checkout_str)) == 0) {
        if (strcmp(checkout_str, "false") == 0 ||
            strcmp(checkout_str, "0")     == 0)
            do_checkout = 0;
    }

    char cwd_buf[1024] = {0};
    gctx_get_val(&ctx, args_json, "cwd", cwd_buf, sizeof(cwd_buf));
    const char *cwd = cwd_buf[0] ? cwd_buf : ".";

#ifdef __SANITIZE_ADDRESS__
    (void)cwd;
    (void)do_checkout;
    return make_error(arena, "git_branch_create: disabled under ASan");
#else
    int    rc;
    char  *out    = NULL;
    size_t outlen = 0;

    if (do_checkout) {
        /* git checkout -b <name> */
        char *const argv[] = {
            "git", "-C", (char *)(uintptr_t)cwd,
            "checkout", "-b", name, NULL
        };
        rc = run_argv(argv, &out, &outlen);
    } else {
        /* git branch <name> */
        char *const argv[] = {
            "git", "-C", (char *)(uintptr_t)cwd,
            "branch", name, NULL
        };
        rc = run_argv(argv, &out, &outlen);
    }

    if (rc != 0) {
        size_t msglen = outlen + 48;
        char  *errmsg = arena_alloc(arena, msglen);
        if (errmsg)
            snprintf(errmsg, msglen, "git branch creation failed:\n%s",
                     out ? out : "");
        free(out);
        ToolResult r;
        r.error   = 1;
        r.content = errmsg;
        r.len     = errmsg ? strlen(errmsg) : 0;
        return r;
    }

    char *result = arena_alloc(arena, outlen + 1);
    if (result && out) {
        memcpy(result, out, outlen);
        result[outlen] = '\0';
    } else if (result) {
        result[0] = '\0';
    }
    free(out);

    ToolResult r;
    r.error   = 0;
    r.content = result;
    r.len     = result ? outlen : 0;
    return r;
#endif /* __SANITIZE_ADDRESS__ */
}

/* -------------------------------------------------------------------------
 * git_tools_register
 * ---------------------------------------------------------------------- */

void git_tools_register(void)
{
    tool_register("git_commit", s_commit_schema, git_commit_handler, TOOL_SAFE_MUTATING);
    tool_register("git_branch_create", s_branch_schema, git_branch_create_handler, TOOL_SAFE_MUTATING);
}
