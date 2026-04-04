/*
 * history.c — conversation persistence (CMP-143)
 *
 * Auto-save: append JSONL turns to ~/.nanocode/conversations/<ts>-<title>.jsonl
 * Resume:    list recent files, load selected one into arena Conversation
 * Search:    streaming case-insensitive scan across all history files
 * Export:    render Conversation to Markdown
 *
 * All heap allocations in this file are malloc/free — HistoryCtx is a
 * long-lived object tied to the session, not an arena.  history_load uses
 * malloc internally for I/O buffers (freed before return); the Conversation
 * itself goes into the caller-supplied arena.
 */

#define _POSIX_C_SOURCE 200809L

#define JSMN_STATIC
#include "../../vendor/jsmn/jsmn.h"

#include "../../include/history.h"
#include "../agent/conversation.h"
#include "../util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Max line length when reading JSONL lines. */
#define HIST_LINE_MAX   (64 * 1024)

/* Max characters of title used in filename. */
#define HIST_TITLE_MAX  50

/* Max path length for internal use. */
#define HIST_PATH_MAX   512

/* jsmn token budget for one JSONL line. */
#define HIST_JSMN_TOKS  64

/* -------------------------------------------------------------------------
 * HistoryCtx
 * ---------------------------------------------------------------------- */

struct HistoryCtx {
    int  fd;                       /* open file descriptor (O_APPEND) */
    char path[HIST_PATH_MAX];      /* absolute path of the JSONL file  */
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Resolve ~/.nanocode/conversations/ into `out` (capacity `cap`).
 * Returns 0 on success, -1 on failure.
 */
static int hist_conversations_dir(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || !home[0])
        return -1;

    int n = snprintf(out, cap, "%s/.nanocode/conversations", home);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

/*
 * mkdir -p equivalent for a two-level path: create parent then leaf.
 * Returns 0 on success (or if already exists), -1 on error.
 */
static int hist_mkdirp(const char *dir)
{
    /* Try creating leaf directly first (common case). */
    if (mkdir(dir, 0700) == 0 || errno == EEXIST)
        return 0;
    if (errno != ENOENT)
        return -1;

    /* Parent doesn't exist — create it. */
    char parent[HIST_PATH_MAX];
    strncpy(parent, dir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';

    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return -1;
    *slash = '\0';

    if (mkdir(parent, 0700) != 0 && errno != EEXIST)
        return -1;

    /* Now create leaf. */
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * Sanitize `title` into a filename-safe string in `out` (capacity `cap`).
 * Copies at most HIST_TITLE_MAX characters, replacing non-alphanumeric
 * chars with '-', collapsing runs of '-', stripping leading/trailing '-'.
 */
static void hist_sanitize_title(const char *title, char *out, size_t cap)
{
    if (!title || !out || cap < 2) {
        if (out && cap >= 2) { out[0] = 'x'; out[1] = '\0'; }
        return;
    }

    size_t wi   = 0;
    int    dash = 0;           /* 1 if last written char was '-' */

    for (size_t i = 0; title[i] && wi + 1 < cap && i < HIST_TITLE_MAX; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c)) {
            out[wi++] = (char)tolower(c);
            dash = 0;
        } else {
            if (!dash && wi > 0) {   /* don't start with '-' */
                out[wi++] = '-';
                dash = 1;
            }
        }
    }
    /* Strip trailing '-'. */
    while (wi > 0 && out[wi - 1] == '-')
        wi--;

    out[wi] = '\0';
    if (wi == 0) { out[0] = 'x'; out[1] = '\0'; }
}

/*
 * Write a JSON-escaped string to a heap buffer starting at offset `*pos`
 * in `buf` (capacity `cap`).  Includes surrounding quotes.
 * Returns 0 on success, -1 if buffer is too small.
 */
static int hist_json_str(char *buf, size_t cap, size_t *pos, const char *s)
{
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '"';
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            if (*pos + 8 >= cap) return -1;
            if (*p == '"')       { buf[(*pos)++] = '\\'; buf[(*pos)++] = '"';  }
            else if (*p == '\\') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '\\'; }
            else if (*p == '\n') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'n';  }
            else if (*p == '\r') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'r';  }
            else if (*p == '\t') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 't';  }
            else if (*p < 0x20) {
                int n = snprintf(buf + *pos, cap - *pos, "\\u00%02x", (unsigned)*p);
                if (n < 0 || (size_t)n >= cap - *pos) return -1;
                *pos += (size_t)n;
            } else {
                buf[(*pos)++] = (char)*p;
            }
        }
    }
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '"';
    return 0;
}

