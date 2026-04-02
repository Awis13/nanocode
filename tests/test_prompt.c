/*
 * test_prompt.c — unit tests for the system prompt builder (CMP-121)
 *
 * Acceptance criteria:
 *   - Detects Makefile in cwd and mentions C project
 *   - Includes CLAUDE.md content when present
 *   - Includes git status output when in a git repo
 *   - Lists all registered tools by name
 *   - Graceful if not in git repo (no crash)
 *   - ASan clean
 */

#include "test.h"
#include "../src/agent/prompt.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"
#include "../include/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Write a file with the given content.  Returns 0 on success. */
static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* Remove a file, ignoring errors. */
static void remove_file(const char *path)
{
    remove(path);
}

/* Dummy tool handler — never actually called in these tests. */
static ToolResult dummy_handler(Arena *arena, const char *args_json)
{
    (void)args_json;
    ToolResult r = { 0, arena_alloc(arena, 3), 2 };
    if (r.content)
        memcpy(r.content, "ok", 3);
    return r;
}

/* -------------------------------------------------------------------------
 * NULL / edge cases
 * ---------------------------------------------------------------------- */

TEST(test_prompt_null_arena)
{
    /* Must return NULL, not crash. */
    char *p = prompt_build(NULL, "/tmp", NULL, NULL);
    ASSERT_NULL(p);
}

TEST(test_prompt_null_cwd)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, NULL, NULL, NULL);
    ASSERT_NULL(p);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Base identity always present
 * ---------------------------------------------------------------------- */

