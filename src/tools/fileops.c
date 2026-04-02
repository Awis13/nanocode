/*
 * fileops.c — file operation tools: read_file, write_file, edit_file, glob
 *
 * All result memory is arena-allocated. Intermediate buffers for accumulation
 * (glob results, line-range reads) use heap-backed Buf, then a final copy
 * goes into the arena so the caller gets a clean arena-owned result.
 */

#define JSMN_STATIC
#include "jsmn.h"

#include "executor.h"
#include "fileops.h"
#include "../util/arena.h"
#include "../util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>   /* getcwd */
#include <limits.h>   /* PATH_MAX */

/* Maximum bytes returned by read_file without an explicit range. */
#define READ_MAX_BYTES  (50 * 1024)

/* Maximum file size edit_file will load into the arena. */
#define EDIT_MAX_BYTES  (1024 * 1024)

/* Maximum tokens when parsing tool argument objects. */
#define ARG_TOK_MAX     256

/* -------------------------------------------------------------------------
 * Per-session resource limits (set once at startup via fileops_set_limits)
 * ---------------------------------------------------------------------- */

static long s_max_file_size     = 10 * 1024 * 1024;  /* 10 MB */
static int  s_max_files_created = 50;
static int  s_files_created     = 0;

void fileops_set_limits(long max_file_size_bytes, int max_files_created)
{
    s_max_file_size     = max_file_size_bytes;
    s_max_files_created = max_files_created;
    s_files_created     = 0;
}

/* -------------------------------------------------------------------------
 * Path safety: canonicalize path and restrict to working directory.
 *
 * Resolves . and .. components purely by string manipulation (no filesystem
 * access), so it works for paths that do not yet exist (e.g. new write targets).
 * Returns an arena-allocated absolute canonical path if it is within the CWD,
 * or NULL if the path escapes the working directory or is malformed.
 * ---------------------------------------------------------------------- */

static char *path_restrict(Arena *arena, const char *path)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return NULL;

    /* Build absolute path. */
    char abs[PATH_MAX * 2];
    if (path[0] == '/') {
        if ((size_t)snprintf(abs, sizeof(abs), "%s", path) >= sizeof(abs))
            return NULL;
    } else {
        if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", cwd, path) >= sizeof(abs))
            return NULL;
    }

    /*
     * Walk each component, resolving . and ..
     * stack[i] = write position in buf just before component i was appended,
     * so popping (..) restores pos to that value.
     */
    char   buf[PATH_MAX];
    size_t stack[512];   /* supports up to 512 path components */
    int    depth = 0;

    buf[0] = '/';
    size_t pos = 1;

    char *p = abs;
    while (*p) {
        if (*p == '/') { p++; continue; }

        char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);

        if (clen == 0) {
            /* empty component (double slash) */
        } else if (clen == 1 && p[0] == '.') {
            /* "." — skip */
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            /* ".." — pop */
            if (depth > 0)
                pos = stack[--depth];
        } else {
            /* Normal component — push. */
            if (depth >= (int)(sizeof(stack) / sizeof(stack[0])))
                return NULL;  /* path too deep */
            if (pos + clen + 2 > sizeof(buf))
                return NULL;  /* overflow */
            stack[depth++] = pos;
            memcpy(buf + pos, p, clen);
            pos += clen;
            buf[pos++] = '/';
        }

        p = slash ? slash : (p + clen);
    }

    /* Strip trailing slash (but keep root as "/"). */
    if (pos > 1) pos--;
    buf[pos] = '\0';

    /* Require that canonical path is within (or equal to) CWD. */
    size_t cwd_len = strlen(cwd);
    if (strncmp(buf, cwd, cwd_len) != 0 ||
        (buf[cwd_len] != '/' && buf[cwd_len] != '\0'))
        return NULL;

    char *result = arena_alloc(arena, pos + 1);
    memcpy(result, buf, pos + 1);
    return result;
}

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
 * JSON string unescaping
 *
 * Operates in-place on the raw token content (no surrounding quotes).
 * Always produces <= input bytes; returns new length.
 * ---------------------------------------------------------------------- */