/*
 * Case-insensitive substring search.
 * Returns pointer to first occurrence of needle in haystack, or NULL.
 */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)needle[0])) {
            size_t i;
            for (i = 0; i < nlen; i++) {
                if (!haystack[i]) break;
                if (tolower((unsigned char)haystack[i]) !=
                    tolower((unsigned char)needle[i]))
                    break;
            }
            if (i == nlen) return haystack;
        }
    }
    return NULL;
}

/*
 * Write `len` bytes of `data` to `fd`, retrying on EINTR.
 */
static int hist_write(int fd, const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * history_open
 * ---------------------------------------------------------------------- */

HistoryCtx *history_open(const char *title)
{
    char dir[HIST_PATH_MAX];
    if (hist_conversations_dir(dir, sizeof(dir)) != 0)
        return NULL;

    if (hist_mkdirp(dir) != 0)
        return NULL;

    /* Build filename: <YYYYmmdd-HHMMSS>-<title>.jsonl */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm_utc);

    char safe_title[HIST_TITLE_MAX + 4];
    hist_sanitize_title(title ? title : "session", safe_title, sizeof(safe_title));

    char path[HIST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s-%s.jsonl", dir, ts, safe_title);
    if (n < 0 || (size_t)n >= sizeof(path))
        return NULL;

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0)
        return NULL;

    HistoryCtx *hx = malloc(sizeof(HistoryCtx));
    if (!hx) {
        close(fd);
        return NULL;
    }
    hx->fd = fd;
    strncpy(hx->path, path, sizeof(hx->path) - 1);
    hx->path[sizeof(hx->path) - 1] = '\0';
    return hx;
}

/* -------------------------------------------------------------------------
 * history_append_turn
 * ---------------------------------------------------------------------- */

void history_append_turn(HistoryCtx *hx, const char *role,
                         const char *content, int tokens, int is_tool)
{
    if (!hx || hx->fd < 0) return;

    /* Estimate buffer size: content * 6 (worst-case JSON escaping) + overhead. */
    size_t content_len = content ? strlen(content) : 0;
    size_t buf_cap = content_len * 6 + 256;
    if (buf_cap < 512) buf_cap = 512;

    char *buf = malloc(buf_cap);
    if (!buf) return;

    /* Build timestamp. */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    /* Serialize: {"role":"...","content":"...","tokens":N,"timestamp":"..."} */
    size_t pos = 0;
    if (pos + 1 >= buf_cap) { free(buf); return; }
    buf[pos++] = '{';

    /* "role": */
    const char *role_key = "\"role\":";
    if (pos + strlen(role_key) >= buf_cap) { free(buf); return; }
    memcpy(buf + pos, role_key, strlen(role_key)); pos += strlen(role_key);
    if (hist_json_str(buf, buf_cap, &pos, role) != 0) { free(buf); return; }

    /* ,"content": */
    const char *content_key = ",\"content\":";
    if (pos + strlen(content_key) >= buf_cap) { free(buf); return; }
    memcpy(buf + pos, content_key, strlen(content_key)); pos += strlen(content_key);
    if (hist_json_str(buf, buf_cap, &pos, content) != 0) { free(buf); return; }

    /* ,"tokens":N */
    if (pos + 32 >= buf_cap) { free(buf); return; }
    int n = snprintf(buf + pos, buf_cap - pos, ",\"tokens\":%d", tokens);
    if (n < 0 || (size_t)n >= buf_cap - pos) { free(buf); return; }
    pos += (size_t)n;

    /* ,"is_tool":N */
    if (pos + 16 >= buf_cap) { free(buf); return; }
    n = snprintf(buf + pos, buf_cap - pos, ",\"is_tool\":%d", is_tool ? 1 : 0);
    if (n < 0 || (size_t)n >= buf_cap - pos) { free(buf); return; }
    pos += (size_t)n;

    /* ,"timestamp":"..." */
    const char *ts_key = ",\"timestamp\":";
    if (pos + strlen(ts_key) >= buf_cap) { free(buf); return; }
    memcpy(buf + pos, ts_key, strlen(ts_key)); pos += strlen(ts_key);
    if (hist_json_str(buf, buf_cap, &pos, ts) != 0) { free(buf); return; }

    /* }\n */
    if (pos + 2 >= buf_cap) { free(buf); return; }
    buf[pos++] = '}';
    buf[pos++] = '\n';

    hist_write(hx->fd, buf, pos);
    free(buf);
}

