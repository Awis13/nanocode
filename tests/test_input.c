/*
 * test_input.c — unit tests for the terminal line editor
 *
 * Covers EditBuf operations, history management, and tab completion.
 * Terminal I/O (raw mode, key reading) is not tested here; it requires
 * an interactive TTY and is validated manually.
 */

#include "test.h"
#include "../src/tui/input.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================================
 * EditBuf — insert / move / delete
 * ====================================================================== */

TEST(test_edit_insert_basic)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert(&e, 'a');
    edit_insert(&e, 'b');
    edit_insert(&e, 'c');
    ASSERT_EQ((int)e.len, 3);
    ASSERT_EQ((int)e.pos, 3);
    ASSERT_STR_EQ(e.buf, "abc");
}

TEST(test_edit_insert_middle)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert(&e, 'a');
    edit_insert(&e, 'c');
    edit_move_left(&e);   /* pos = 1 */
    edit_insert(&e, 'b'); /* inserts between 'a' and 'c' */
    ASSERT_STR_EQ(e.buf, "abc");
    ASSERT_EQ((int)e.len, 3);
    ASSERT_EQ((int)e.pos, 2);
}

TEST(test_edit_insert_str)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "hello", 5);
    ASSERT_STR_EQ(e.buf, "hello");
    ASSERT_EQ((int)e.len, 5);
    ASSERT_EQ((int)e.pos, 5);
}

TEST(test_edit_backspace)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "abc", 3);
    edit_backspace(&e);
    ASSERT_STR_EQ(e.buf, "ab");
    ASSERT_EQ((int)e.len, 2);
    ASSERT_EQ((int)e.pos, 2);
}

TEST(test_edit_backspace_from_middle)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "abc", 3);
    edit_move_left(&e); /* pos = 2 */
    edit_backspace(&e); /* removes 'b', pos = 1 */
    ASSERT_STR_EQ(e.buf, "ac");
    ASSERT_EQ((int)e.pos, 1);
}

TEST(test_edit_backspace_at_start)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert(&e, 'x');
    edit_move_home(&e);
    edit_backspace(&e); /* should be a no-op */
    ASSERT_STR_EQ(e.buf, "x");
    ASSERT_EQ((int)e.len, 1);
    ASSERT_EQ((int)e.pos, 0);
}

TEST(test_edit_delete_forward)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "abc", 3);
    edit_move_home(&e);
    edit_delete(&e); /* removes 'a' */
    ASSERT_STR_EQ(e.buf, "bc");
    ASSERT_EQ((int)e.len, 2);
    ASSERT_EQ((int)e.pos, 0);
}

TEST(test_edit_delete_at_end)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "abc", 3);
    edit_delete(&e); /* cursor at end: no-op */
    ASSERT_STR_EQ(e.buf, "abc");
    ASSERT_EQ((int)e.len, 3);
}

TEST(test_edit_move_bounds)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_move_left(&e);  /* at 0: no-op */
    ASSERT_EQ((int)e.pos, 0);
    edit_move_right(&e); /* at end (0): no-op */
    ASSERT_EQ((int)e.pos, 0);

    edit_insert_str(&e, "hi", 2);
    edit_move_right(&e); /* already at end: no-op */
    ASSERT_EQ((int)e.pos, 2);
    edit_move_home(&e);
    ASSERT_EQ((int)e.pos, 0);
    edit_move_end(&e);
    ASSERT_EQ((int)e.pos, 2);
}

TEST(test_edit_reset)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "hello world", 11);
    edit_reset(&e);
    ASSERT_EQ((int)e.len, 0);
    ASSERT_EQ((int)e.pos, 0);
    ASSERT_EQ(e.buf[0], '\0');
}

TEST(test_edit_overflow_clamped)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    /* Fill to capacity minus 1 (NUL) */
    for (int i = 0; i < INPUT_LINE_MAX - 1; i++)
        edit_insert(&e, 'x');
    ASSERT_EQ((int)e.len, INPUT_LINE_MAX - 1);
    /* One more insert must be a no-op */
    edit_insert(&e, 'y');
    ASSERT_EQ((int)e.len, INPUT_LINE_MAX - 1);
    ASSERT_EQ(e.buf[INPUT_LINE_MAX - 1], '\0');
}

