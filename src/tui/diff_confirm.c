/*
 * diff_confirm.c -- interactive diff confirmation callback
 *
 * Flow for each write_file / edit_file invocation:
 *   1. Capture the colored unified diff into a string buffer.
 *   2. If > TRUNCATE_TOTAL lines: show first TRUNCATE_HEAD + summary + TRUNCATE_TAIL.
 *   3. Print the (possibly truncated) diff to fd_out.
 *   4. Read a single keypress from fd_in using raw termios.
 *   5. Return per key: y=apply, n=reject, a=apply-all, e=editor.
 */

#include "diff_confirm.h"
#include "../tools/diff_sandbox.h"
#include "../util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* Forward declaration avoids pulling in diff_sandbox.h transitive deps. */
void diff_sandbox_show_one(const char *path,
                           const char *old_content,
                           const char *new_content,
                           Arena *arena);

/* Diff truncation thresholds. */
#define TRUNCATE_TOTAL  100
#define TRUNCATE_HEAD    50
#define TRUNCATE_TAIL    10

/* Scratch arena size for the diff renderer. */
#define DIFF_ARENA_SIZE  (512 * 1024)

/* -------------------------------------------------------------------------
 * capture_diff -- render unified diff into a heap-allocated string
 *
 * Uses pipe + dup2 to intercept diff_sandbox_show_one()'s stdout output,
 * then restores stdout. Returns malloc'd NUL-terminated string; caller frees.
 * Returns NULL on allocation / pipe failure.
 * ---------------------------------------------------------------------- */

static char *capture_diff(const char *path,
                           const char *old_content,
                           const char *new_content)
{
    int pfd[2];
    if (pipe(pfd) < 0) return NULL;

    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) { close(pfd[0]); close(pfd[1]); return NULL; }

    if (dup2(pfd[1], STDOUT_FILENO) < 0) {
        close(pfd[0]); close(pfd[1]); close(saved_stdout); return NULL;
    }
    close(pfd[1]);

    Arena *arena = arena_new(DIFF_ARENA_SIZE);
    if (arena) {
        diff_sandbox_show_one(path, old_content, new_content, arena);
        arena_free(arena);
    }
    fflush(stdout);

    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    size_t cap  = 4096;
    size_t used = 0;
    char  *buf  = malloc(cap);
    if (!buf) { close(pfd[0]); return NULL; }

    ssize_t n;
    while ((n = read(pfd[0], buf + used, cap - used - 1)) > 0) {
        used += (size_t)n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(pfd[0]); return NULL; }
            buf = nb;
        }
    }
    close(pfd[0]);
    buf[used] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * maybe_truncate -- if diff exceeds TRUNCATE_TOTAL lines, trim the middle
 *
 * Returns a malloc'd replacement string, or the original pointer if no
 * truncation was needed (original must still be free()'d by caller).
 * ---------------------------------------------------------------------- */

static char *maybe_truncate(char *diff, int *did_truncate)
{
    *did_truncate = 0;

    int lines = 0;
    for (const char *p = diff; *p; p++)
        if (*p == '\n') lines++;

    if (lines <= TRUNCATE_TOTAL) return diff;
    *did_truncate = 1;

    char **line_ptrs = malloc((size_t)(lines + 2) * sizeof(char *));
    if (!line_ptrs) return diff;

    int   idx = 0;
    char *p   = diff;
    line_ptrs[idx++] = p;
    while (*p) {
        if (*p++ == '\n' && *p) line_ptrs[idx++] = p;
    }
    int total = idx;

    size_t cap = 4096;
    char  *out = malloc(cap);
    if (!out) { free(line_ptrs); return diff; }
    size_t used = 0;

#define APPEND(str, len) do {                                       \
    size_t _l = (len);                                              \
    while (used + _l + 1 >= cap) {                                  \
        cap *= 2;                                                    \
        char *_nb = realloc(out, cap);                              \
        if (!_nb) { free(line_ptrs); free(out); return diff; }      \
        out = _nb;                                                   \
    }                                                               \
    memcpy(out + used, (str), _l);                                  \
    used += _l;                                                     \
} while (0)

    for (int i = 0; i < TRUNCATE_HEAD && i < total; i++) {
        const char *start = line_ptrs[i];
        const char *end   = (i + 1 < total) ? line_ptrs[i+1] : start + strlen(start);
        APPEND(start, (size_t)(end - start));
    }

    int skipped = total - TRUNCATE_HEAD - TRUNCATE_TAIL;
    char summary[128];
    int slen = snprintf(summary, sizeof(summary),
                        "\x1b[33m... %d more lines (truncated) ...\x1b[0m\n",
                        skipped);
    APPEND(summary, (size_t)slen);

    int tail_start = total - TRUNCATE_TAIL;
    if (tail_start < TRUNCATE_HEAD) tail_start = TRUNCATE_HEAD;
    for (int i = tail_start; i < total; i++) {
        const char *start = line_ptrs[i];
        const char *end   = (i + 1 < total) ? line_ptrs[i+1] : start + strlen(start);
        APPEND(start, (size_t)(end - start));
    }
#undef APPEND

    out[used] = '\0';
    free(line_ptrs);
    free(diff);
    return out;
}

