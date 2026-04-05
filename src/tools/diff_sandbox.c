/*
 * diff_sandbox.c — patch buffer, cumulative diff, confirm/reject
 *
 * Accumulates file patches, renders unified diffs to the terminal, and
 * writes files atomically (temp-then-rename).
 *
 * The DP table for LCS computation is heap-allocated and freed immediately
 * after use to keep the arena footprint small.  All other data (patch
 * entries, line arrays, string copies) lives in the caller-supplied arena.
 */

#include "diff_sandbox.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define CONTEXT_LINES   3
#define MAX_DIFF_LINES  2000    /* per-side line cap; larger files fall back */
#define TMP_SUFFIX      ".nanosandbox"

/* -------------------------------------------------------------------------
 * ANSI colour helpers
 * ---------------------------------------------------------------------- */

#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_CYAN   "\033[36m"

static int use_color(int fd)
{
    return isatty(fd);
}

/* -------------------------------------------------------------------------
 * Internal structures
 * ---------------------------------------------------------------------- */

typedef struct PatchEntry {
    const char        *path;
    const char        *old_content;   /* NULL for new files */
    const char        *new_content;
    struct PatchEntry *next;
} PatchEntry;

struct DiffSandbox {
    Arena      *arena;
    PatchEntry *head;
    PatchEntry *tail;
    int         count;
    int         discarded;
};

/* -------------------------------------------------------------------------
 * Arena string duplication
 * ---------------------------------------------------------------------- */

static const char *arena_strdup(Arena *arena, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char  *d = arena_alloc(arena, n + 1);
    memcpy(d, s, n + 1);
    return d;
}

/* -------------------------------------------------------------------------
 * mkdir -p for the directory component of `filepath`
 * ---------------------------------------------------------------------- */

static void ensure_parent_dirs(const char *filepath)
{
    char   buf[4096];
    size_t len = strlen(filepath);
    if (len >= sizeof(buf)) return;
    memcpy(buf, filepath, len + 1);

    char *last_slash = strrchr(buf, '/');
    if (!last_slash) return;
    *last_slash = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755); /* ignore errors (EEXIST is expected) */
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

/* -------------------------------------------------------------------------
 * Line splitting
 *
 * Splits `text` into an arena-allocated array of NUL-terminated strings.
 * Each line includes its trailing '\n' when present.
 * ---------------------------------------------------------------------- */

typedef struct { const char **lines; int count; } Lines;

static Lines split_lines(Arena *arena, const char *text)
{
    Lines l = { NULL, 0 };
    if (!text || !*text) return l;

    /* Count lines: one per '\n', plus one extra for a missing trailing '\n'. */
    int count = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') count++;
    if (text[strlen(text) - 1] != '\n')
        count++;

    l.lines = arena_alloc(arena, (size_t)(count + 1) * sizeof(char *));

    const char *start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - start);
            if (*p == '\n') len++; /* include the newline */
            if (len > 0 || *p == '\n') {
                char *line = arena_alloc(arena, len + 1);
                memcpy(line, start, len);
                line[len] = '\0';
                l.lines[l.count++] = line;
            }
            start = p + 1;
            if (*p == '\0') break;
        }
    }
    return l;
}

/* -------------------------------------------------------------------------
 * LCS-based diff
 *
 * Computes a minimal edit script between Lines `a` and `b` using the
 * classic O(m*n) DP algorithm.  The DP table is heap-allocated and freed
 * before returning.  The EditOp array is arena-allocated.
 *
 * Returns NULL with *out_nops = -1 when either file exceeds MAX_DIFF_LINES.
 * ---------------------------------------------------------------------- */

#define OP_EQ   0
#define OP_ADD  1
#define OP_DEL  2

typedef struct { int kind; int a_idx; int b_idx; } EditOp;

static EditOp *compute_diff(Arena *arena,
                            const Lines *a, const Lines *b,
                            int *out_nops)
{
    int m = a->count, n = b->count;

    if (m > MAX_DIFF_LINES || n > MAX_DIFF_LINES) {
        *out_nops = -1;
        return NULL;
    }

    /* Heap-allocate the DP table to avoid bloating the arena. */
    size_t table_bytes = (size_t)(m + 1) * (size_t)(n + 1) * sizeof(int);
    int   *dp          = malloc(table_bytes);
    if (!dp) { *out_nops = -1; return NULL; }
    memset(dp, 0, table_bytes);

#define DP(i, j)  dp[(size_t)(i) * (size_t)(n + 1) + (size_t)(j)]

    for (int i = 1; i <= m; i++) {
        for (int j = 1; j <= n; j++) {
            if (strcmp(a->lines[i - 1], b->lines[j - 1]) == 0)
                DP(i, j) = DP(i - 1, j - 1) + 1;
            else
                DP(i, j) = DP(i - 1, j) >= DP(i, j - 1)
                          ? DP(i - 1, j) : DP(i, j - 1);
        }
    }

    /* Backtrack from (m, n) to build edit ops in reverse order.
     * Tie-break: prefer ADD — this yields DEL-before-ADD in the final
     * (reversed) edit script, matching the standard unified-diff style. */
    EditOp *ops  = arena_alloc(arena, (size_t)(m + n + 1) * sizeof(EditOp));
    int     nops = 0;
    int     i = m, j = n;

    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 &&
            strcmp(a->lines[i - 1], b->lines[j - 1]) == 0) {
            ops[nops++] = (EditOp){ OP_EQ, i - 1, j - 1 };
            i--; j--;
        } else if (j > 0 &&
                   (i == 0 || DP(i, j - 1) >= DP(i - 1, j))) {
            ops[nops++] = (EditOp){ OP_ADD, -1, j - 1 };
            j--;
        } else {
            ops[nops++] = (EditOp){ OP_DEL, i - 1, -1 };
            i--;
        }
    }

