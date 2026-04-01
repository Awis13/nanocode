/*
 * grep.c — grep tool: recursive content search, POSIX regex.h, context lines
 *
 * Uses POSIX regcomp/regexec for matching, opendir/readdir for directory
 * traversal, and fnmatch-compatible glob_match for file filtering.
 *
 * Output modes:
 *   content           — matching lines (with optional context) in grep format
 *   files_with_matches — one file path per matching file (default)
 *   count             — "path:N" for each file with at least one match
 *
 * .gitignore support: .git/ and vendor/ directories are always skipped.
 */

#define JSMN_STATIC
#include "jsmn.h"

#include "executor.h"
#include "grep.h"
#include "../util/arena.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <regex.h>

/* Max context lines supported (ring buffer capacity). */
#define GREP_MAX_CTX    100
/* Max line length stored in context ring buffer. */
#define GREP_CTX_LINE   4096
/* Default head_limit. */
#define GREP_HEAD_DEF   250
/* Max JSON argument tokens. */
#define ARG_TOK_MAX     256
/* Max line length when reading a file. */
#define GREP_LINE_MAX   4096

/* -------------------------------------------------------------------------
 * Result helpers
 * ---------------------------------------------------------------------- */

static ToolResult err_result(Arena *arena, const char *msg)
{
    size_t n   = strlen(msg);
    char  *buf = arena_alloc(arena, n + 1);
    memcpy(buf, msg, n + 1);
    return (ToolResult){ .error = 1, .content = buf, .len = n };
}

static ToolResult ok_result(char *content, size_t len)
{
    return (ToolResult){ .error = 0, .content = content, .len = len };
}

/* -------------------------------------------------------------------------
 * JSON helpers — same pattern as fileops.c
 * ---------------------------------------------------------------------- */

static int json_unescape(char *buf, int len)
{
    int src = 0, dst = 0;
    while (src < len) {
        if (buf[src] != '\\') { buf[dst++] = buf[src++]; continue; }
        src++;
        if (src >= len) break;
        switch (buf[src]) {
        case '"':  buf[dst++] = '"';  src++; break;
        case '\\': buf[dst++] = '\\'; src++; break;
        case '/':  buf[dst++] = '/';  src++; break;
        case 'b':  buf[dst++] = '\b'; src++; break;
        case 'f':  buf[dst++] = '\f'; src++; break;
        case 'n':  buf[dst++] = '\n'; src++; break;
        case 'r':  buf[dst++] = '\r'; src++; break;
        case 't':  buf[dst++] = '\t'; src++; break;
        case 'u':
            buf[dst++] = '\\';
            buf[dst++] = 'u';
            src++;
            for (int k = 0; k < 4 && src < len; k++, src++)
                buf[dst++] = buf[src];
            break;
        default: buf[dst++] = buf[src++]; break;
        }
    }
    buf[dst] = '\0';
    return dst;
}

static int args_parse(const char *args_json, jsmntok_t *toks, int max_toks)
{
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, args_json, strlen(args_json), toks, max_toks);
    return r < 0 ? 0 : r;
}

static char *args_str(Arena *arena, jsmntok_t *toks, int ntok,
                      const char *json, const char *key)
{
    int klen = (int)strlen(key);
    for (int i = 1; i < ntok - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        if (toks[i].end - toks[i].start != klen) continue;
        if (memcmp(json + toks[i].start, key, (size_t)klen) != 0) continue;
        if (toks[i + 1].type != JSMN_STRING) continue;
        int   vlen = toks[i + 1].end - toks[i + 1].start;
        char *out  = arena_alloc(arena, (size_t)vlen + 1);
        memcpy(out, json + toks[i + 1].start, (size_t)vlen);
        int actual = json_unescape(out, vlen);
        out[actual] = '\0';
        return out;
    }
    return NULL;
}

static int args_long(jsmntok_t *toks, int ntok,
                     const char *json, const char *key, long *out)
{
    int klen = (int)strlen(key);
    for (int i = 1; i < ntok - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        if (toks[i].end - toks[i].start != klen) continue;
        if (memcmp(json + toks[i].start, key, (size_t)klen) != 0) continue;
        if (toks[i + 1].type != JSMN_PRIMITIVE) continue;
        int  vlen = toks[i + 1].end - toks[i + 1].start;
        char buf[32];
        if (vlen >= (int)sizeof(buf)) return -1;
        memcpy(buf, json + toks[i + 1].start, (size_t)vlen);
        buf[vlen] = '\0';
        char *end;
        *out = strtol(buf, &end, 10);
        return (*end == '\0') ? 0 : -1;
    }
    return -1;
}

