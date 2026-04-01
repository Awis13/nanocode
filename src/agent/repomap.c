/*
 * repomap.c — lightweight repo map: symbol extraction and context injection
 *
 * Pattern-based symbol extraction for C, Python, JS/TS, Go, and Rust.
 * No external dependencies — satisfies all CMP-147 acceptance criteria.
 *
 * The approach:
 *  - For each source file, read it line-by-line.
 *  - Maintain a minimal state machine (normal / block-comment).
 *  - Match top-level identifiers against per-language patterns.
 *  - Store (relative_path, name, kind) triples in a flat heap array.
 *  - repomap_render groups them by file and emits the compact text block.
 */

#define _POSIX_C_SOURCE 200809L

#include "agent/repomap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* =========================================================================
 * Internal constants
 * ====================================================================== */

#define LINE_MAX_LEN  4096
#define PATH_MAX_LEN  1024
#define NAME_MAX_LEN  256
#define SYMS_INIT     256
#define RENDER_MAX    (4096 - 1) /* cap render output at 4 KB */

/* =========================================================================
 * Data types
 * ====================================================================== */

typedef struct {
    char *file; /* malloc'd relative path from root */
    char *name; /* malloc'd symbol name */
    char  kind; /* 'f'=fn, 's'=struct, 'e'=enum, 't'=typedef/type/trait */
} Sym;

struct RepoMap {
    Arena *arena; /* borrowed — for repomap_render output */
    Sym   *syms;
    int    nsyms;
    int    cap;
};

/* =========================================================================
 * Helpers
 * ====================================================================== */

static int sym_push(RepoMap *rm, const char *file, const char *name, char kind)
{
    if (rm->nsyms >= rm->cap) {
        int newcap = rm->cap ? rm->cap * 2 : SYMS_INIT;
        Sym *s = realloc(rm->syms, (size_t)newcap * sizeof(Sym));
        if (!s) return 0;
        rm->syms = s;
        rm->cap  = newcap;
    }
    rm->syms[rm->nsyms].file = strdup(file);
    rm->syms[rm->nsyms].name = strdup(name);
    rm->syms[rm->nsyms].kind = kind;
    if (!rm->syms[rm->nsyms].file || !rm->syms[rm->nsyms].name) {
        free(rm->syms[rm->nsyms].file);
        free(rm->syms[rm->nsyms].name);
        return 0;
    }
    rm->nsyms++;
    return 1;
}