/* -------------------------------------------------------------------------
 * history_close / history_path
 * ---------------------------------------------------------------------- */

void history_close(HistoryCtx *hx)
{
    if (!hx) return;
    if (hx->fd >= 0) close(hx->fd);
    free(hx);
}

const char *history_path(const HistoryCtx *hx)
{
    return hx ? hx->path : NULL;
}

/* -------------------------------------------------------------------------
 * history_list_recent
 * ---------------------------------------------------------------------- */

typedef struct {
    char   path[HIST_PATH_MAX];
    time_t mtime;
} HistEntry;

static int hist_entry_cmp(const void *a, const void *b)
{
    const HistEntry *ea = (const HistEntry *)a;
    const HistEntry *eb = (const HistEntry *)b;
    /* Descending by mtime — newest first. */
    if (eb->mtime > ea->mtime) return 1;
    if (eb->mtime < ea->mtime) return -1;
    return 0;
}

int history_list_recent(char **paths, int maxn)
{
    if (!paths || maxn <= 0) return 0;

    char dir[HIST_PATH_MAX];
    if (hist_conversations_dir(dir, sizeof(dir)) != 0)
        return 0;

    DIR *dp = opendir(dir);
    if (!dp) return 0;

    /* Collect all .jsonl entries. */
    int      cap     = 64;
    int      count   = 0;
    HistEntry *entries = malloc((size_t)cap * sizeof(HistEntry));
    if (!entries) { closedir(dp); return 0; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        /* Skip non-.jsonl files. */
        size_t nlen = strlen(de->d_name);
        if (nlen < 6 || strcmp(de->d_name + nlen - 6, ".jsonl") != 0)
            continue;

        char full[HIST_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (count >= cap) {
            int new_cap = cap * 2;
            HistEntry *tmp = realloc(entries, (size_t)new_cap * sizeof(HistEntry));
            if (!tmp) break;
            entries = tmp;
            cap = new_cap;
        }

        strncpy(entries[count].path, full, HIST_PATH_MAX - 1);
        entries[count].path[HIST_PATH_MAX - 1] = '\0';
        entries[count].mtime = st.st_mtime;
        count++;
    }
    closedir(dp);

    qsort(entries, (size_t)count, sizeof(HistEntry), hist_entry_cmp);

    int ret = count < maxn ? count : maxn;
    for (int i = 0; i < ret; i++) {
        paths[i] = strdup(entries[i].path);
        if (!paths[i]) { ret = i; break; }
    }

    free(entries);
    return ret;
}

/* -------------------------------------------------------------------------
 * history_load
 * ---------------------------------------------------------------------- */

/*
 * Extract the string value of key `key` from a flat JSON object in `line`.
 * Writes the unescaped value into `out` (capacity `cap`).
 * Returns 1 on success, 0 if key not found or wrong type.
 */
static int hist_extract_str(const char *line, size_t linelen,
                            const jsmntok_t *toks, int ntok,
                            const char *key, char *out, size_t cap)
{
    int size = toks[0].size;
    int i = 1;
    for (int k = 0; k < size && i + 1 < ntok; k++) {
        /* Key token. */
        if (toks[i].type == JSMN_STRING) {
            int klen = toks[i].end - toks[i].start;
            if (klen == (int)strlen(key) &&
                memcmp(line + toks[i].start, key, (size_t)klen) == 0) {
                /* Value token. */
                int vi = i + 1;
                if (vi < ntok && toks[vi].type == JSMN_STRING) {
                    int vlen = toks[vi].end - toks[vi].start;
                    if ((size_t)vlen >= cap) vlen = (int)cap - 1;
                    /* Unescape manually (simple — handle \n \\ \"). */
                    int wi = 0;
                    for (int j = 0; j < vlen && (size_t)wi + 1 < cap; j++) {
                        char c = line[toks[vi].start + j];
                        if (c == '\\' && j + 1 < vlen) {
                            j++;
                            char e = line[toks[vi].start + j];
                            switch (e) {
                            case '"':  out[wi++] = '"';  break;
                            case '\\': out[wi++] = '\\'; break;
                            case 'n':  out[wi++] = '\n'; break;
                            case 'r':  out[wi++] = '\r'; break;
                            case 't':  out[wi++] = '\t'; break;
                            default:   out[wi++] = e;    break;
                            }
                        } else {
                            out[wi++] = c;
                        }
                    }
                    out[wi] = '\0';
                    return 1;
                }
                return 0;
            }
        }
        /* Skip to next key-value pair. */
        i += 2;
    }
    (void)linelen;
    return 0;
}

/*
 * Extract the integer value of key `key` from a flat JSON object in `line`.
 * Writes the parsed value into `out`.
 * Returns 1 on success, 0 if key not found or value is not a primitive.
 */
static int hist_extract_int(const char *line, size_t linelen,
                             const jsmntok_t *toks, int ntok,
                             const char *key, int *out)
{
    int size = toks[0].size;
    int i = 1;
    for (int k = 0; k < size && i + 1 < ntok; k++) {
        if (toks[i].type == JSMN_STRING) {
            int klen = toks[i].end - toks[i].start;
            if (klen == (int)strlen(key) &&
                memcmp(line + toks[i].start, key, (size_t)klen) == 0) {
                int vi = i + 1;
                if (vi < ntok && toks[vi].type == JSMN_PRIMITIVE) {
                    char num[16];
                    int nlen = toks[vi].end - toks[vi].start;
                    if (nlen >= (int)sizeof(num)) nlen = (int)sizeof(num) - 1;
                    memcpy(num, line + toks[vi].start, (size_t)nlen);
                    num[nlen] = '\0';
                    *out = atoi(num);
                    return 1;
                }
                return 0;
            }
        }
        i += 2;
    }
    (void)linelen;
    return 0;
}

Conversation *history_load(Arena *arena, const char *path)
{
    if (!arena || !path) return NULL;

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Conversation *conv = conv_new(arena);
    if (!conv) { fclose(fp); return NULL; }

    char     *line  = malloc(HIST_LINE_MAX);
    jsmntok_t *toks = malloc(HIST_JSMN_TOKS * sizeof(jsmntok_t));
    char      *role = malloc(64);
    char      *content = malloc(HIST_LINE_MAX);

    if (!line || !toks || !role || !content) {
        free(line); free(toks); free(role); free(content);
        fclose(fp);
        return NULL;
    }

    while (fgets(line, HIST_LINE_MAX, fp) != NULL) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        jsmn_parser parser;
        jsmn_init(&parser);
        int ntok = jsmn_parse(&parser, line, len, toks, HIST_JSMN_TOKS);
        if (ntok < 1 || toks[0].type != JSMN_OBJECT) continue;

        if (!hist_extract_str(line, len, toks, ntok, "role",    role,    64)         ||
            !hist_extract_str(line, len, toks, ntok, "content", content, HIST_LINE_MAX))
            continue;

        conv_add(conv, role, content);
        /* Restore is_tool flag — conv_add initialises it to 0. */
        int is_tool = 0;
        hist_extract_int(line, len, toks, ntok, "is_tool", &is_tool);
        if (is_tool && conv->nturn > 0)
            conv->turns[conv->nturn - 1].is_tool = 1;
    }

    free(line); free(toks); free(role); free(content);
    fclose(fp);
    return conv;
}