static int json_unescape(char *buf, int len)
{
    int src = 0, dst = 0;

    while (src < len) {
        if (buf[src] != '\\') {
            buf[dst++] = buf[src++];
            continue;
        }
        /* consume the backslash */
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
            /* \uXXXX — copy verbatim (no full UTF-16 decode needed here) */
            buf[dst++] = '\\';
            buf[dst++] = 'u';
            src++; /* consume 'u' */
            /* copy up to 4 hex digits */
            for (int k = 0; k < 4 && src < len; k++, src++)
                buf[dst++] = buf[src];
            break;
        default:
            buf[dst++] = buf[src++];
            break;
        }
    }

    buf[dst] = '\0';
    return dst;
}

/* -------------------------------------------------------------------------
 * Local JSON argument extractors (flat object only)
 * ---------------------------------------------------------------------- */

/* Parse args_json into toks[]. Returns token count, or 0 on error. */
static int args_parse(const char *args_json, jsmntok_t *toks, int max_toks)
{
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, args_json, strlen(args_json), toks, max_toks);
    return r < 0 ? 0 : r;
}

/*
 * Extract a string value for `key`, unescape it, and allocate from `arena`.
 * Returns NULL if the key is absent or the value is not a string.
 */
static char *args_str(Arena *arena, jsmntok_t *toks, int ntok,
                      const char *json, const char *key)
{
    int klen = (int)strlen(key);

    for (int i = 1; i < ntok - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        if (toks[i].end - toks[i].start != klen) continue;
        if (memcmp(json + toks[i].start, key, (size_t)klen) != 0) continue;
        if (toks[i + 1].type != JSMN_STRING) continue;

        int vlen  = toks[i + 1].end - toks[i + 1].start;
        char *out = arena_alloc(arena, (size_t)vlen + 1);
        memcpy(out, json + toks[i + 1].start, (size_t)vlen);
        int actual = json_unescape(out, vlen);
        out[actual] = '\0';
        return out;
    }
    return NULL;
}

/*
 * Extract a long integer primitive for `key`.
 * Returns 0 on success with *out set; -1 if absent or not numeric.
 */
static int args_long(jsmntok_t *toks, int ntok,
                     const char *json, const char *key, long *out)
{
    int klen = (int)strlen(key);

    for (int i = 1; i < ntok - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        if (toks[i].end - toks[i].start != klen) continue;
        if (memcmp(json + toks[i].start, key, (size_t)klen) != 0) continue;
        if (toks[i + 1].type != JSMN_PRIMITIVE) continue;

        int vlen = toks[i + 1].end - toks[i + 1].start;
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

/*
 * Extract a boolean primitive for `key`.
 * Returns 0 on success with *out set to 1 (true) or 0 (false); -1 if absent.
 */
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
 * mkdir -p
 * ---------------------------------------------------------------------- */

/* Create all components of `path` up to (but not including) the last '/'. */
static int ensure_parent_dirs(const char *filepath)
{
    char   buf[4096];
    size_t len = strlen(filepath);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, filepath, len + 1);

    char *last_slash = strrchr(buf, '/');
    if (!last_slash) return 0; /* no directory component — nothing to create */
    *last_slash = '\0';

    /* Walk the path, creating each component. */
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* -------------------------------------------------------------------------
 * Glob pattern matching with ** support
 *
 * Supports:
 *   *    — any sequence of non-slash characters
 *   **   — any sequence including slashes (zero or more path components)
 *   ?    — any single non-slash character
 *
 * Falls back to character-by-character matching for literal segments.
 * ---------------------------------------------------------------------- */

static int glob_match(const char *pat, const char *str)
{
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            /* Advance past the double-star and an optional following slash. */
            const char *rest = pat + 2;
            if (*rest == '/') rest++;

            /* ** at end of pattern matches everything. */
            if (*rest == '\0') return 1;

            /* Try matching `rest` at the current position and at every
             * position after a directory separator. */
            const char *p = str;
            for (;;) {
                if (glob_match(rest, p)) return 1;
                p = strchr(p, '/');
                if (!p) break;
                p++; /* skip the '/' */
            }
            return 0;

        } else if (*pat == '*') {
            /* Single * — does not cross '/'. */
            const char *rest = pat + 1;
            const char *p    = str;
            while (*p && *p != '/') {
                if (glob_match(rest, p)) return 1;
                p++;
            }
            return glob_match(rest, p);

        } else if (*pat == '?') {
            if (*str == '\0' || *str == '/') return 0;
            pat++;
            str++;

        } else {
            if (*pat != *str) return 0;
            pat++;
            str++;
        }
    }
    return *str == '\0';
}