/* Trim leading whitespace from a string. */
static const char *ltrim(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* True if `s` starts with `prefix`. */
static int startswith(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* =========================================================================
 * C / C++ keyword list (identifiers that look like functions but aren't)
 * ====================================================================== */

static const char *const c_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "return",
    "sizeof", "typeof", "alignof", "offsetof",
    "typedef", "struct", "enum", "union",
    "static", "extern", "inline", "const", "volatile", "restrict",
    "void", "int", "long", "short", "char", "float", "double",
    "unsigned", "signed", "bool",
    "NULL", "true", "false",
    NULL
};

static int is_c_keyword(const char *s, size_t n)
{
    for (int i = 0; c_keywords[i]; i++) {
        if (strlen(c_keywords[i]) == n &&
            strncmp(c_keywords[i], s, n) == 0)
            return 1;
    }
    return 0;
}

/* Extract the identifier immediately before the first '(' on a line.
 * Returns the identifier length, or 0 if none found / is a keyword. */
static size_t c_fn_name(const char *line, const char **name_start)
{
    const char *p = strchr(line, '(');
    if (!p || p == line) return 0;

    /* Back past whitespace to the end of the identifier. */
    const char *end = p;
    while (end > line && isspace((unsigned char)end[-1])) end--;
    if (end == line) return 0;

    /* Back across the identifier characters. */
    const char *start = end;
    while (start > line &&
           (isalnum((unsigned char)start[-1]) || start[-1] == '_'))
        start--;

    size_t n = (size_t)(end - start);
    if (n == 0 || n >= NAME_MAX_LEN) return 0;
    if (is_c_keyword(start, n)) return 0;

    /* Reject identifiers that start with a digit (can't be a name). */
    if (isdigit((unsigned char)*start)) return 0;

    *name_start = start;
    return n;
}

/* =========================================================================
 * Per-language extractors
 * ====================================================================== */

static void extract_c(RepoMap *rm, const char *relpath, FILE *f)
{
    char line[LINE_MAX_LEN];
    int  in_block_comment = 0;

    while (fgets(line, (int)sizeof(line), f)) {
        /* Update block-comment state. */
        if (in_block_comment) {
            if (strstr(line, "*/")) in_block_comment = 0;
            continue;
        }
        if (strstr(line, "/*") && !strstr(line, "*/"))
            in_block_comment = 1;

        /* Skip preprocessor, indented lines, pure comment lines. */
        const char *t = ltrim(line);
        if (t[0] == '#' || t[0] == '*' || t[0] == '\0') continue;
        if (startswith(t, "//")) continue;
        /* Skip indented lines (non-top-level). */
        if (isspace((unsigned char)line[0])) continue;

        /* ---- struct / typedef struct ---- */
        if (startswith(t, "typedef struct") || startswith(t, "typedef enum") ||
            startswith(t, "typedef union")) {
            /* e.g.  typedef struct Foo Foo; or typedef struct { ... } Foo; */
            /* Look for the last identifier on the line before ';' */
            char *semi = strrchr(line, ';');
            if (semi) {
                /* work backward from ';' */
                const char *end2 = semi;
                while (end2 > t && isspace((unsigned char)end2[-1])) end2--;
                const char *st2 = end2;
                while (st2 > t &&
                       (isalnum((unsigned char)st2[-1]) || st2[-1] == '_'))
                    st2--;
                size_t n2 = (size_t)(end2 - st2);
                if (n2 > 0 && n2 < NAME_MAX_LEN) {
                    char nm[NAME_MAX_LEN];
                    memcpy(nm, st2, n2); nm[n2] = '\0';
                    char kind = startswith(t, "typedef enum") ? 'e' : 's';
                    sym_push(rm, relpath, nm, kind);
                }
            }
            continue;
        }

        if (startswith(t, "struct ") || startswith(t, "enum ") ||
            startswith(t, "union ")) {
            /* struct Foo { or enum Bar { */
            const char *p2 = t + (startswith(t, "struct ") ? 7 :
                                  startswith(t, "enum ")   ? 5 : 6);
            while (isspace((unsigned char)*p2)) p2++;
            const char *en2 = p2;
            while (isalnum((unsigned char)*en2) || *en2 == '_') en2++;
            size_t n2 = (size_t)(en2 - p2);
            if (n2 > 0 && n2 < NAME_MAX_LEN && strchr(line, '{')) {
                char nm[NAME_MAX_LEN];
                memcpy(nm, p2, n2); nm[n2] = '\0';
                char kind = startswith(t, "enum ") ? 'e' : 's';
                sym_push(rm, relpath, nm, kind);
            }
            continue;
        }

        /* ---- function declaration / definition ---- */
        const char *nstart = NULL;
        size_t nlen = c_fn_name(t, &nstart);
        if (nlen > 0) {
            char nm[NAME_MAX_LEN];
            memcpy(nm, nstart, nlen); nm[nlen] = '\0';
            sym_push(rm, relpath, nm, 'f');
        }
    }
}

static void extract_python(RepoMap *rm, const char *relpath, FILE *f)
{
    char line[LINE_MAX_LEN];
    while (fgets(line, (int)sizeof(line), f)) {
        const char *t = ltrim(line);
        if (startswith(t, "def ")) {
            const char *p = t + 4;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 'f');
            }
        } else if (startswith(t, "class ")) {
            const char *p = t + 6;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 's');
            }
        }
    }
}

static void extract_js(RepoMap *rm, const char *relpath, FILE *f)
{
    char line[LINE_MAX_LEN];
    while (fgets(line, (int)sizeof(line), f)) {
        const char *t = ltrim(line);
        /* function foo( */
        if (startswith(t, "function ") || startswith(t, "async function ")) {
            const char *p = startswith(t, "async ") ? t + 15 : t + 9;
            while (isspace((unsigned char)*p)) p++;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 'f');
            }
        }
        /* class Foo */
        if (startswith(t, "class ") || startswith(t, "export class ")) {
            const char *p = startswith(t, "export ") ? t + 13 : t + 6;
            while (isspace((unsigned char)*p)) p++;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 's');
            }
        }
        /* export function / export default function */
        if (startswith(t, "export function ") ||
            startswith(t, "export default function ")) {
            const char *p = startswith(t, "export default ") ? t + 24 : t + 16;
            while (isspace((unsigned char)*p)) p++;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 'f');
            }
        }
    }
}