/* -------------------------------------------------------------------------
 * history_search
 * ---------------------------------------------------------------------- */

/*
 * Search one file for `term`.  Writes matches to fd_out.
 * Returns the number of matches found.
 */
static int hist_search_file(const char *filepath, const char *term, int fd_out)
{
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;

    char *prev = malloc(HIST_LINE_MAX);
    char *cur  = malloc(HIST_LINE_MAX);
    char *next = malloc(HIST_LINE_MAX);
    char *out  = malloc(HIST_LINE_MAX + HIST_PATH_MAX + 64);

    if (!prev || !cur || !next || !out) {
        free(prev); free(cur); free(next); free(out);
        fclose(fp);
        return 0;
    }

    /* Extract just the filename for display. */
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;

    int    matches  = 0;
    int    linenum  = 0;
    int    have_cur = 0;
    prev[0] = '\0';

    while (1) {
        /* Shift: prev <- cur, cur <- next line from file. */
        if (have_cur)
            strncpy(prev, cur, HIST_LINE_MAX - 1);
        prev[HIST_LINE_MAX - 1] = '\0';

        if (!fgets(cur, HIST_LINE_MAX, fp)) break;
        linenum++;
        have_cur = 1;

        /* Strip trailing newline. */
        size_t clen = strlen(cur);
        while (clen > 0 && (cur[clen-1] == '\n' || cur[clen-1] == '\r'))
            cur[--clen] = '\0';

        if (!ci_strstr(cur, term)) continue;

        /* Match found. Print context. */
        matches++;

        /* Print previous line if non-empty. */
        if (prev[0]) {
            size_t n = (size_t)snprintf(out, HIST_LINE_MAX + HIST_PATH_MAX + 64,
                                        "%s:%d: %s\n", fname, linenum - 1, prev);
            hist_write(fd_out, out, n);
        }

        /* Print matching line. */
        {
            size_t n = (size_t)snprintf(out, HIST_LINE_MAX + HIST_PATH_MAX + 64,
                                        "%s:%d: %s\n", fname, linenum, cur);
            hist_write(fd_out, out, n);
        }

        /* Peek at next line for context. */
        char *nxt_buf = next;
        if (fgets(nxt_buf, HIST_LINE_MAX, fp)) {
            linenum++;
            size_t nlen = strlen(nxt_buf);
            while (nlen > 0 && (nxt_buf[nlen-1] == '\n' || nxt_buf[nlen-1] == '\r'))
                nxt_buf[--nlen] = '\0';
            size_t n = (size_t)snprintf(out, HIST_LINE_MAX + HIST_PATH_MAX + 64,
                                        "%s:%d: %s\n", fname, linenum, nxt_buf);
            hist_write(fd_out, out, n);
            /* Use next line as prev for the following iteration. */
            strncpy(prev, nxt_buf, HIST_LINE_MAX - 1);
            prev[HIST_LINE_MAX - 1] = '\0';
            have_cur = 0; /* prev is already set; skip the shift next iteration */
        }

        if (matches > 1 || prev[0]) {
            const char *sep = "---\n";
            hist_write(fd_out, sep, strlen(sep));
        }
    }

    free(prev); free(cur); free(next); free(out);
    fclose(fp);
    return matches;
}