/* =========================================================================
 * History — add, peek, wrap, dedup
 * ====================================================================== */

TEST(test_history_add_basic)
{
    input_history_reset();
    input_history_add("alpha");
    input_history_add("beta");
    input_history_add("gamma");
    /* hist_peek(0) = newest */
    input_history_add(""); /* empty: should be a no-op, not crash */
    (void)0;
}

/* Helper: peek is a static internal function; we test it via save/load. */
static void check_history_order(const char **expected, int n)
{
    char tmpfile[] = "/tmp/test_input_hist_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return;
    close(fd);

    input_history_save(tmpfile);

    FILE *f = fopen(tmpfile, "r");
    if (!f) { unlink(tmpfile); return; }

    char line[INPUT_LINE_MAX];
    for (int i = 0; i < n; i++) {
        if (!fgets(line, sizeof(line), f)) {
            ASSERT_TRUE(0); /* fewer lines than expected */
            break;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            len--;
        line[len] = '\0';
        ASSERT_STR_EQ(line, expected[i]);
    }
    fclose(f);
    unlink(tmpfile);
}

TEST(test_history_order)
{
    input_history_reset();
    input_history_add("first");
    input_history_add("second");
    input_history_add("third");
    /* save writes oldest first */
    const char *exp[] = {"first", "second", "third"};
    check_history_order(exp, 3);
}

TEST(test_history_dedup)
{
    input_history_reset();
    input_history_add("cmd");
    input_history_add("cmd"); /* duplicate: should be dropped */
    const char *exp[] = {"cmd"};
    check_history_order(exp, 1);
}

TEST(test_history_dedup_non_consecutive)
{
    input_history_reset();
    input_history_add("cmd");
    input_history_add("other");
    input_history_add("cmd"); /* not consecutive: should be kept */
    const char *exp[] = {"cmd", "other", "cmd"};
    check_history_order(exp, 3);
}

TEST(test_history_skip_empty)
{
    input_history_reset();
    input_history_add("");
    input_history_add("   "); /* non-empty but whitespace: kept */
    /* only the whitespace line should be saved */
    char tmpfile[] = "/tmp/test_input_empty_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return;
    close(fd);
    input_history_save(tmpfile);
    FILE *f = fopen(tmpfile, "r");
    if (!f) { unlink(tmpfile); return; }
    char line[128];
    int count = 0;
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);
    unlink(tmpfile);
    ASSERT_EQ(count, 1);
}

TEST(test_history_save_load_roundtrip)
{
    input_history_reset();
    input_history_add("one");
    input_history_add("two");
    input_history_add("three");

    char tmpfile[] = "/tmp/test_input_rt_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return;
    close(fd);

    input_history_save(tmpfile);
    input_history_reset();

    /* After reset, history should be empty. */
    char tmp2[] = "/tmp/test_input_rt2_XXXXXX";
    int fd2 = mkstemp(tmp2);
    if (fd2 < 0) { unlink(tmpfile); return; }
    close(fd2);
    input_history_save(tmp2);
    FILE *f2 = fopen(tmp2, "r");
    if (f2) {
        char line[128];
        ASSERT_TRUE(!fgets(line, sizeof(line), f2)); /* empty */
        fclose(f2);
    }
    unlink(tmp2);

    /* Reload and check order. */
    input_history_load(tmpfile);
    const char *exp[] = {"one", "two", "three"};
    check_history_order(exp, 3);

    unlink(tmpfile);
}

TEST(test_history_load_nonexistent)
{
    input_history_reset();
    input_history_add("kept");
    /* Loading a nonexistent file is a no-op: history stays intact. */
    input_history_load("/nonexistent_path_zzzzzz_test");
    char tmpfile[] = "/tmp/test_input_ne_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return;
    close(fd);
    input_history_save(tmpfile);
    FILE *f = fopen(tmpfile, "r");
    int count = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) count++;
        fclose(f);
    }
    unlink(tmpfile);
    ASSERT_EQ(count, 1); /* "kept" entry still present */
}

/* =========================================================================
 * Tab completion
 * ====================================================================== */

TEST(test_tab_no_slash)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "hello", 5);
    size_t before = e.len;
    input_tab_complete(&e);
    ASSERT_EQ((int)e.len, (int)before); /* no change */
}