/* -------------------------------------------------------------------------
 * read_key -- read a single character in raw terminal mode
 * ---------------------------------------------------------------------- */

static char read_key(int fd_in)
{
    struct termios old_t, raw_t;
    if (tcgetattr(fd_in, &old_t) < 0) return 'n';

    raw_t = old_t;
    cfmakeraw(&raw_t);
    tcsetattr(fd_in, TCSAFLUSH, &raw_t);

    char c = 'n';
    (void)read(fd_in, &c, 1);

    tcsetattr(fd_in, TCSAFLUSH, &old_t);
    return c;
}

/* -------------------------------------------------------------------------
 * open_in_editor -- write content to a temp file, launch $EDITOR, read back
 * ---------------------------------------------------------------------- */

static char *open_in_editor(const char *content)
{
    char tmp_path[] = "/tmp/nanocode_edit_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return NULL;

    size_t clen = content ? strlen(content) : 0;
    if (clen > 0 && write(fd, content, clen) != (ssize_t)clen) {
        close(fd); unlink(tmp_path); return NULL;
    }
    close(fd);

    const char *editor = getenv("EDITOR");
    if (!editor || !*editor) editor = "vi";

    size_t cmd_len = strlen(editor) + 1 + strlen(tmp_path) + 1;
    char  *cmd     = malloc(cmd_len);
    if (!cmd) { unlink(tmp_path); return NULL; }
    snprintf(cmd, cmd_len, "%s %s", editor, tmp_path);

    int ret = system(cmd);
    free(cmd);
    if (ret != 0) { unlink(tmp_path); return NULL; }

    FILE *f = fopen(tmp_path, "rb");
    if (!f) { unlink(tmp_path); return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *result = malloc((size_t)sz + 1);
    if (!result) { fclose(f); unlink(tmp_path); return NULL; }

    size_t rd = fread(result, 1, (size_t)sz, f);
    result[rd] = '\0';
    fclose(f);
    unlink(tmp_path);
    return result;
}

/* -------------------------------------------------------------------------
 * diff_confirm_cb -- the main confirmation callback
 * ---------------------------------------------------------------------- */

int diff_confirm_cb(const char *path,
                    const char *old_content,
                    const char *new_content,
                    void *ctx)
{
    DiffConfirmCtx *dcc = (DiffConfirmCtx *)ctx;
    int fd_out = dcc ? dcc->fd_out : STDOUT_FILENO;
    int fd_in  = dcc ? dcc->fd_in  : STDIN_FILENO;

    if (dcc && dcc->auto_apply) return -1;  /* apply-all */

    char *diff = capture_diff(path, old_content, new_content);
    if (!diff) {
        dprintf(fd_out, "\nWrite to: %s\n", path);
    } else {
        int truncated = 0;
        diff = maybe_truncate(diff, &truncated);
        (void)write(fd_out, diff, strlen(diff));
        (void)truncated;
        free(diff);
    }

    const char *prompt =
        "\x1b[1mApply this change?\x1b[0m "
        "[\x1b[32my\x1b[0m]es / "
        "[\x1b[31mn\x1b[0m]o / "
        "[\x1b[33me\x1b[0m]dit / "
        "[\x1b[36ma\x1b[0m]ll: ";
    (void)write(fd_out, prompt, strlen(prompt));

    for (;;) {
        char key = read_key(fd_in);
        char echo[3] = { key, '\n', '\0' };
        (void)write(fd_out, echo, 2);

        switch (key) {
        case 'y': case 'Y': return 1;
        case 'n': case 'N': return 0;
        case 'a': case 'A': return -1;
        case 'e': case 'E': {
            char *edited = open_in_editor(new_content);
            if (edited) {
                dprintf(fd_out,
                        "\x1b[33mNote:\x1b[0m editor opened; "
                        "the agent's original content will be written.\n"
                        "Apply the agent's original change?\n"
                        "[\x1b[32my\x1b[0m]es / "
                        "[\x1b[31mn\x1b[0m]o: ");
                free(edited);
                for (;;) {
                    char k2 = read_key(fd_in);
                    char e2[3] = { k2, '\n', '\0' };
                    (void)write(fd_out, e2, 2);
                    if (k2 == 'y' || k2 == 'Y') return 1;
                    if (k2 == 'n' || k2 == 'N') return 0;
                }
            }
            (void)write(fd_out, prompt, strlen(prompt));
            break;
        }
        default:
            (void)write(fd_out, prompt, strlen(prompt));
            break;
        }
    }
}
