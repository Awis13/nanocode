/*
 * memory.c — cross-session memory tool (memory_write)
 *
 * Stores named sections in ~/.nanocode/memory.md:
 *
 *   ## key
 *   content line 1
 *   content line 2
 *
 * On write:
 *   1. Load existing file (if any).
 *   2. Replace the matching ## key section or append a new one.
 *   3. Count total lines; if > MEMORY_MAX_LINES, drop oldest sections
 *      from the top until within the limit.
 *   4. Write atomically via temp-file + rename.
 */

#define _POSIX_C_SOURCE 200809L

#include "memory.h"
#include "executor.h"
#include "../util/arena.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Return the path to ~/.nanocode/memory.md.
 * Uses a static buffer; not thread-safe, but nanocode is single-threaded.
 */
static const char *memory_path(void)
{
    static char s_path[512];
    static int  s_init = 0;
    if (!s_init) {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            home = "/tmp";
        snprintf(s_path, sizeof(s_path), "%s/.nanocode/memory.md", home);
        s_init = 1;
    }
    return s_path;
}

/*
 * Ensure ~/.nanocode/ directory exists.
 * Returns 0 on success, -1 on failure.
 */
static int ensure_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/tmp";

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.nanocode", home);

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * Read the entire file at `path` into a heap buffer.
 * Sets *out_len to the byte count (not including NUL).
 * Returns NULL if the file does not exist or is empty.
 * Caller must free().
 */