/* -------------------------------------------------------------------------
 * Recursive directory walk for glob
 * ---------------------------------------------------------------------- */

/*
 * Walk `base_dir/rel` recursively. For each regular file whose path relative
 * to `base_dir` matches `pattern`, append "base_dir/rel_path\n" to `out`.
 *
 * `rel` is "" at the top level, "subdir" or "subdir/deeper" deeper down.
 */
static void glob_walk(const char *base_dir, const char *rel,
                      const char *pattern, Buf *out)
{
    char dir[4096];
    if (rel[0] == '\0')
        snprintf(dir, sizeof(dir), "%s", base_dir);
    else
        snprintf(dir, sizeof(dir), "%s/%s", base_dir, rel);

    DIR *dp = opendir(dir);
    if (!dp) return;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* Build the path component relative to base_dir. */
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
            /* Filesystems that don't populate d_type — fall back to stat(). */
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            struct stat st;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode))  is_dir  = 1;
                if (S_ISREG(st.st_mode))  is_file = 1;
            }
        }

        if (is_dir) {
            glob_walk(base_dir, entry_rel, pattern, out);
        } else if (is_file) {
            if (glob_match(pattern, entry_rel)) {
                char line[4096];
                snprintf(line, sizeof(line), "%s/%s\n", base_dir, entry_rel);
                buf_append_str(out, line);
            }
        }
    }

    closedir(dp);
}

/* -------------------------------------------------------------------------
 * Tool: read_file
 * ---------------------------------------------------------------------- */

static const char READ_SCHEMA[] =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"path\":{\"type\":\"string\"},"
       "\"offset\":{\"type\":\"integer\",\"description\":\"First line (0-indexed)\"},"
       "\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return\"}"
     "},"
     "\"required\":[\"path\"]}";

ToolResult fileops_read(Arena *arena, const char *args_json)
{
    jsmntok_t toks[ARG_TOK_MAX];
    int ntok = args_parse(args_json, toks, ARG_TOK_MAX);
    if (ntok <= 0)
        return err_result(arena, "read_file: invalid JSON args");

    char *path = args_str(arena, toks, ntok, args_json, "path");
    if (!path)
        return err_result(arena, "read_file: missing required arg 'path'");
    path = path_restrict(arena, path);
    if (!path)
        return err_result(arena, "read_file: path outside working directory");

    long offset = 0;
    long limit  = -1; /* -1 means no limit */
    args_long(toks, ntok, args_json, "offset", &offset);
    args_long(toks, ntok, args_json, "limit",  &limit);

    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[4096];
        snprintf(msg, sizeof(msg), "read_file: cannot open '%s': %s",
                 path, strerror(errno));
        return err_result(arena, msg);
    }

    if (offset == 0 && limit < 0) {
        /* Simple read — up to READ_MAX_BYTES. */
        char *buf = arena_alloc(arena, READ_MAX_BYTES + 1);
        size_t n  = fread(buf, 1, READ_MAX_BYTES, f);
        fclose(f);
        buf[n] = '\0';
        return ok_result(buf, n);
    }

    /* Line-range read: skip `offset` lines, return at most `limit` lines. */
    char line_buf[65536];
    long cur_line = 0;
    Buf  acc;
    buf_init(&acc);

    while (fgets(line_buf, (int)sizeof(line_buf), f)) {
        if (cur_line >= offset) {
            if (limit >= 0 && (cur_line - offset) >= limit) break;
            buf_append_str(&acc, line_buf);
        }
        cur_line++;
    }
    fclose(f);

    size_t rlen   = acc.len;
    char  *result = arena_alloc(arena, rlen + 1);
    if (rlen > 0)
        memcpy(result, buf_str(&acc), rlen);
    result[rlen] = '\0';
    buf_destroy(&acc);

    return ok_result(result, rlen);
}

/* -------------------------------------------------------------------------
 * Tool: write_file
 * ---------------------------------------------------------------------- */

static const char WRITE_SCHEMA[] =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"path\":{\"type\":\"string\"},"
       "\"content\":{\"type\":\"string\"}"
     "},"
     "\"required\":[\"path\",\"content\"]}";

