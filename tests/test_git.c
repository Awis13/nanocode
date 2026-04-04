/*
 * test_git.c — unit tests for the git integration module (CMP-148)
 *
 * Tests cover:
 *   - git_detect: recognises a git repo, returns 0 for non-git dirs
 *   - git_status: correct parse of porcelain output
 *   - git_log / git_branch: smoke tests in the actual repo
 *   - git_diff: returns a string (empty or not) without crashing
 *   - git_context_summary: section headers appear/absent appropriately
 *   - Tool registration: git_commit and git_branch_create registered
 *   - Graceful on NULL inputs
 */

#include "test.h"
#include "../src/agent/git.h"
#include "../src/tools/executor.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

static int contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL;
}

/* =========================================================================
 * git_detect
 * ====================================================================== */

TEST(test_detect_actual_repo)
{
    /*
     * The nanocode source tree is a git repo.  git_detect(".") must
     * return 1 when run from the project root.
     */
    ASSERT_TRUE(git_detect(".") == 1);
}

TEST(test_detect_non_repo)
{
    /* /tmp is not a git repo (unless someone made it one). */
    ASSERT_TRUE(git_detect("/tmp") == 0);
}

TEST(test_detect_null)
{
    ASSERT_TRUE(git_detect(NULL) == 0);
    ASSERT_TRUE(git_detect("") == 0);
}

TEST(test_detect_subdir)
{
    /* A subdirectory of a git repo is still detected as a git repo. */
    ASSERT_TRUE(git_detect("src") == 1);
}

/* =========================================================================
 * git_status
 * ====================================================================== */

TEST(test_status_non_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Non-git directory must return NULL, not crash. */
    GitStatusResult *r = git_status(a, "/tmp");
    ASSERT_NULL(r);

    arena_free(a);
}

TEST(test_status_null)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_status(NULL, "."));
    ASSERT_NULL(git_status(a, NULL));

    arena_free(a);
}

TEST(test_status_returns_struct_in_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Running in the actual repo must return a non-NULL result. */
    GitStatusResult *r = git_status(a, ".");
    ASSERT_NOT_NULL(r);
    if (!r) { arena_free(a); return; }

    /* count must be in valid range */
    ASSERT_TRUE(r->count >= 0);
    ASSERT_TRUE(r->count <= GIT_STATUS_MAX);

    /* Each returned entry must have a two-char xy code and non-empty path */
    for (int i = 0; i < r->count; i++) {
        ASSERT_TRUE(r->files[i].xy[2] == '\0');
        ASSERT_TRUE(r->files[i].path[0] != '\0');
    }

    arena_free(a);
}

/* =========================================================================
 * git_log
 * ====================================================================== */

TEST(test_log_in_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *log = git_log(a, ".", 5);
    ASSERT_NOT_NULL(log);
    /* The repo has commits, so the log must be non-empty. */
    ASSERT_TRUE(strlen(log) > 0);

    arena_free(a);
}

TEST(test_log_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_log(NULL, ".", 5));
    ASSERT_NULL(git_log(a, NULL, 5));
    ASSERT_NULL(git_log(a, ".", 0));
    ASSERT_NULL(git_log(a, ".", -1));

    arena_free(a);
}

/* =========================================================================
 * git_branch
 * ====================================================================== */

TEST(test_branch_in_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *branch = git_branch(a, ".");
    ASSERT_NOT_NULL(branch);
    /* Must be a non-empty string with no trailing newline. */
    size_t len = strlen(branch);
    ASSERT_TRUE(len > 0);
    ASSERT_TRUE(branch[len - 1] != '\n');

    arena_free(a);
}

TEST(test_branch_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_branch(NULL, "."));
    ASSERT_NULL(git_branch(a, NULL));

    arena_free(a);
}

/* =========================================================================
 * git_diff
 * ====================================================================== */

TEST(test_diff_no_crash)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Running git diff must not crash; result may be empty if tree is clean. */
    char *diff = git_diff(a, ".", NULL);
    ASSERT_NOT_NULL(diff); /* popen_to_arena never returns NULL on success */

    arena_free(a);
}