#undef DP
    free(dp);

    /* Reverse to forward order. */
    for (int lo = 0, hi = nops - 1; lo < hi; lo++, hi--) {
        EditOp tmp = ops[lo]; ops[lo] = ops[hi]; ops[hi] = tmp;
    }

    *out_nops = nops;
    return ops;
}

/* -------------------------------------------------------------------------
 * Coloured line printers
 * ---------------------------------------------------------------------- */

static void print_hunk_hdr(int fd, int old_start, int old_count,
                           int new_start, int new_count)
{
    if (use_color(fd))
        dprintf(fd, ANSI_CYAN "@@ -%d,%d +%d,%d @@\n" ANSI_RESET,
                old_start, old_count, new_start, new_count);
    else
        dprintf(fd, "@@ -%d,%d +%d,%d @@\n",
                old_start, old_count, new_start, new_count);
}

static void print_file_hdr(int fd, const char *a, const char *b)
{
    if (use_color(fd))
        dprintf(fd, ANSI_BOLD "--- %s\n+++ %s\n" ANSI_RESET, a, b);
    else
        dprintf(fd, "--- %s\n+++ %s\n", a, b);
}

static void print_add(int fd, const char *s)
{
    if (use_color(fd)) dprintf(fd, ANSI_GREEN "+%s" ANSI_RESET, s);
    else               dprintf(fd, "+%s", s);
}

static void print_del(int fd, const char *s)
{
    if (use_color(fd)) dprintf(fd, ANSI_RED "-%s" ANSI_RESET, s);
    else               dprintf(fd, "-%s", s);
}

static void print_ctx(int fd, const char *s) { dprintf(fd, " %s", s); }

/* -------------------------------------------------------------------------
 * Unified diff renderer for a single file
 * ---------------------------------------------------------------------- */