static void extract_go(RepoMap *rm, const char *relpath, FILE *f)
{
    char line[LINE_MAX_LEN];
    while (fgets(line, (int)sizeof(line), f)) {
        const char *t = ltrim(line);
        if (startswith(t, "func ")) {
            const char *p = t + 5;
            /* Skip receiver: func (r *Foo) Name( */
            if (*p == '(') {
                while (*p && *p != ')') p++;
                if (*p == ')') p++;
                while (isspace((unsigned char)*p)) p++;
            }
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 'f');
            }
        } else if (startswith(t, "type ")) {
            const char *p = t + 5;
            const char *end = p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - p);
            const char *rest = ltrim(end);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, p, n); nm[n] = '\0';
                char kind = startswith(rest, "struct") ? 's' :
                            startswith(rest, "interface") ? 't' : 't';
                sym_push(rm, relpath, nm, kind);
            }
        }
    }
}

static void extract_rust(RepoMap *rm, const char *relpath, FILE *f)
{
    char line[LINE_MAX_LEN];
    while (fgets(line, (int)sizeof(line), f)) {
        const char *t = ltrim(line);
        /* pub fn / fn / pub async fn */
        const char *fn_p = NULL;
        if (startswith(t, "pub async fn ")) fn_p = t + 13;
        else if (startswith(t, "pub fn "))    fn_p = t + 7;
        else if (startswith(t, "fn "))        fn_p = t + 3;
        if (fn_p) {
            const char *end = fn_p;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - fn_p);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, fn_p, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, 'f');
            }
            continue;
        }
        /* struct / enum / trait (pub or not) */
        const char *kp = t;
        if (startswith(kp, "pub ")) kp += 4;
        char kind = 0;
        const char *np = NULL;
        if (startswith(kp, "struct ")) { kind = 's'; np = kp + 7; }
        else if (startswith(kp, "enum "))   { kind = 'e'; np = kp + 5; }
        else if (startswith(kp, "trait "))  { kind = 't'; np = kp + 6; }
        if (kind && np) {
            const char *end = np;
            while (isalnum((unsigned char)*end) || *end == '_') end++;
            size_t n = (size_t)(end - np);
            if (n && n < NAME_MAX_LEN) {
                char nm[NAME_MAX_LEN]; memcpy(nm, np, n); nm[n] = '\0';
                sym_push(rm, relpath, nm, kind);
            }
        }
    }
}

/* =========================================================================
 * Directory traversal
 * ====================================================================== */

static const char *const skip_dirs[] = {
    "vendor", ".git", "node_modules", "build", ".build", "target",
    "dist", "__pycache__", NULL
};

static int should_skip_dir(const char *name)
{
    for (int i = 0; skip_dirs[i]; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) return 1;
    }
    return name[0] == '.';
}

static void scan_entry(RepoMap *rm, const char *root,
                       const char *relpath, const char *name);

static void scan_dir_rec(RepoMap *rm, const char *root, const char *relpath)
{
    char fullpath[PATH_MAX_LEN];
    if (relpath[0])
        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, relpath);
    else
        snprintf(fullpath, sizeof(fullpath), "%s", root);

    DIR *d = opendir(fullpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        scan_entry(rm, root, relpath, ent->d_name);
    }
    closedir(d);
}