static int args_bool(jsmntok_t *toks, int ntok,
                     const char *json, const char *key, int *out)
{
    int klen = (int)strlen(key);
    for (int i = 1; i < ntok - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        if (toks[i].end - toks[i].start != klen) continue;
        if (memcmp(json + toks[i].start, key, (size_t)klen) != 0) continue;
        if (toks[i + 1].type != JSMN_PRIMITIVE) continue;
        int         vlen = toks[i + 1].end - toks[i + 1].start;
        const char *v    = json + toks[i + 1].start;
        if (vlen == 4 && memcmp(v, "true",  4) == 0) { *out = 1; return 0; }
        if (vlen == 5 && memcmp(v, "false", 5) == 0) { *out = 0; return 0; }
        return -1;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Glob matching (supports * and **)
 * ---------------------------------------------------------------------- */

static int grep_glob_match(const char *pat, const char *str)
{
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            const char *rest = pat + 2;
            if (*rest == '/') rest++;
            if (*rest == '\0') return 1;
            const char *p = str;
            for (;;) {
                if (grep_glob_match(rest, p)) return 1;
                p = strchr(p, '/');
                if (!p) break;
                p++;
            }
            return 0;
        } else if (*pat == '*') {
            const char *rest = pat + 1;
            const char *p    = str;
            while (*p && *p != '/') {
                if (grep_glob_match(rest, p)) return 1;
                p++;
            }
            return grep_glob_match(rest, p);
        } else if (*pat == '?') {
            if (*str == '\0' || *str == '/') return 0;
            pat++; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

/*
 * Return 1 if the file at `rel_path` matches `glob_pat`.
 * Tries both the full relative path and just the basename.
 * If glob_pat is NULL or empty, always returns 1.
 */
static int file_matches_glob(const char *glob_pat, const char *rel_path)
{
    if (!glob_pat || glob_pat[0] == '\0') return 1;
    if (grep_glob_match(glob_pat, rel_path)) return 1;
    const char *base = strrchr(rel_path, '/');
    base = base ? base + 1 : rel_path;
    return grep_glob_match(glob_pat, base);
}

/* -------------------------------------------------------------------------
 * Context ring buffer
 * ---------------------------------------------------------------------- */

typedef struct {
    char   data[GREP_CTX_LINE];
    size_t len;
    int    lineno;
} CtxLine;

typedef struct {
    CtxLine *entries;   /* heap-allocated array of `cap` entries */
    int      cap;       /* total capacity */
    int      head;      /* next slot to write (oldest when full) */
    int      count;     /* valid entries, 0..cap */
} RingBuf;

static void ring_init(RingBuf *rb, int cap)
{
    rb->entries = malloc((size_t)cap * sizeof(CtxLine));
    rb->cap     = rb->entries ? cap : 0;
    rb->head    = 0;
    rb->count   = 0;
}

static void ring_free(RingBuf *rb)
{
    free(rb->entries);
    rb->entries = NULL;
}

static void ring_reset(RingBuf *rb)
{
    rb->head  = 0;
    rb->count = 0;
}

static void ring_push(RingBuf *rb, const char *data, size_t len, int lineno)
{
    if (rb->cap == 0) return;
    CtxLine *slot = &rb->entries[rb->head];
    size_t   copy = len < GREP_CTX_LINE - 1 ? len : GREP_CTX_LINE - 1;
    memcpy(slot->data, data, copy);
    slot->data[copy] = '\0';
    slot->len        = copy;
    slot->lineno     = lineno;
    rb->head         = (rb->head + 1) % rb->cap;
    if (rb->count < rb->cap) rb->count++;
}

/* Return ring entry at logical index `i` (0 = oldest). */
static CtxLine *ring_get(RingBuf *rb, int i)
{
    int idx = (rb->head - rb->count + i + rb->cap * 2) % rb->cap;
    return &rb->entries[idx];
}

/* -------------------------------------------------------------------------
 * Grep state
 * ---------------------------------------------------------------------- */

typedef struct {
    regex_t     rx;
    const char *glob_pat;
    const char *output_mode;   /* "content" | "files_with_matches" | "count" */
    int         before;
    int         after;
    long        head_limit;    /* 0 = unlimited */
    Buf         out;
    long        lines_out;
    long        total_count;   /* total matching lines (all files) */
    RingBuf     ring;
} GrepState;

/*
 * Emit `len` bytes to output. Returns 1 if head_limit was reached, 0 otherwise.
 * A "line" is counted each time we emit a chunk that ends with '\n'.
 * We count by newlines rather than emit calls for accurate head_limit tracking.
 */
static int grep_emit_raw(GrepState *gs, const char *data, size_t len)
{
    if (gs->head_limit > 0 && gs->lines_out >= gs->head_limit) return 1;
    buf_append(&gs->out, data, len);
    /* Count newlines in what we just appended. */
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            gs->lines_out++;
            if (gs->head_limit > 0 && gs->lines_out >= gs->head_limit)
                return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * File search — content mode
 *
 * Emits:
 *   path:lineno:line    for matching lines
 *   path:lineno-line    for context lines
 *   --\n                between non-contiguous output groups
 *
 * Returns 1 if head_limit was hit (caller should stop walking).
 * ---------------------------------------------------------------------- */

static int grep_file_content(GrepState *gs, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    ring_reset(&gs->ring);

    int    lineno       = 0;
    int    last_printed = -1;   /* lineno of last printed line (-1 = none) */
    int    after_remain = 0;    /* after-context lines still to print */
    int    capped       = 0;

    char linebuf[GREP_LINE_MAX];

    while (!capped && fgets(linebuf, (int)sizeof(linebuf), f)) {
        lineno++;
        size_t llen   = strlen(linebuf);
        int    matched = (regexec(&gs->rx, linebuf, 0, NULL, 0) == 0);

        if (matched) {
            int ctx_start = lineno - gs->before;
            if (ctx_start < 1) ctx_start = 1;

            /* Print -- separator if there's a gap since last output. */
            if (last_printed >= 0 && ctx_start > last_printed + 1) {
                capped = grep_emit_raw(gs, "--\n", 3);
                if (capped) break;
            }

            /* Print before-context lines from ring that haven't been printed. */
            for (int k = 0; k < gs->ring.count && !capped; k++) {
                CtxLine *cl = ring_get(&gs->ring, k);
                if (cl->lineno < ctx_start)      continue;
                if (cl->lineno <= last_printed)   continue;
                if (cl->lineno >= lineno)         break;

                char prefix[64];
                int  plen = snprintf(prefix, sizeof(prefix),
                                     "%s:%d-", filepath, cl->lineno);
                capped = grep_emit_raw(gs, prefix, (size_t)plen);
                if (!capped) capped = grep_emit_raw(gs, cl->data, cl->len);
                last_printed = cl->lineno;
            }
            if (capped) break;

            /* Print the matching line. */
            char prefix[64];
            int  plen = snprintf(prefix, sizeof(prefix),
                                 "%s:%d:", filepath, lineno);
            capped = grep_emit_raw(gs, prefix, (size_t)plen);
            if (!capped) capped = grep_emit_raw(gs, linebuf, llen);
            last_printed  = lineno;
            after_remain  = gs->after;

        } else if (after_remain > 0) {
            /* After-context line. */
            char prefix[64];
            int  plen = snprintf(prefix, sizeof(prefix),
                                 "%s:%d-", filepath, lineno);
            capped = grep_emit_raw(gs, prefix, (size_t)plen);
            if (!capped) capped = grep_emit_raw(gs, linebuf, llen);
            last_printed = lineno;
            after_remain--;
        }

        /* Always push to ring buffer (before-context candidates). */
        ring_push(&gs->ring, linebuf, llen, lineno);
    }

    fclose(f);
    return capped;
}

/* -------------------------------------------------------------------------
 * File search — files_with_matches mode
 *
 * Emits the file path once if any line matches. Returns 1 if capped.
 * ---------------------------------------------------------------------- */

static int grep_file_fwm(GrepState *gs, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    char linebuf[GREP_LINE_MAX];
    int  found  = 0;
    int  capped = 0;

    while (!found && fgets(linebuf, (int)sizeof(linebuf), f)) {
        if (regexec(&gs->rx, linebuf, 0, NULL, 0) == 0) {
            found = 1;
        }
    }
    fclose(f);

    if (found) {
        char line[4096];
        int  n = snprintf(line, sizeof(line), "%s\n", filepath);
        capped = grep_emit_raw(gs, line, (size_t)n);
    }
    return capped;
}

/* -------------------------------------------------------------------------
 * File search — count mode
 *
 * Emits "path:N\n" for each file with at least one match. Returns 1 if capped.
 * ---------------------------------------------------------------------- */

static int grep_file_count(GrepState *gs, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    char linebuf[GREP_LINE_MAX];
    long count = 0;

    while (fgets(linebuf, (int)sizeof(linebuf), f)) {
        if (regexec(&gs->rx, linebuf, 0, NULL, 0) == 0)
            count++;
    }
    fclose(f);

    if (count > 0) {
        gs->total_count += count;
        char line[4096];
        int  n = snprintf(line, sizeof(line), "%s:%ld\n", filepath, count);
        return grep_emit_raw(gs, line, (size_t)n);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Recursive directory walk
 * ---------------------------------------------------------------------- */

/*
 * Walk `base_dir/rel` recursively. For each regular file whose path relative
 * to `base_dir` matches `glob_pat`, run the appropriate search function.
 *
 * Returns 1 if head_limit was hit and walking should stop.
 */
static int grep_walk(GrepState *gs, const char *base_dir, const char *rel)
{
    char dir[4096];
    if (rel[0] == '\0')
        snprintf(dir, sizeof(dir), "%s", base_dir);
    else
        snprintf(dir, sizeof(dir), "%s/%s", base_dir, rel);

    DIR *dp = opendir(dir);
    if (!dp) return 0;

    struct dirent *ent;
    int capped = 0;

    while (!capped && (ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* Build path relative to base_dir. */
        char entry_rel[4096];
        if (rel[0] == '\0')
            snprintf(entry_rel, sizeof(entry_rel), "%s", name);
        else
            snprintf(entry_rel, sizeof(entry_rel), "%s/%s", rel, name);

        int is_dir = 0, is_file = 0;

        if (ent->d_type == DT_DIR) {
            is_dir = 1;
        } else if (ent->d_type == DT_REG || ent->d_type == DT_LNK) {
            is_file = 1;
        } else if (ent->d_type == DT_UNKNOWN) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            struct stat st;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) is_dir  = 1;
                if (S_ISREG(st.st_mode)) is_file = 1;
            }
        }

        if (is_dir) {
            /* Skip .git and vendor directories (basic .gitignore support). */
            if (strcmp(name, ".git") == 0 || strcmp(name, "vendor") == 0)
                continue;
            capped = grep_walk(gs, base_dir, entry_rel);

        } else if (is_file) {
            if (!file_matches_glob(gs->glob_pat, entry_rel)) continue;

            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", base_dir, entry_rel);

            if (strcmp(gs->output_mode, "content") == 0)
                capped = grep_file_content(gs, full);
            else if (strcmp(gs->output_mode, "count") == 0)
                capped = grep_file_count(gs, full);
            else
                capped = grep_file_fwm(gs, full);
        }
    }

    closedir(dp);
    return capped;
}

/* -------------------------------------------------------------------------
 * Tool schema
 * ---------------------------------------------------------------------- */

static const char GREP_SCHEMA[] =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"pattern\":    {\"type\":\"string\"},"
       "\"path\":       {\"type\":\"string\","
                        "\"description\":\"Base directory (default: .)\"},"
       "\"glob\":       {\"type\":\"string\","
                        "\"description\":\"Filename filter (e.g. *.c)\"},"
       "\"output_mode\":{\"type\":\"string\","
                        "\"enum\":[\"content\",\"files_with_matches\",\"count\"],"
                        "\"description\":\"Output format (default: files_with_matches)\"},"
       "\"-A\":         {\"type\":\"integer\","
                        "\"description\":\"Lines of after-context\"},"
       "\"-B\":         {\"type\":\"integer\","
                        "\"description\":\"Lines of before-context\"},"
       "\"-C\":         {\"type\":\"integer\","
                        "\"description\":\"Lines of before+after context\"},"
       "\"-i\":         {\"type\":\"boolean\","
                        "\"description\":\"Case-insensitive match\"},"
       "\"head_limit\": {\"type\":\"integer\","
                        "\"description\":\"Max output lines (default 250, 0=unlimited)\"}"
     "},"
     "\"required\":[\"pattern\"]}";

/* -------------------------------------------------------------------------
 * Main handler
 * ---------------------------------------------------------------------- */

ToolResult grep_search(Arena *arena, const char *args_json)
{
    jsmntok_t toks[ARG_TOK_MAX];
    int ntok = args_parse(args_json, toks, ARG_TOK_MAX);
    if (ntok <= 0)
        return err_result(arena, "grep: invalid JSON args");

    /* --- Required: pattern --- */
    char *pattern = args_str(arena, toks, ntok, args_json, "pattern");
    if (!pattern)
        return err_result(arena, "grep: missing required arg 'pattern'");

    /* --- Optional args --- */
    char *path = args_str(arena, toks, ntok, args_json, "path");
    if (!path) path = (char *)".";

    /* Strip trailing slash (but keep plain "/"). */
    size_t plen = strlen(path);
    if (plen > 1 && path[plen - 1] == '/')
        path[plen - 1] = '\0';

    char *glob_pat     = args_str(arena, toks, ntok, args_json, "glob");
    char *output_mode  = args_str(arena, toks, ntok, args_json, "output_mode");
    if (!output_mode) output_mode = (char *)"files_with_matches";

    /* Context: -C sets both before and after; -A/-B override individually. */
    long ctx_C = 0, ctx_A = 0, ctx_B = 0;
    int  has_C = (args_long(toks, ntok, args_json, "-C", &ctx_C) == 0);
    int  has_A = (args_long(toks, ntok, args_json, "-A", &ctx_A) == 0);
    int  has_B = (args_long(toks, ntok, args_json, "-B", &ctx_B) == 0);

    int before = (int)(has_C ? ctx_C : 0);
    int after  = (int)(has_C ? ctx_C : 0);
    if (has_B) before = (int)ctx_B;
    if (has_A) after  = (int)ctx_A;
    if (before < 0) before = 0;
    if (after  < 0) after  = 0;
    if (before > GREP_MAX_CTX) before = GREP_MAX_CTX;
    if (after  > GREP_MAX_CTX) after  = GREP_MAX_CTX;

    int  case_insensitive = 0;
    args_bool(toks, ntok, args_json, "-i", &case_insensitive);

    long head_limit = GREP_HEAD_DEF;
    args_long(toks, ntok, args_json, "head_limit", &head_limit);
    if (head_limit < 0) head_limit = 0;

    /* --- Compile regex --- */
    int rx_flags = REG_EXTENDED | REG_NEWLINE;
    if (case_insensitive) rx_flags |= REG_ICASE;

    regex_t rx;
    int     rx_err = regcomp(&rx, pattern, rx_flags);
    if (rx_err != 0) {
        char errbuf[256];
        regerror(rx_err, &rx, errbuf, sizeof(errbuf));
        char msg[512];
        snprintf(msg, sizeof(msg), "grep: invalid pattern '%s': %s",
                 pattern, errbuf);
        return err_result(arena, msg);
    }

    /* --- Set up grep state --- */
    GrepState gs;
    memset(&gs, 0, sizeof(gs));
    gs.rx          = rx;
    gs.glob_pat    = glob_pat;
    gs.output_mode = output_mode;
    gs.before      = before;
    gs.after       = after;
    gs.head_limit  = head_limit;
    gs.lines_out   = 0;
    gs.total_count = 0;
    buf_init(&gs.out);

    /* Allocate ring buffer (only needed for content mode with before-context). */
    int ring_cap = (before > 0) ? before : 1;
    ring_init(&gs.ring, ring_cap);

    /* --- Walk and search --- */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        /* Path is a single file — search it directly. */
        if (strcmp(output_mode, "content") == 0)
            grep_file_content(&gs, path);
        else if (strcmp(output_mode, "count") == 0)
            grep_file_count(&gs, path);
        else
            grep_file_fwm(&gs, path);
    } else {
        grep_walk(&gs, path, "");
    }

    /* --- Build result --- */
    regfree(&rx);
    ring_free(&gs.ring);

    size_t rlen   = gs.out.len;
    char  *result = arena_alloc(arena, rlen + 1);
    if (rlen > 0)
        memcpy(result, buf_str(&gs.out), rlen);
    result[rlen] = '\0';
    buf_destroy(&gs.out);

    return ok_result(result, rlen);
}

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

void grep_register(void)
{
    tool_register("grep", GREP_SCHEMA, grep_search);
}