ToolResult fileops_write(Arena *arena, const char *args_json)
{
    jsmntok_t toks[ARG_TOK_MAX];
    int ntok = args_parse(args_json, toks, ARG_TOK_MAX);
    if (ntok <= 0)
        return err_result(arena, "write_file: invalid JSON args");

    char *path    = args_str(arena, toks, ntok, args_json, "path");
    char *content = args_str(arena, toks, ntok, args_json, "content");

    if (!path)    return err_result(arena, "write_file: missing required arg 'path'");
    if (!content) return err_result(arena, "write_file: missing required arg 'content'");

    path = path_restrict(arena, path);
    if (!path)
        return err_result(arena, "write_file: path outside working directory");

    /* Enforce file size limit. */
    size_t clen = strlen(content);
    if (s_max_file_size > 0 && (long)clen > s_max_file_size) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"file too large\",\"limit_bytes\":%ld}",
                 s_max_file_size);
        return err_result(arena, msg);
    }

    /* Enforce per-session new-file limit. */
    struct stat st_check;
    int is_new_file = (stat(path, &st_check) != 0);
    if (is_new_file && s_max_files_created > 0 &&
        s_files_created >= s_max_files_created) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"session file limit reached\",\"limit\":%d}",
                 s_max_files_created);
        return err_result(arena, msg);
    }

    if (ensure_parent_dirs(path) < 0) {
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "write_file: cannot create parent dirs for '%s': %s",
                 path, strerror(errno));
        return err_result(arena, msg);
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "write_file: cannot open '%s' for writing: %s",
                 path, strerror(errno));
        return err_result(arena, msg);
    }

    size_t written = fwrite(content, 1, clen, f);
    int    wr_err  = ferror(f);
    fclose(f);

    if (wr_err || written != clen) {
        char msg[4096];
        snprintf(msg, sizeof(msg), "write_file: write error on '%s'", path);
        return err_result(arena, msg);
    }

    if (is_new_file)
        s_files_created++;

    char *ok = arena_alloc(arena, 128);
    int   n  = snprintf(ok, 128, "wrote %zu bytes to %s", clen, path);
    return ok_result(ok, (size_t)n);
}

/* -------------------------------------------------------------------------
 * Tool: edit_file
 * ---------------------------------------------------------------------- */

static const char EDIT_SCHEMA[] =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"path\":{\"type\":\"string\"},"
       "\"old_string\":{\"type\":\"string\"},"
       "\"new_string\":{\"type\":\"string\"},"
       "\"replace_all\":{\"type\":\"boolean\"}"
     "},"
     "\"required\":[\"path\",\"old_string\",\"new_string\"]}";

