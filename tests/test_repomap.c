/*
 * test_repomap.c — unit tests for the repo map module
 *
 * Tests cover:
 *  - C symbol extraction (functions, structs, enums) from in-memory files
 *  - Python, Go, Rust basic extraction
 *  - Directory scan with skip logic
 *  - Render output format and 4 KB cap
 *  - Edge cases: empty file, no symbols, render on empty map
 */

#include "test.h"
#include "../src/agent/repomap.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

/*
 * Write `content` to `path` (creates file).
 * Returns 1 on success, 0 on failure.
 */
static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(content, f);
    fclose(f);
    return 1;
}

/* Make a directory, ignoring EEXIST. */
static void mkdirp(const char *path)
{
    mkdir(path, 0755);
}

/* Return 1 if `haystack` contains `needle`. */
static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/* =========================================================================
 * C extraction tests — single-file scans via a temp directory
 * ====================================================================== */

TEST(test_c_functions)
{
    char tmpdir[] = "/tmp/test_repomap_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/foo.h", dir);
    write_file(path,
        "/* header */\n"
        "void foo_init(Arena *a);\n"
        "int  foo_process(const char *s, size_t n);\n"
        "void foo_free(Arena *a);\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    ASSERT_NOT_NULL(rm);

    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "foo_init"));
    ASSERT_TRUE(contains(out, "foo_process"));
    ASSERT_TRUE(contains(out, "foo_free"));
    ASSERT_TRUE(contains(out, "(fn)"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

TEST(test_c_structs_and_enums)
{
    char tmpdir[] = "/tmp/test_repomap_s_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/types.h", dir);
    write_file(path,
        "typedef struct { int x; int y; } Point;\n"
        "typedef enum { RED, GREEN, BLUE } Color;\n"
        "struct Node {\n"
        "    int val;\n"
        "};\n"
        "enum Direction { NORTH, SOUTH, EAST, WEST };\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "Point"));
    ASSERT_TRUE(contains(out, "Color"));
    ASSERT_TRUE(contains(out, "(struct)") || contains(out, "(enum)"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

TEST(test_c_skips_keywords)
{
    char tmpdir[] = "/tmp/test_repomap_kw_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/control.c", dir);
    write_file(path,
        "int main(void) {\n"
        "    if (x > 0) { return 1; }\n"
        "    for (int i = 0; i < 10; i++) {}\n"
        "    while (1) { break; }\n"
        "    return 0;\n"
        "}\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    /* 'if', 'for', 'while' must NOT appear as symbols */
    ASSERT_FALSE(contains(out, "if (fn)"));
    ASSERT_FALSE(contains(out, "for (fn)"));
    ASSERT_FALSE(contains(out, "while (fn)"));
    /* 'main' is a valid function */
    ASSERT_TRUE(contains(out, "main"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

TEST(test_c_skips_block_comments)
{
    char tmpdir[] = "/tmp/test_repomap_bc_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/commented.h", dir);
    write_file(path,
        "/*\n"
        " * void should_not_appear(void);\n"
        " */\n"
        "void real_fn(void);\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_FALSE(contains(out, "should_not_appear"));
    ASSERT_TRUE(contains(out, "real_fn"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

TEST(test_c_empty_file)
{
    char tmpdir[] = "/tmp/test_repomap_ef_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/empty.h", dir);
    write_file(path, "");

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((int)strlen(out), 0);

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

/* =========================================================================
 * Python extraction
 * ====================================================================== */

TEST(test_python_extraction)
{
    char tmpdir[] = "/tmp/test_repomap_py_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/utils.py", dir);
    write_file(path,
        "class Tokenizer:\n"
        "    def __init__(self):\n"
        "        pass\n"
        "\n"
        "def tokenize(text):\n"
        "    return []\n"
        "\n"
        "def count_tokens(text):\n"
        "    return 0\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "Tokenizer"));
    ASSERT_TRUE(contains(out, "tokenize"));
    ASSERT_TRUE(contains(out, "count_tokens"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

/* =========================================================================
 * Go extraction
 * ====================================================================== */

TEST(test_go_extraction)
{
    char tmpdir[] = "/tmp/test_repomap_go_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/server.go", dir);
    write_file(path,
        "package main\n"
        "\n"
        "type Server struct { addr string }\n"
        "\n"
        "func NewServer(addr string) *Server {\n"
        "    return &Server{addr: addr}\n"
        "}\n"
        "\n"
        "func (s *Server) Start() error {\n"
        "    return nil\n"
        "}\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "Server"));
    ASSERT_TRUE(contains(out, "NewServer"));
    ASSERT_TRUE(contains(out, "Start"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

/* =========================================================================
 * Rust extraction
 * ====================================================================== */

TEST(test_rust_extraction)
{
    char tmpdir[] = "/tmp/test_repomap_rs_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/lib.rs", dir);
    write_file(path,
        "pub struct Config { pub key: String }\n"
        "pub enum Mode { Fast, Slow }\n"
        "pub trait Runnable {\n"
        "    fn run(&self);\n"
        "}\n"
        "pub fn new_config() -> Config {\n"
        "    Config { key: String::new() }\n"
        "}\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "Config"));
    ASSERT_TRUE(contains(out, "Mode"));
    ASSERT_TRUE(contains(out, "Runnable"));
    ASSERT_TRUE(contains(out, "new_config"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

/* =========================================================================
 * Directory traversal and skip logic
 * ====================================================================== */

TEST(test_skip_vendor_and_git)
{
    char tmpdir[] = "/tmp/test_repomap_sk_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    /* Files in vendor/ and .git/ should not be scanned. */
    char vdir[256], gdir[256];
    snprintf(vdir, sizeof(vdir), "%s/vendor", dir);
    snprintf(gdir, sizeof(gdir), "%s/.git", dir);
    mkdirp(vdir);
    mkdirp(gdir);

    char vpath[256], gpath[256], spath[256];
    snprintf(vpath, sizeof(vpath), "%s/vendor/lib.h", dir);
    snprintf(gpath, sizeof(gpath), "%s/.git/hook.c", dir);
    snprintf(spath, sizeof(spath), "%s/src.h", dir);
    write_file(vpath, "void vendor_fn(void);\n");
    write_file(gpath, "void git_fn(void);\n");
    write_file(spath, "void real_fn(void);\n");

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_FALSE(contains(out, "vendor_fn"));
    ASSERT_FALSE(contains(out, "git_fn"));
    ASSERT_TRUE(contains(out, "real_fn"));

    repomap_free(rm);
    arena_free(a);
    unlink(vpath); unlink(gpath); unlink(spath);
    rmdir(vdir); rmdir(gdir); rmdir(dir);
}

TEST(test_recursive_scan)
{
    char tmpdir[] = "/tmp/test_repomap_rec_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char subdir[256];
    snprintf(subdir, sizeof(subdir), "%s/sub", dir);
    mkdirp(subdir);

    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/root.h", dir);
    snprintf(path2, sizeof(path2), "%s/sub/deep.h", dir);
    write_file(path1, "void root_fn(void);\n");
    write_file(path2, "void deep_fn(void);\n");

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    ASSERT_TRUE(contains(out, "root_fn"));
    ASSERT_TRUE(contains(out, "deep_fn"));

    repomap_free(rm);
    arena_free(a);
    unlink(path1); unlink(path2); rmdir(subdir); rmdir(dir);
}

/* =========================================================================
 * Render format and size cap
 * ====================================================================== */

TEST(test_render_format)
{
    char tmpdir[] = "/tmp/test_repomap_fmt_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (!dir) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/api.h", dir);
    write_file(path,
        "typedef struct Connection Connection;\n"
        "Connection *conn_new(void);\n"
        "void conn_close(Connection *c);\n"
    );

    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    repomap_scan(rm, dir);
    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    /* File path must appear as a header followed by ':'. */
    ASSERT_TRUE(contains(out, "api.h:") || contains(out, "api.h:\n"));
    /* Symbols must be indented with two spaces. */
    ASSERT_TRUE(contains(out, "  conn_new"));
    ASSERT_TRUE(contains(out, "  conn_close"));

    repomap_free(rm);
    arena_free(a);
    unlink(path);
    rmdir(dir);
}

TEST(test_render_empty_map)
{
    Arena *a = arena_new(1 << 16);
    RepoMap *rm = repomap_new(a);
    ASSERT_NOT_NULL(rm);

    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((int)strlen(out), 0);

    repomap_free(rm);
    arena_free(a);
}

TEST(test_render_null_safety)
{
    Arena *a = arena_new(1 << 16);
    repomap_free(NULL); /* must not crash */
    repomap_scan(NULL, "/tmp"); /* must not crash */
    (void)repomap_render(NULL, a); /* may return NULL — that is fine */
    arena_free(a);
}

/* =========================================================================
 * Scan nanocode's own src/ — integration smoke test
 * ====================================================================== */

TEST(test_scan_nanocode_src)
{
    /*
     * Scan the actual nanocode src/ tree.
     * We only check that:
     *   - The scan succeeds (no crash)
     *   - The output is non-empty (symbols were found)
     *   - The output is under 4 KB
     *   - Known symbols are present
     */
    Arena *a = arena_new(1 << 20);
    RepoMap *rm = repomap_new(a);
    ASSERT_NOT_NULL(rm);

    repomap_scan(rm, "src");

    char *out = repomap_render(rm, a);
    ASSERT_NOT_NULL(out);

    size_t len = strlen(out);
    ASSERT_TRUE(len > 0);
    ASSERT_TRUE(len <= 4096);

    /* Known functions from the codebase. */
    ASSERT_TRUE(contains(out, "arena_new") || contains(out, "arena_alloc"));
    ASSERT_TRUE(contains(out, "renderer_new") || contains(out, "renderer_token"));

    repomap_free(rm);
    arena_free(a);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_repomap ===\n");

    RUN_TEST(test_c_functions);
    RUN_TEST(test_c_structs_and_enums);
    RUN_TEST(test_c_skips_keywords);
    RUN_TEST(test_c_skips_block_comments);
    RUN_TEST(test_c_empty_file);

    RUN_TEST(test_python_extraction);
    RUN_TEST(test_go_extraction);
    RUN_TEST(test_rust_extraction);

    RUN_TEST(test_skip_vendor_and_git);
    RUN_TEST(test_recursive_scan);

    RUN_TEST(test_render_format);
    RUN_TEST(test_render_empty_map);
    RUN_TEST(test_render_null_safety);

    RUN_TEST(test_scan_nanocode_src);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