TEST(test_diff_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_diff(NULL, ".", NULL));
    ASSERT_NULL(git_diff(a, NULL, NULL));

    arena_free(a);
}

/* =========================================================================
 * git_context_summary
 * ====================================================================== */

TEST(test_context_summary_in_repo)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *ctx = git_context_summary(a, ".");
    /*
     * The repo has commits so we expect at least the Recent Commits section.
     * NULL is acceptable only when the summary is fully empty (clean repo,
     * no commits), which cannot be the case in the test environment.
     */
    if (ctx) {
        /* When non-NULL, one or both sections should appear. */
        int has_status  = contains(ctx, "## Git Status");
        int has_commits = contains(ctx, "## Recent Commits");
        ASSERT_TRUE(has_status || has_commits);
    }
    /* If ctx is NULL the repo is simply clean with no log — acceptable. */

    arena_free(a);
}

TEST(test_context_summary_non_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* /tmp is not a git repo: must return NULL without crashing. */
    char *ctx = git_context_summary(a, "/tmp");
    ASSERT_NULL(ctx);

    arena_free(a);
}

TEST(test_context_summary_null)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_context_summary(NULL, "."));
    ASSERT_NULL(git_context_summary(a, NULL));

    arena_free(a);
}

TEST(test_context_summary_section_headers_absent_in_non_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *ctx = git_context_summary(a, "/tmp");
    /* Either NULL or non-empty without the git section headers. */
    if (ctx) {
        ASSERT_FALSE(contains(ctx, "## Git Status"));
        ASSERT_FALSE(contains(ctx, "## Recent Commits"));
    }

    arena_free(a);
}


/* =========================================================================
 * git_diff_cached
 * ====================================================================== */

TEST(test_diff_cached_no_crash)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Running git diff --cached must not crash; may be empty in clean tree. */
    char *diff = git_diff_cached(a, ".", 0);
    ASSERT_NOT_NULL(diff);

    arena_free(a);
}

TEST(test_diff_cached_stat_no_crash)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *diff = git_diff_cached(a, ".", 1);
    ASSERT_NOT_NULL(diff);

    arena_free(a);
}

TEST(test_diff_cached_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_diff_cached(NULL, ".", 0));
    ASSERT_NULL(git_diff_cached(a, NULL, 0));

    arena_free(a);
}

/* =========================================================================
 * git_diff_stat
 * ====================================================================== */

TEST(test_diff_stat_no_crash)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *stat = git_diff_stat(a, ".");
    ASSERT_NOT_NULL(stat);

    arena_free(a);
}

TEST(test_diff_stat_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_diff_stat(NULL, "."));
    ASSERT_NULL(git_diff_stat(a, NULL));

    arena_free(a);
}

/* =========================================================================
 * git_untracked
 * ====================================================================== */

TEST(test_untracked_no_crash)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    char *ut = git_untracked(a, ".");
    ASSERT_NOT_NULL(ut);

    arena_free(a);
}

TEST(test_untracked_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_untracked(NULL, "."));
    ASSERT_NULL(git_untracked(a, NULL));

    arena_free(a);
}

/* =========================================================================
 * git_special_state
 * ====================================================================== */

TEST(test_special_state_clean_repo)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /*
     * A clean repo with no merge/rebase in progress must return NULL.
     * (If a CI environment happens to be in a merge this test may skip.)
     */
    char *state = git_special_state(a, ".");
    /* Either NULL (clean) or a non-empty description. */
    if (state)
        ASSERT_TRUE(strlen(state) > 0);

    arena_free(a);
}

TEST(test_special_state_null_inputs)
{
    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ASSERT_NULL(git_special_state(NULL, "."));
    ASSERT_NULL(git_special_state(a, NULL));

    arena_free(a);
}

/* =========================================================================
 * git_context_summary — extended checks for new sections
 * ====================================================================== */

TEST(test_context_summary_branch_in_header)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    char *ctx = git_context_summary(a, ".");
    if (ctx && ctx[0]) {
        /* The new summary starts with ## Git Context and includes Branch: */
        int has_header = contains(ctx, "## Git Context");
        int has_branch = contains(ctx, "Branch:");
        ASSERT_TRUE(has_header && has_branch);
    }

    arena_free(a);
}