int history_search(const char *term, int fd_out)
{
    if (!term || !term[0]) return 0;

    char dir[HIST_PATH_MAX];
    if (hist_conversations_dir(dir, sizeof(dir)) != 0) return 0;

    DIR *dp = opendir(dir);
    if (!dp) return 0;

    int total = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 6 || strcmp(de->d_name + nlen - 6, ".jsonl") != 0)
            continue;
        char full[HIST_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) continue;
        total += hist_search_file(full, term, fd_out);
    }
    closedir(dp);
    return total;
}

/* -------------------------------------------------------------------------
 * history_export
 * ---------------------------------------------------------------------- */

/*
 * Write a string to an open FILE*.
 */
static void hist_fwrite(FILE *fp, const char *s)
{
    if (s) fputs(s, fp);
}

int history_export(const Conversation *conv, const char *path, int fd_out)
{
    if (!conv) return -1;

    FILE *fp = NULL;
    int   use_fd = 0;

    if (path) {
        fp = fopen(path, "w");
        if (!fp) return -1;
    } else {
        fp = fdopen(dup(fd_out), "w");
        if (!fp) return -1;
        use_fd = 1;
    }

    (void)use_fd;

    for (int i = 0; i < conv->nturn; i++) {
        const Turn *t = &conv->turns[i];
        if (!t->role || !t->content) continue;

        /* Skip system and tool turns. */
        if (strcmp(t->role, "system") == 0) continue;
        if (t->is_tool) continue;

        /* Role header. */
        if (strcmp(t->role, "user") == 0)
            hist_fwrite(fp, "## User\n\n");
        else if (strcmp(t->role, "assistant") == 0)
            hist_fwrite(fp, "## Assistant\n\n");
        else {
            fprintf(fp, "## %s\n\n", t->role);
        }

        /* Content (preserved verbatim — code fences already in content). */
        hist_fwrite(fp, t->content);
        hist_fwrite(fp, "\n\n");
    }

    fclose(fp);
    return 0;
}