static char *read_heap(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0)                     { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf)                        { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/*
 * Count '\n' characters in `s` (= number of newlines = roughly line count).
 */
static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

/* -------------------------------------------------------------------------
 * Core logic: update/append a section, then trim if over limit
 * ---------------------------------------------------------------------- */

/*
 * Build the updated memory file contents.
 * - `existing`: current file text (may be NULL/empty).
 * - `key`     : section key (the ## heading, without "## ").
 * - `content` : new body text for that section.
 *
 * Returns heap-allocated NUL-terminated string.  Caller must free().
 * Returns NULL on OOM.
 */
static char *build_updated(const char *existing, const char *key,
                            const char *content)
{
    /*
     * Strategy: walk `existing` looking for "## <key>\n".
     * If found, replace everything from that heading up to the next
     * "## " heading (or end-of-file) with the new section.
     * Otherwise, append the new section.
     */

    /* Build the heading we're looking for: "## <key>\n" */
    size_t klen  = strlen(key);
    size_t hlen  = 4 + klen;  /* "## " + key + "\n" = 4 + klen */
    char  *needle = malloc(hlen + 1);
    if (!needle) return NULL;
    memcpy(needle, "## ", 3);
    memcpy(needle + 3, key, klen);
    needle[3 + klen] = '\n';
    needle[hlen]     = '\0';

    /* New section text: "## <key>\n<content>\n\n" */
    size_t clen      = strlen(content);
    size_t newsec_len = 3 + klen + 1 + clen + 2; /* "## "+key+"\n"+content+"\n\n" */
    char  *newsec     = malloc(newsec_len + 1);
    if (!newsec) { free(needle); return NULL; }
    {
        char *p = newsec;
        memcpy(p, "## ", 3);          p += 3;
        memcpy(p, key, klen);          p += klen;
        *p++ = '\n';
        memcpy(p, content, clen);      p += clen;
        if (clen == 0 || content[clen - 1] != '\n')
            *p++ = '\n';
        *p++ = '\n';
        *p   = '\0';
        newsec_len = (size_t)(p - newsec);
    }

    char *result = NULL;

    if (!existing || !existing[0]) {
        /* Nothing existing — new section is the whole file. */
        result = malloc(newsec_len + 1);
        if (result) memcpy(result, newsec, newsec_len + 1);
        free(needle);
        free(newsec);
        return result;
    }

    /* Look for existing heading. */
    const char *found = strstr(existing, needle);
    if (!found) {
        /* Append new section. */
        size_t elen = strlen(existing);
        result = malloc(elen + newsec_len + 1);
        if (result) {
            memcpy(result, existing, elen);
            memcpy(result + elen, newsec, newsec_len + 1);
        }
        free(needle);
        free(newsec);
        return result;
    }

    /* Replace from `found` to the start of the next "## " section. */
    const char *after = found + hlen; /* skip past "## key\n" */
    /* Find next ## heading in `after`. */
    const char *next_sec = strstr(after, "\n## ");
    if (next_sec)
        next_sec += 1; /* include the leading '\n' separator in prefix */

    size_t prefix_len = (size_t)(found - existing);
    size_t suffix_len = next_sec ? strlen(next_sec) : 0;

    result = malloc(prefix_len + newsec_len + suffix_len + 1);
    if (result) {
        char *p = result;
        memcpy(p, existing,  prefix_len); p += prefix_len;
        memcpy(p, newsec,    newsec_len); p += newsec_len;
        if (next_sec) {
            memcpy(p, next_sec, suffix_len);
            p += suffix_len;
        }
        *p = '\0';
    }

    free(needle);
    free(newsec);
    return result;
}

/*
 * Trim the oldest sections from the top of `text` until the line count is
 * <= MEMORY_MAX_LINES.
 *
 * Sections start with "## ".  We drop whole sections from the front.
 * Returns a heap-allocated string.  Input `text` is NOT modified or freed.
 */
static char *trim_oldest(const char *text)
{
    if (count_lines(text) <= MEMORY_MAX_LINES)
        return strdup(text);

    const char *p = text;
    while (count_lines(p) > MEMORY_MAX_LINES) {
        /* Skip the first section.  It starts with "## ". */
        if (strncmp(p, "## ", 3) != 0)
            break; /* not a section boundary — stop trimming */

        /* Find the start of the next section or end of file. */
        const char *next = strstr(p + 3, "\n## ");
        if (!next) {
            /* Only one section left; return empty string rather than
             * deleting the last entry. */
            return strdup("");
        }
        p = next + 1; /* skip '\n', point at "## " of next section */
    }
    return strdup(p);
}

/* -------------------------------------------------------------------------
 * Write atomically via temp file + rename
 * ---------------------------------------------------------------------- */

static int write_atomic(const char *path, const char *text)
{
    /* Build temp path in same directory: <path>.tmp */
    size_t plen = strlen(path);
    char  *tmp  = malloc(plen + 5);
    if (!tmp) return -1;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    FILE *f = fopen(tmp, "w");
    if (!f) { free(tmp); return -1; }

    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    fclose(f);

    if (w != n) { free(tmp); return -1; }

    int rc = rename(tmp, path);
    free(tmp);
    return rc;
}

/* -------------------------------------------------------------------------
 * Tool handler
 * ---------------------------------------------------------------------- */

/*
 * Parse a JSON string field from args_json.
 * Returns heap-allocated NUL-terminated string on success, NULL on failure.
 * Simple scanner: handles only top-level string values (no nesting).
 */
static char *extract_string_field(const char *json, const char *field)
{
    if (!json || !field) return NULL;

    size_t flen = strlen(field);
    /* Build search pattern: "<field>" */
    char *pat = malloc(flen + 4);
    if (!pat) return NULL;
    pat[0] = '"';
    memcpy(pat + 1, field, flen);
    pat[flen + 1] = '"';
    pat[flen + 2] = '\0';

    const char *p = strstr(json, pat);
    free(pat);
    if (!p) return NULL;

    /* Advance past the key and optional whitespace/colon. */
    p += flen + 2;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == ' ')
        p++;

    if (*p != '"') return NULL;
    p++; /* skip opening quote */

    /* Scan to closing quote, handling backslash escapes. */
    size_t cap    = 1024;
    char  *val    = malloc(cap);
    if (!val) return NULL;
    size_t out    = 0;

    while (*p && *p != '"') {
        if (out + 4 >= cap) {
            cap *= 2;
            char *tmp = realloc(val, cap);
            if (!tmp) { free(val); return NULL; }
            val = tmp;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  val[out++] = '"';  break;
            case '\\': val[out++] = '\\'; break;
            case '/':  val[out++] = '/';  break;
            case 'n':  val[out++] = '\n'; break;
            case 'r':  val[out++] = '\r'; break;
            case 't':  val[out++] = '\t'; break;
            default:   val[out++] = *p;   break;
            }
        } else {
            val[out++] = *p;
        }
        p++;
    }
    val[out] = '\0';
    return val;
}