static void print_unified_diff(int fd,
                               const char *path,
                               const char *old_content,
                               const char *new_content,
                               Arena *arena)
{
    char hdr_a[4096], hdr_b[4096];
    snprintf(hdr_a, sizeof(hdr_a), "a/%s", path);
    snprintf(hdr_b, sizeof(hdr_b), "b/%s", path);
    print_file_hdr(fd, hdr_a, hdr_b);

    Lines a = split_lines(arena, old_content ? old_content : "");
    Lines b = split_lines(arena, new_content ? new_content : "");

    int      nops = 0;
    EditOp  *ops  = compute_diff(arena, &a, &b, &nops);

    if (nops < 0 || !ops) {
        /* Fallback for large/unanticipated files: show full new content. */
        print_hunk_hdr(fd, 0, a.count, a.count > 0 ? 1 : 0, b.count);
        for (int k = 0; k < b.count; k++) print_add(fd, b.lines[k]);
        return;
    }

    if (nops == 0) return; /* files are identical */

    /*
     * Two-pass hunk marking:
     *   1. Forward:  mark CONTEXT_LINES EQ ops after each change.
     *   2. Backward: mark CONTEXT_LINES EQ ops before each change.
     */
    char *in_hunk = malloc((size_t)nops);
    if (!in_hunk) return;
    memset(in_hunk, 0, (size_t)nops);

    int ctx = 0;
    for (int k = 0; k < nops; k++) {
        if (ops[k].kind != OP_EQ) { in_hunk[k] = 1; ctx = CONTEXT_LINES; }
        else if (ctx > 0)          { in_hunk[k] = 1; ctx--; }
    }
    ctx = 0;
    for (int k = nops - 1; k >= 0; k--) {
        if (ops[k].kind != OP_EQ) { in_hunk[k] = 1; ctx = CONTEXT_LINES; }
        else if (ctx > 0)          { in_hunk[k] = 1; ctx--; }
    }

    /*
     * Walk the marked ops, emitting contiguous hunk segments.
     * old_line / new_line track the 1-based current position in each file.
     */
    int old_line = 1, new_line = 1;
    int k = 0;

    while (k < nops) {
        if (!in_hunk[k]) {
            if (ops[k].kind != OP_ADD) old_line++;
            if (ops[k].kind != OP_DEL) new_line++;
            k++;
            continue;
        }

        /* Find end of this contiguous hunk segment. */
        int hend = k;
        while (hend < nops && in_hunk[hend]) hend++;

        /* Count old/new lines in the hunk for the @@ header. */
        int old_count = 0, new_count = 0;
        for (int j = k; j < hend; j++) {
            if (ops[j].kind != OP_ADD) old_count++;
            if (ops[j].kind != OP_DEL) new_count++;
        }

        /*
         * Hunk start line numbers:
         *   - When old_count > 0: the first old line in this hunk.
         *   - When old_count == 0 (pure insertion): the line just before
         *     the insertion point (0 when inserting at the file's start).
         */
        int old_start = (old_count > 0) ? old_line
                                        : (old_line > 1 ? old_line - 1 : 0);
        int new_start = (new_count > 0) ? new_line
                                        : (new_line > 1 ? new_line - 1 : 0);

        print_hunk_hdr(fd, old_start, old_count, new_start, new_count);

        for (int j = k; j < hend; j++) {
            switch (ops[j].kind) {
            case OP_EQ:
                print_ctx(fd, a.lines[ops[j].a_idx]);
                old_line++; new_line++;
                break;
            case OP_DEL:
                print_del(fd, a.lines[ops[j].a_idx]);
                old_line++;
                break;
            case OP_ADD:
                print_add(fd, b.lines[ops[j].b_idx]);
                new_line++;
                break;
            }
        }

        k = hend;
    }

    free(in_hunk);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

DiffSandbox *diff_sandbox_new(Arena *arena)
{
    DiffSandbox *sb = arena_alloc(arena, sizeof(DiffSandbox));
    sb->arena     = arena;
    sb->head      = NULL;
    sb->tail      = NULL;
    sb->count     = 0;
    sb->discarded = 0;
    return sb;
}

void diff_sandbox_queue(DiffSandbox *sb, const char *path,
                        const char *old_content, const char *new_content)
{
    if (!sb || sb->discarded || !path || !new_content) return;

    PatchEntry *e = arena_alloc(sb->arena, sizeof(PatchEntry));
    e->path        = arena_strdup(sb->arena, path);
    e->old_content = arena_strdup(sb->arena, old_content); /* NULL → NULL */
    e->new_content = arena_strdup(sb->arena, new_content);
    e->next        = NULL;

    if (sb->tail) sb->tail->next = e;
    else          sb->head = e;
    sb->tail = e;
    sb->count++;
}

void diff_sandbox_show(DiffSandbox *sb)
{
    if (!sb || sb->count == 0) return;
    for (PatchEntry *e = sb->head; e; e = e->next)
        print_unified_diff(STDOUT_FILENO, e->path, e->old_content, e->new_content, sb->arena);
    fflush(stdout);
}

void diff_sandbox_show_one(const char *path,
                           const char *old_content,
                           const char *new_content,
                           int fd,
                           Arena *arena)
{
    if (!path || !new_content || !arena) return;
    print_unified_diff(fd, path, old_content, new_content, arena);
}

int diff_sandbox_apply(DiffSandbox *sb)
{
    if (!sb || sb->discarded) return -1;
    if (sb->count == 0) return 0;

    /* Allocate temp-path storage on the heap (freed before return). */
    char (*tmp_paths)[4096] = malloc((size_t)sb->count * sizeof(*tmp_paths));
    if (!tmp_paths) return -1;

    int         written = 0;
    PatchEntry *e       = sb->head;

    /* ------------------------------------------------------------------ *
     * Phase 1 — write each file to a temp path.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < sb->count && e; i++, e = e->next) {
        size_t plen = strlen(e->path);
        if (plen + sizeof(TMP_SUFFIX) > 4096)
            goto cleanup;

        snprintf(tmp_paths[i], 4096, "%s" TMP_SUFFIX, e->path);
        ensure_parent_dirs(tmp_paths[i]);

        FILE *f = fopen(tmp_paths[i], "wb");
        if (!f) goto cleanup;

        size_t clen    = strlen(e->new_content);
        int    ok      = ((size_t)fwrite(e->new_content, 1, clen, f) == clen
                          && !ferror(f));
        fclose(f);

        if (!ok) { unlink(tmp_paths[i]); goto cleanup; }
        written++;
    }

    /* ------------------------------------------------------------------ *
     * Phase 2 — rename all temp files to their final paths.
     * If a rename fails, clean up unprocessed temps and return an error.
     * (Temps already renamed cannot be rolled back — POSIX limitation.)
     * ------------------------------------------------------------------ */
    e = sb->head;
    for (int i = 0; i < sb->count && e; i++, e = e->next) {
        if (rename(tmp_paths[i], e->path) != 0) {
            for (int j = i; j < written; j++)
                unlink(tmp_paths[j]);
            free(tmp_paths);
            return -1;
        }
    }

    free(tmp_paths);
    return 0;

cleanup:
    for (int i = 0; i < written; i++)
        unlink(tmp_paths[i]);
    free(tmp_paths);
    return -1;
}

void diff_sandbox_discard(DiffSandbox *sb)
{
    if (sb) sb->discarded = 1;
    /* Arena-allocated metadata is left in place.  No files are touched. */
}