TEST(test_tab_empty)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    input_tab_complete(&e); /* must not crash */
    ASSERT_EQ((int)e.len, 0);
}

TEST(test_tab_unique_match)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "/he", 3);
    input_tab_complete(&e);
    ASSERT_STR_EQ(e.buf, "/help");
    ASSERT_EQ((int)e.pos, 5);
}

TEST(test_tab_common_prefix)
{
    /* "/c" matches /clear — only one command starts with /c */
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "/c", 2);
    input_tab_complete(&e);
    ASSERT_STR_EQ(e.buf, "/clear");
}

TEST(test_tab_slash_only)
{
    /* "/" matches everything: common prefix is "/" itself — no extension */
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert(&e, '/');
    size_t before = e.len;
    input_tab_complete(&e);
    /* At minimum, buffer should still start with '/' and len >= before */
    ASSERT_TRUE(e.buf[0] == '/');
    ASSERT_TRUE(e.len >= before);
}

TEST(test_tab_no_complete_after_space)
{
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "/help arg", 9);
    input_tab_complete(&e); /* no-op: space present */
    ASSERT_STR_EQ(e.buf, "/help arg");
}

TEST(test_tab_full_command)
{
    /* Completing an already-complete command: no change */
    EditBuf e;
    memset(&e, 0, sizeof(e));
    edit_insert_str(&e, "/help", 5);
    input_tab_complete(&e);
    ASSERT_STR_EQ(e.buf, "/help");
}

/* =========================================================================
 * Arena integration
 * ====================================================================== */

TEST(test_input_read_noninteractive)
{
    /*
     * Pipe a line into stdin and call input_read — exercises the
     * non-interactive (fgets) path without requiring a TTY.
     */
    int pipefd[2];
    if (pipe(pipefd) < 0) return;

    /* Replace stdin */
    int saved_stdin = dup(STDIN_FILENO);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);

    const char *msg = "hello world\n";
    write(pipefd[1], msg, strlen(msg));
    close(pipefd[1]);

    Arena *a = arena_new(1 << 16);
    InputLine il = input_read(a, "> ");

    /* Restore stdin */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);

    ASSERT_EQ(il.is_eof, 0);
    ASSERT_NOT_NULL(il.text);
    ASSERT_STR_EQ(il.text, "hello world");
    ASSERT_EQ((int)il.len, 11);

    arena_free(a);
}

TEST(test_input_read_eof)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return;

    int saved_stdin = dup(STDIN_FILENO);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
    close(pipefd[1]); /* immediately close write end → EOF */

    Arena *a = arena_new(1 << 16);
    InputLine il = input_read(a, "> ");

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);

    ASSERT_EQ(il.is_eof, 1);

    arena_free(a);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    fprintf(stderr, "=== test_input ===\n");

    RUN_TEST(test_edit_insert_basic);
    RUN_TEST(test_edit_insert_middle);
    RUN_TEST(test_edit_insert_str);
    RUN_TEST(test_edit_backspace);
    RUN_TEST(test_edit_backspace_from_middle);
    RUN_TEST(test_edit_backspace_at_start);
    RUN_TEST(test_edit_delete_forward);
    RUN_TEST(test_edit_delete_at_end);
    RUN_TEST(test_edit_move_bounds);
    RUN_TEST(test_edit_reset);
    RUN_TEST(test_edit_overflow_clamped);

    RUN_TEST(test_history_add_basic);
    RUN_TEST(test_history_order);
    RUN_TEST(test_history_dedup);
    RUN_TEST(test_history_dedup_non_consecutive);
    RUN_TEST(test_history_skip_empty);
    RUN_TEST(test_history_save_load_roundtrip);
    RUN_TEST(test_history_load_nonexistent);

    RUN_TEST(test_tab_no_slash);
    RUN_TEST(test_tab_empty);
    RUN_TEST(test_tab_unique_match);
    RUN_TEST(test_tab_common_prefix);
    RUN_TEST(test_tab_slash_only);
    RUN_TEST(test_tab_no_complete_after_space);
    RUN_TEST(test_tab_full_command);

    RUN_TEST(test_input_read_noninteractive);
    RUN_TEST(test_input_read_eof);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