static void scan_entry(RepoMap *rm, const char *root,
                       const char *relpath, const char *name)
{
    char fullpath[PATH_MAX_LEN];
    char newrel[PATH_MAX_LEN];

    if (relpath[0])
        snprintf(newrel, sizeof(newrel), "%s/%s", relpath, name);
    else
        snprintf(newrel, sizeof(newrel), "%s", name);

    snprintf(fullpath, sizeof(fullpath), "%s/%s", root, newrel);

    struct stat st;
    if (stat(fullpath, &st) < 0) return;

    if (S_ISDIR(st.st_mode)) {
        if (!should_skip_dir(name))
            scan_dir_rec(rm, root, newrel);
        return;
    }

    if (!S_ISREG(st.st_mode)) return;

    /* Determine language by extension. */
    const char *ext = strrchr(name, '.');
    if (!ext) return;

    /*
     * For C source files, skip if a header with the same base name exists in
     * the same directory. Headers define the public API and avoid duplicating
     * symbols that already appear in the corresponding .h.
     */
    if (!strcmp(ext, ".c") || !strcmp(ext, ".cc") || !strcmp(ext, ".cpp")) {
        char hpath[PATH_MAX_LEN];
        const char *fp_ext = strrchr(fullpath, '.');
        size_t base_len = (size_t)(fp_ext - fullpath);
        if (fp_ext && base_len + 3 < sizeof(hpath)) {
            memcpy(hpath, fullpath, base_len);
            strcpy(hpath + base_len, ".h");
            struct stat hst;
            if (stat(hpath, &hst) == 0 && S_ISREG(hst.st_mode))
                return; /* prefer .h */
        }
    }

    FILE *f = fopen(fullpath, "r");
    if (!f) return;

    if (!strcmp(ext, ".c") || !strcmp(ext, ".h") ||
        !strcmp(ext, ".cc") || !strcmp(ext, ".cpp") || !strcmp(ext, ".hh"))
        extract_c(rm, newrel, f);
    else if (!strcmp(ext, ".py"))
        extract_python(rm, newrel, f);
    else if (!strcmp(ext, ".js") || !strcmp(ext, ".ts") ||
             !strcmp(ext, ".jsx") || !strcmp(ext, ".tsx"))
        extract_js(rm, newrel, f);
    else if (!strcmp(ext, ".go"))
        extract_go(rm, newrel, f);
    else if (!strcmp(ext, ".rs"))
        extract_rust(rm, newrel, f);

    fclose(f);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

RepoMap *repomap_new(Arena *arena)
{
    RepoMap *rm = calloc(1, sizeof(RepoMap));
    if (!rm) return NULL;
    rm->arena = arena;
    return rm;
}

void repomap_scan(RepoMap *rm, const char *root_dir)
{
    if (!rm || !root_dir) return;
    scan_dir_rec(rm, root_dir, "");
}

char *repomap_render(RepoMap *rm, Arena *arena)
{
    if (!rm || !arena) return NULL;

    /* We render into a growing heap buffer, then copy to arena. */
    char   *out  = NULL;
    size_t  olen = 0;
    size_t  ocap = 0;

#define OPUT(s, n) do { \
    size_t _n = (size_t)(n); \
    if (olen + _n + 1 > ocap) { \
        size_t _nc = ocap ? ocap * 2 : 4096; \
        while (_nc < olen + _n + 1) _nc *= 2; \
        char *_p = realloc(out, _nc); \
        if (!_p) goto render_oom; \
        out = _p; ocap = _nc; \
    } \
    memcpy(out + olen, (s), _n); \
    olen += _n; \
    out[olen] = '\0'; \
} while (0)

    /* Walk files in appearance order, grouping by file. */
    const char *cur_file = NULL;
    for (int i = 0; i < rm->nsyms && olen < RENDER_MAX; i++) {
        if (!cur_file || strcmp(cur_file, rm->syms[i].file) != 0) {
            cur_file = rm->syms[i].file;
            if (i > 0) OPUT("\n", 1);
            OPUT(cur_file, strlen(cur_file));
            OPUT(":\n", 2);
        }
        OPUT("  ", 2);
        OPUT(rm->syms[i].name, strlen(rm->syms[i].name));
        const char *ks = rm->syms[i].kind == 'f' ? " (fn)" :
                         rm->syms[i].kind == 's' ? " (struct)" :
                         rm->syms[i].kind == 'e' ? " (enum)" : " (type)";
        OPUT(ks, strlen(ks));
        OPUT("\n", 1);
    }

    if (!out) {
        char *empty = arena_alloc(arena, 1);
        if (empty) *empty = '\0';
        return empty;
    }

    /* Copy to arena. */
    char *result = arena_alloc(arena, olen + 1);
    if (result) {
        memcpy(result, out, olen + 1);
    }
    free(out);
    return result;

render_oom:
    free(out);
    {
        char *empty = arena_alloc(arena, 1);
        if (empty) *empty = '\0';
        return empty;
    }
#undef OPUT
}

void repomap_free(RepoMap *rm)
{
    if (!rm) return;
    for (int i = 0; i < rm->nsyms; i++) {
        free(rm->syms[i].file);
        free(rm->syms[i].name);
    }
    free(rm->syms);
    free(rm);
}