ToolResult fileops_edit(Arena *arena, const char *args_json)
{
    jsmntok_t toks[ARG_TOK_MAX];
    int ntok = args_parse(args_json, toks, ARG_TOK_MAX);
    if (ntok <= 0)
        return err_result(arena, "edit_file: invalid JSON args");

    char *path       = args_str(arena, toks, ntok, args_json, "path");
    char *old_string = args_str(arena, toks, ntok, args_json, "old_string");
    char *new_string = args_str(arena, toks, ntok, args_json, "new_string");

    if (!path)       return err_result(arena, "edit_file: missing required arg 'path'");
    if (!old_string) return err_result(arena, "edit_file: missing required arg 'old_string'");
    if (!new_string) return err_result(arena, "edit_file: missing required arg 'new_string'");

    path = path_restrict(arena, path);
    if (!path)
        return err_result(arena, "edit_file: path outside working directory");

    int replace_all = 0;
    args_bool(toks, ntok, args_json, "replace_all", &replace_all);

    size_t old_len = strlen(old_string);
    size_t new_len = strlen(new_string);

    if (old_len == 0)
        return err_result(arena, "edit_file: old_string must not be empty");

    /* Read file into arena. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[4096];
        snprintf(msg, sizeof(msg), "edit_file: cannot open '%s': %s",
                 path, strerror(errno));
        return err_result(arena, msg);
    }

    char  *file_buf = arena_alloc(arena, EDIT_MAX_BYTES + 1);
    size_t file_len = fread(file_buf, 1, EDIT_MAX_BYTES, f);
    fclose(f);
    file_buf[file_len] = '\0';

    /* Count occurrences of old_string. */
    int         count = 0;
    const char *scan  = file_buf;
    while ((scan = strstr(scan, old_string)) != NULL) {
        count++;
        scan += old_len;
    }

    if (count == 0) {
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "edit_file: old_string not found in '%s'", path);
        return err_result(arena, msg);
    }

    if (count > 1 && !replace_all) {
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "edit_file: old_string appears %d times in '%s' "
                 "(use replace_all=true to replace all occurrences)",
                 count, path);
        return err_result(arena, msg);
    }

    /* Build replacement. */
    size_t n_replace = replace_all ? (size_t)count : 1;
    size_t out_len   = file_len
                     + n_replace * new_len
                     - n_replace * old_len;

    /* Enforce file size limit on resulting content. */
    if (s_max_file_size > 0 && (long)out_len > s_max_file_size) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"file too large\",\"limit_bytes\":%ld}",
                 s_max_file_size);
        return err_result(arena, msg);
    }

    char  *out_buf = arena_alloc(arena, out_len + 1);
    char  *dst     = out_buf;
    const char *src = file_buf;

    for (size_t i = 0; i < n_replace; i++) {
        const char *found      = strstr(src, old_string);
        size_t      prefix_len = (size_t)(found - src);

        memcpy(dst, src, prefix_len);
        dst += prefix_len;

        memcpy(dst, new_string, new_len);
        dst += new_len;

        src = found + old_len;
    }

    /* Append the remainder. */
    size_t tail = (size_t)((file_buf + file_len) - src);
    memcpy(dst, src, tail);
    dst   += tail;
    *dst   = '\0';

    /* Write back. */
    f = fopen(path, "wb");
    if (!f) {
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "edit_file: cannot open '%s' for writing: %s",
                 path, strerror(errno));
        return err_result(arena, msg);
    }
    size_t wr    = fwrite(out_buf, 1, out_len, f);
    int    fwerr = ferror(f);
    fclose(f);

    if (fwerr || wr != out_len) {
        char msg[4096];
        snprintf(msg, sizeof(msg), "edit_file: write error on '%s'", path);
        return err_result(arena, msg);
    }

    char *ok = arena_alloc(arena, 128);
    int   n  = snprintf(ok, 128,
                        "replaced %zu occurrence(s) in %s", n_replace, path);
    return ok_result(ok, (size_t)n);
}

/* -------------------------------------------------------------------------
 * Tool: glob
 * ---------------------------------------------------------------------- */

static const char GLOB_SCHEMA[] =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"pattern\":{\"type\":\"string\","
         "\"description\":\"Glob pattern; ** matches across directories\"},"
       "\"path\":{\"type\":\"string\","
         "\"description\":\"Base directory (default: .)\"}"
     "},"
     "\"required\":[\"pattern\"]}";

ToolResult fileops_glob(Arena *arena, const char *args_json)
{
    jsmntok_t toks[ARG_TOK_MAX];
    int ntok = args_parse(args_json, toks, ARG_TOK_MAX);
    if (ntok <= 0)
        return err_result(arena, "glob: invalid JSON args");

    char *pattern = args_str(arena, toks, ntok, args_json, "pattern");
    if (!pattern)
        return err_result(arena, "glob: missing required arg 'pattern'");

    char *base = args_str(arena, toks, ntok, args_json, "path");
    if (!base)
        base = (char *)".";

    base = path_restrict(arena, base);
    if (!base)
        return err_result(arena, "glob: base directory outside working directory");

    /* Strip a single trailing slash (but keep plain "/"). */
    size_t blen = strlen(base);
    if (blen > 1 && base[blen - 1] == '/')
        base[blen - 1] = '\0';

    /* Collect matches via heap-backed Buf. */
    Buf result;
    buf_init(&result);
    glob_walk(base, "", pattern, &result);

    /* Copy to arena, stripping the trailing newline if present. */
    size_t rlen = result.len;
    if (rlen > 0 && buf_str(&result)[rlen - 1] == '\n')
        rlen--;

    char *out = arena_alloc(arena, rlen + 1);
    if (rlen > 0)
        memcpy(out, buf_str(&result), rlen);
    out[rlen] = '\0';
    buf_destroy(&result);

    return ok_result(out, rlen);
}

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

void fileops_register_all(void)
{
    tool_register("read_file",  READ_SCHEMA,  fileops_read);
    tool_register("write_file", WRITE_SCHEMA, fileops_write);
    tool_register("edit_file",  EDIT_SCHEMA,  fileops_edit);
    tool_register("glob",       GLOB_SCHEMA,  fileops_glob);
}