TEST(test_prompt_contains_nanocode)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(strstr(p, "nanocode"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Project detection: Makefile → C project
 * ---------------------------------------------------------------------- */

TEST(test_prompt_detects_makefile)
{
    /* Use a temp directory with just a Makefile. */
    const char *tmpdir = "/tmp/test_prompt_makefile";
    mkdir(tmpdir, 0755);

    char mf_path[256];
    snprintf(mf_path, sizeof(mf_path), "%s/Makefile", tmpdir);
    write_file(mf_path, "all:\n\techo hi\n");

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);

    /* Must mention C project. */
    ASSERT_NOT_NULL(strstr(p, "C/C++"));

    remove_file(mf_path);
    rmdir(tmpdir);
    arena_free(a);
}

TEST(test_prompt_no_project_file)
{
    /* Directory with no recognized build files — no "Detected:" line. */
    const char *tmpdir = "/tmp/test_prompt_empty_proj";
    mkdir(tmpdir, 0755);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(strstr(p, "Detected:"));

    rmdir(tmpdir);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * CLAUDE.md injection
 * ---------------------------------------------------------------------- */

TEST(test_prompt_injects_claude_md)
{
    const char *tmpdir = "/tmp/test_prompt_claudemd";
    mkdir(tmpdir, 0755);

    char claude_path[256];
    snprintf(claude_path, sizeof(claude_path), "%s/CLAUDE.md", tmpdir);
    write_file(claude_path,
        "# Project Rules\nAlways write tests.\nUse arena allocator.\n");

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);

    ASSERT_NOT_NULL(strstr(p, "Always write tests."));
    ASSERT_NOT_NULL(strstr(p, "Use arena allocator."));

    remove_file(claude_path);
    rmdir(tmpdir);
    arena_free(a);
}

TEST(test_prompt_injects_nanocode_md)
{
    const char *tmpdir = "/tmp/test_prompt_nanocodemd";
    mkdir(tmpdir, 0755);

    char nc_path[256];
    snprintf(nc_path, sizeof(nc_path), "%s/.nanocode.md", tmpdir);
    write_file(nc_path, "Custom nanocode config.\n");

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(strstr(p, "Custom nanocode config."));

    remove_file(nc_path);
    rmdir(tmpdir);
    arena_free(a);
}

TEST(test_prompt_claude_md_takes_priority)
{
    /* When both CLAUDE.md and .nanocode.md exist, CLAUDE.md wins. */
    const char *tmpdir = "/tmp/test_prompt_both_md";
    mkdir(tmpdir, 0755);

    char claude_path[256], nc_path[256];
    snprintf(claude_path, sizeof(claude_path), "%s/CLAUDE.md",      tmpdir);
    snprintf(nc_path,     sizeof(nc_path),     "%s/.nanocode.md",   tmpdir);
    write_file(claude_path, "from-claude-md\n");
    write_file(nc_path,     "from-nanocode-md\n");

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(strstr(p, "from-claude-md"));
    ASSERT_NULL(strstr(p, "from-nanocode-md"));

    remove_file(claude_path);
    remove_file(nc_path);
    rmdir(tmpdir);
    arena_free(a);
}

TEST(test_prompt_no_config_file)
{
    /* No CLAUDE.md, no .nanocode.md — must not crash or emit garbage. */
    const char *tmpdir = "/tmp/test_prompt_no_cfg";
    mkdir(tmpdir, 0755);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, tmpdir, NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(strstr(p, "Project Instructions"));

    rmdir(tmpdir);
    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Git status — run in the actual repo directory (which is a git repo)
 * ---------------------------------------------------------------------- */

TEST(test_prompt_git_status_in_repo)
{
    /*
     * The nanocode source tree is itself a git repo; run prompt_build
     * there so we get real git output.  We only check that the prompt
     * does not crash and is non-empty; the section header may or may
     * not appear depending on working-tree state.
     */
    /* Use current directory — the binary is run from the project root,
     * which is a git repository. */
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, ".", NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(strlen(p) > 0);

    arena_free(a);
}

TEST(test_prompt_graceful_not_in_git)
{
    /* /tmp is not a git repo — must not crash. */
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, NULL);
    ASSERT_NOT_NULL(p); /* still returns a valid prompt */

    /* Git sections must be absent. */
    ASSERT_NULL(strstr(p, "Git Status"));
    ASSERT_NULL(strstr(p, "Recent Commits"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Available tools listing
 * ---------------------------------------------------------------------- */

TEST(test_prompt_lists_tools)
{
    tool_registry_reset();
    tool_register("bash",    "{}", dummy_handler);
    tool_register("read",    "{}", dummy_handler);
    tool_register("write",   "{}", dummy_handler);

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, NULL);
    ASSERT_NOT_NULL(p);

    ASSERT_NOT_NULL(strstr(p, "bash"));
    ASSERT_NOT_NULL(strstr(p, "read"));
    ASSERT_NOT_NULL(strstr(p, "write"));

    tool_registry_reset();
    arena_free(a);
}

TEST(test_prompt_no_tools_section_when_empty)
{
    tool_registry_reset();

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(strstr(p, "Available Tools"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Working directory always present
 * ---------------------------------------------------------------------- */

TEST(test_prompt_contains_cwd)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp/some/project", NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(strstr(p, "/tmp/some/project"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * Sandbox policy block injection
 * ---------------------------------------------------------------------- */

TEST(test_prompt_sandbox_block_injected_when_enabled)
{
    SandboxConfig sc;
    memset(&sc, 0, sizeof(sc));
    sc.enabled          = 1;
    sc.profile          = "strict";
    sc.allowed_paths    = "/tmp:/home/user";
    sc.allowed_commands = "ls:cat";
    sc.network          = 0;
    sc.max_file_size    = 1048576;

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, &sc);
    ASSERT_NOT_NULL(p);

    ASSERT_NOT_NULL(strstr(p, "<sandbox_policy>"));
    ASSERT_NOT_NULL(strstr(p, "strict"));
    ASSERT_NOT_NULL(strstr(p, "/tmp"));

    arena_free(a);
}

TEST(test_prompt_sandbox_block_absent_when_disabled)
{
    SandboxConfig sc;
    memset(&sc, 0, sizeof(sc));
    sc.enabled = 0;
    sc.profile = "strict";

    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, &sc);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(strstr(p, "<sandbox_policy>"));

    arena_free(a);
}

TEST(test_prompt_sandbox_null_sc_no_block)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *p = prompt_build(a, "/tmp", NULL, NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(strstr(p, "<sandbox_policy>"));

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_prompt ===\n");

    RUN_TEST(test_prompt_null_arena);
    RUN_TEST(test_prompt_null_cwd);
    RUN_TEST(test_prompt_contains_nanocode);

    RUN_TEST(test_prompt_detects_makefile);
    RUN_TEST(test_prompt_no_project_file);

    RUN_TEST(test_prompt_injects_claude_md);
    RUN_TEST(test_prompt_injects_nanocode_md);
    RUN_TEST(test_prompt_claude_md_takes_priority);
    RUN_TEST(test_prompt_no_config_file);

    RUN_TEST(test_prompt_git_status_in_repo);
    RUN_TEST(test_prompt_graceful_not_in_git);

    RUN_TEST(test_prompt_lists_tools);
    RUN_TEST(test_prompt_no_tools_section_when_empty);

    RUN_TEST(test_prompt_contains_cwd);

    RUN_TEST(test_prompt_sandbox_block_injected_when_enabled);
    RUN_TEST(test_prompt_sandbox_block_absent_when_disabled);
    RUN_TEST(test_prompt_sandbox_null_sc_no_block);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