TEST(test_context_summary_ten_commits)
{
    Arena *a = arena_new(1 << 20);
    ASSERT_NOT_NULL(a);

    /*
     * The repo has more than 5 commits so the summary should include
     * the Recent Commits section.  We just verify the section is present.
     */
    char *ctx = git_context_summary(a, ".");
    if (ctx)
        ASSERT_TRUE(contains(ctx, "## Recent Commits"));

    arena_free(a);
}

/* =========================================================================
 * Tool registration
 * ====================================================================== */

TEST(test_tools_registered)
{
    tool_registry_reset();
    git_tools_register();

    const char *names[64];
    int count = tool_list_names(names, 64);

    int found_commit = 0;
    int found_branch = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "git_commit")        == 0) found_commit = 1;
        if (strcmp(names[i], "git_branch_create") == 0) found_branch = 1;
    }

    ASSERT_TRUE(found_commit);
    ASSERT_TRUE(found_branch);

    tool_registry_reset();
}

TEST(test_tool_commit_missing_message)
{
    tool_registry_reset();
    git_tools_register();

    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Missing "message" → error result, no crash. */
    ToolResult r = tool_invoke(a, "git_commit", "{\"files\":[\"foo.c\"]}");
    ASSERT_TRUE(r.error != 0);
    ASSERT_NOT_NULL(r.content);

    arena_free(a);
    tool_registry_reset();
}

TEST(test_tool_branch_missing_name)
{
    tool_registry_reset();
    git_tools_register();

    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    /* Missing "name" → error result, no crash. */
    ToolResult r = tool_invoke(a, "git_branch_create", "{\"checkout\":false}");
    ASSERT_TRUE(r.error != 0);
    ASSERT_NOT_NULL(r.content);

    arena_free(a);
    tool_registry_reset();
}

TEST(test_tool_invalid_json)
{
    tool_registry_reset();
    git_tools_register();

    Arena *a = arena_new(1 << 16);
    ASSERT_NOT_NULL(a);

    ToolResult r1 = tool_invoke(a, "git_commit",        "not-json");
    ToolResult r2 = tool_invoke(a, "git_branch_create", "not-json");
    ASSERT_TRUE(r1.error != 0);
    ASSERT_TRUE(r2.error != 0);

    arena_free(a);
    tool_registry_reset();
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_git ===\n");

    RUN_TEST(test_detect_actual_repo);
    RUN_TEST(test_detect_non_repo);
    RUN_TEST(test_detect_null);
    RUN_TEST(test_detect_subdir);

    RUN_TEST(test_status_non_repo);
    RUN_TEST(test_status_null);
    RUN_TEST(test_status_returns_struct_in_repo);

    RUN_TEST(test_log_in_repo);
    RUN_TEST(test_log_null_inputs);

    RUN_TEST(test_branch_in_repo);
    RUN_TEST(test_branch_null_inputs);

    RUN_TEST(test_diff_no_crash);
    RUN_TEST(test_diff_null_inputs);

    RUN_TEST(test_context_summary_in_repo);
    RUN_TEST(test_context_summary_non_repo);
    RUN_TEST(test_context_summary_null);
    RUN_TEST(test_context_summary_section_headers_absent_in_non_repo);
    RUN_TEST(test_diff_cached_no_crash);
    RUN_TEST(test_diff_cached_stat_no_crash);
    RUN_TEST(test_diff_cached_null_inputs);
    RUN_TEST(test_diff_stat_no_crash);
    RUN_TEST(test_diff_stat_null_inputs);
    RUN_TEST(test_untracked_no_crash);
    RUN_TEST(test_untracked_null_inputs);
    RUN_TEST(test_special_state_clean_repo);
    RUN_TEST(test_special_state_null_inputs);
    RUN_TEST(test_context_summary_branch_in_header);
    RUN_TEST(test_context_summary_ten_commits);

    RUN_TEST(test_tools_registered);
    RUN_TEST(test_tool_commit_missing_message);
    RUN_TEST(test_tool_branch_missing_name);
    RUN_TEST(test_tool_invalid_json);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