static ToolResult make_error(Arena *arena, const char *msg)
{
    size_t mlen = strlen(msg);
    /* {"error":"<msg>"} */
    size_t blen = mlen + 12;
    char  *buf  = arena_alloc(arena, blen);
    if (buf) snprintf(buf, blen, "{\"error\":\"%s\"}", msg);
    ToolResult r = { .error = 1, .content = buf, .len = buf ? strlen(buf) : 0 };
    return r;
}

static ToolResult memory_write_handler(Arena *arena, const char *args_json)
{
    char *key     = extract_string_field(args_json, "key");
    char *content = extract_string_field(args_json, "content");

    if (!key) {
        free(content);
        return make_error(arena, "memory_write: missing required argument: key");
    }
    if (!content) {
        free(key);
        return make_error(arena, "memory_write: missing required argument: content");
    }

    /* Load existing file. */
    size_t elen    = 0;
    char  *existing = read_heap(memory_path(), &elen);

    /* Build updated content. */
    char *updated = build_updated(existing, key, content);
    free(existing);
    free(key);
    free(content);

    if (!updated)
        return make_error(arena, "memory_write: out of memory");

    /* Trim if over line limit. */
    char *trimmed = trim_oldest(updated);
    free(updated);
    if (!trimmed)
        return make_error(arena, "memory_write: out of memory during trim");

    /* Ensure directory and write. */
    if (ensure_dir() != 0 || write_atomic(memory_path(), trimmed) != 0) {
        free(trimmed);
        return make_error(arena, "memory_write: failed to write memory file");
    }

    int lines = count_lines(trimmed);
    free(trimmed);

    /* Return success message. */
    static const char pfx[] = "{\"ok\":true,\"lines\":";
    size_t buflen = sizeof(pfx) + 12;
    char  *buf    = arena_alloc(arena, buflen);
    if (buf)
        snprintf(buf, buflen, "{\"ok\":true,\"lines\":%d}", lines);
    ToolResult r = { .error = 0, .content = buf, .len = buf ? strlen(buf) : 0 };
    return r;
}

static const char s_memory_write_schema[] =
    "{"
    "\"name\":\"memory_write\","
    "\"description\":"
        "\"Persist a named note to cross-session memory (~/.nanocode/memory.md). "
        "Use to remember facts, decisions, or context that should survive "
        "between conversations. Each key is a section heading; writing the same "
        "key again replaces the previous entry.\","
    "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"key\":{"
                "\"type\":\"string\","
                "\"description\":\"Section heading (short noun phrase, e.g. 'preferred style')\""
            "},"
            "\"content\":{"
                "\"type\":\"string\","
                "\"description\":\"Text to store under this key\""
            "}"
        "},"
        "\"required\":[\"key\",\"content\"]"
    "}"
    "}";

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void memory_tool_register(void)
{
    tool_register("memory_write", s_memory_write_schema, memory_write_handler, TOOL_SAFE_MUTATING);
}

char *memory_load(Arena *arena)
{
    size_t len;
    char  *heap = read_heap(memory_path(), &len);
    if (!heap || len == 0) {
        free(heap);
        return NULL;
    }
    char *copy = arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, heap, len + 1);
    free(heap);
    return copy;
}
