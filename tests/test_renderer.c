/*
 * test_renderer.c — unit tests for the ANSI streaming markdown renderer
 */

#include "test.h"
#include "tui/renderer.h"
#include "util/arena.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Test helpers
 * ====================================================================== */

/*
 * Render `text` via the renderer, capture all output into `out`.
 * `width` sets the terminal width (-1 → default 80).
 * Returns number of bytes written.
 */
static int render_str(const char *text, char *out, int cap, int width)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;

    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    if (width > 0) renderer_set_width(r, width);

    renderer_token(r, text, strlen(text));
    renderer_flush(r);
    renderer_free(r);

    close(fds[1]);

    int n = (int)read(fds[0], out, (size_t)(cap - 1));
    close(fds[0]);
    if (n < 0) n = 0;
    out[n] = '\0';

    arena_free(a);
    return n;
}

/*
 * Render `text` as 1-byte-at-a-time tokens (worst-case streaming split).
 */
static int render_streaming(const char *text, char *out, int cap, int width)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;

    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    if (width > 0) renderer_set_width(r, width);

    for (size_t i = 0; text[i]; i++)
        renderer_token(r, text + i, 1);

    renderer_flush(r);
    renderer_free(r);
    close(fds[1]);

    int n = (int)read(fds[0], out, (size_t)(cap - 1));
    close(fds[0]);
    if (n < 0) n = 0;
    out[n] = '\0';

    arena_free(a);
    return n;
}

/* =========================================================================
 * Basic output
 * ====================================================================== */

TEST(test_plain_text) {
    char out[512];
    int n = render_str("hello world\n", out, sizeof(out), -1);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(out, "hello world") != NULL);
}

TEST(test_plain_text_streaming) {
    char out1[512], out2[512];
    render_str("hello world\n", out1, sizeof(out1), -1);
    render_streaming("hello world\n", out2, sizeof(out2), -1);
    /* Both paths must produce identical visible text */
    ASSERT_TRUE(strstr(out1, "hello world") != NULL);
    ASSERT_TRUE(strstr(out2, "hello world") != NULL);
}

/* =========================================================================
 * Bold
 * ====================================================================== */

TEST(test_bold) {
    char out[512];
    render_str("**bold**\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[1m") != NULL);  /* SGR_BOLD emitted */
    ASSERT_TRUE(strstr(out, "bold") != NULL);
    ASSERT_TRUE(strstr(out, "\033[0m") != NULL);  /* SGR_RESET emitted */
}

TEST(test_bold_streaming) {
    char out[512];
    render_streaming("**bold**\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[1m") != NULL);
    ASSERT_TRUE(strstr(out, "bold") != NULL);
}

TEST(test_bold_with_spaces) {
    char out[512];
    render_str("before **bold words here** after\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "before") != NULL);
    ASSERT_TRUE(strstr(out, "bold words here") != NULL);
    ASSERT_TRUE(strstr(out, "after") != NULL);
    ASSERT_TRUE(strstr(out, "\033[1m") != NULL);
}

/* =========================================================================
 * Italic
 * ====================================================================== */

TEST(test_italic) {
    char out[512];
    render_str("*italic*\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[3m") != NULL);  /* SGR_ITALIC emitted */
    ASSERT_TRUE(strstr(out, "italic") != NULL);
}

TEST(test_italic_streaming) {
    char out[512];
    render_streaming("*italic*\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[3m") != NULL);
    ASSERT_TRUE(strstr(out, "italic") != NULL);
}

/* =========================================================================
 * Inline code
 * ====================================================================== */

TEST(test_inline_code) {
    char out[512];
    render_str("`mycode`\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[7m") != NULL);  /* SGR_REV (reverse video) */
    ASSERT_TRUE(strstr(out, "mycode") != NULL);
}

TEST(test_inline_code_streaming) {
    char out[512];
    render_streaming("`mycode`\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[7m") != NULL);
    ASSERT_TRUE(strstr(out, "mycode") != NULL);
}

/* =========================================================================
 * Headers
 * ====================================================================== */

TEST(test_h1) {
    char out[512];
    render_str("# Title\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[1m") != NULL);  /* bold */
    ASSERT_TRUE(strstr(out, "Title") != NULL);
}

TEST(test_h2) {
    char out[512];
    render_str("## Subtitle\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\033[1m") != NULL);
    ASSERT_TRUE(strstr(out, "Subtitle") != NULL);
}

TEST(test_header_not_plain_hash) {
    char out[512];
    /* A # not followed by space must be rendered literally */
    render_str("#notaheader\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "#notaheader") != NULL);
}

/* =========================================================================
 * Lists
 * ====================================================================== */

TEST(test_unordered_list_dash) {
    char out[512];
    render_str("- item one\n", out, sizeof(out), -1);
    /* Bullet rendered as UTF-8 • (U+2022 = E2 80 A2) */
    ASSERT_TRUE(strstr(out, "\xe2\x80\xa2") != NULL);
    ASSERT_TRUE(strstr(out, "item one") != NULL);
}

TEST(test_unordered_list_star) {
    char out[512];
    render_str("* item two\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "\xe2\x80\xa2") != NULL);
    ASSERT_TRUE(strstr(out, "item two") != NULL);
}

TEST(test_ordered_list) {
    char out[512];
    render_str("1. first\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "1.") != NULL);
    ASSERT_TRUE(strstr(out, "first") != NULL);
}

/* =========================================================================
 * Fenced code blocks
 * ====================================================================== */

TEST(test_fence_plain) {
    char out[1024];
    render_str("```\nsome code\n```\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "some code") != NULL);
    /* Reset after fence */
    ASSERT_TRUE(strstr(out, "\033[0m") != NULL);
}

TEST(test_fence_c_keywords) {
    char out[1024];
    render_str("```c\nif (x) return 0;\n```\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "if") != NULL);
    ASSERT_TRUE(strstr(out, "return") != NULL);
    /* Some ANSI colour must be emitted for keywords */
    ASSERT_TRUE(strstr(out, "\033[") != NULL);
}

TEST(test_fence_python_keywords) {
    char out[1024];
    render_str("```python\ndef foo():\n    return True\n```\n",
               out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "def") != NULL);
    ASSERT_TRUE(strstr(out, "return") != NULL);
    ASSERT_TRUE(strstr(out, "True") != NULL);
}

TEST(test_fence_json) {
    char out[1024];
    render_str("```json\n{\"key\": true}\n```\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "key") != NULL);
    ASSERT_TRUE(strstr(out, "true") != NULL);
}

TEST(test_fence_streaming) {
    char out[1024];
    render_streaming("```c\nint x = 0;\n```\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "int") != NULL);
    ASSERT_TRUE(strstr(out, "x") != NULL);
}

/* =========================================================================
 * Word wrap
 * ====================================================================== */

TEST(test_word_wrap) {
    char out[2048];
    /* 10-char terminal width; "hello world" must wrap */
    render_str("hello world\n", out, sizeof(out), 10);
    /* Should contain a newline between the words (plus trailing) */
    int newlines = 0;
    for (int i = 0; out[i]; i++)
        if (out[i] == '\n') newlines++;
    ASSERT_TRUE(newlines >= 2); /* at least: mid-wrap + trailing flush newline */
}

TEST(test_word_wrap_no_break_long_word) {
    /* A word longer than term_width must NOT be wrapped (would loop forever) */
    char out[2048];
    render_str("averylongwordthatexceedstermwidth\n", out, sizeof(out), 10);
    ASSERT_TRUE(strstr(out, "averylongwordthatexceedstermwidth") != NULL);
}

/* =========================================================================
 * Streaming: 100 tokens without buffering entire response
 * ====================================================================== */

TEST(test_streaming_100_tokens) {
    char out[2048];
    /*
     * Feed 100 single-character 'a' tokens one byte at a time —
     * worst-case streaming split. Output must contain all 100 chars
     * and correct reset sequence.
     */
    char input[101];
    memset(input, 'a', 100);
    input[100] = '\0';

    int n = render_streaming(input, out, sizeof(out), 80);
    /* At minimum: 100 'a' chars + SGR_RESET (4) + newline (1) */
    ASSERT_TRUE(n >= 105);
    /* All 'a' chars must be present (they may be split across lines by wrap) */
    int count = 0;
    for (int i = 0; out[i]; i++)
        if (out[i] == 'a') count++;
    ASSERT_EQ(count, 100);
}

/* =========================================================================
 * Edge cases
 * ====================================================================== */

TEST(test_empty_input) {
    char out[64];
    int n = render_str("", out, sizeof(out), -1);
    /* Empty input: renderer_flush emits SGR_RESET; no crash */
    (void)n;
    ASSERT_TRUE(1); /* just no crash / ASan error */
}

TEST(test_multiple_flush) {
    char out[512];
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    renderer_token(r, "hello\n", 6);
    renderer_flush(r);
    renderer_token(r, "world\n", 6);
    renderer_flush(r);
    renderer_free(r);
    close(fds[1]);
    int n = (int)read(fds[0], out, sizeof(out) - 1);
    close(fds[0]);
    arena_free(a);
    if (n < 0) n = 0;
    out[n] = '\0';
    ASSERT_TRUE(strstr(out, "hello") != NULL);
    ASSERT_TRUE(strstr(out, "world") != NULL);
}

TEST(test_crlf_stripped) {
    char out[512];
    render_str("line one\r\nline two\r\n", out, sizeof(out), -1);
    ASSERT_TRUE(strstr(out, "line one") != NULL);
    ASSERT_TRUE(strstr(out, "line two") != NULL);
}

/* =========================================================================
 * Write-batching frame API
 * ====================================================================== */

/*
 * Verify that output fed inside a frame does NOT reach the fd until
 * renderer_frame_end() is called.
 */
TEST(test_frame_batches_output) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    renderer_set_width(r, 80);

    renderer_frame_begin(r);
    renderer_token(r, "hello frame\n", 12);

    /* Set pipe non-blocking — read must return EAGAIN (nothing written yet) */
    int fl = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    char peek[64];
    int peeked = (int)read(fds[0], peek, sizeof(peek));
    fcntl(fds[0], F_SETFL, fl); /* restore blocking */
    ASSERT_TRUE(peeked <= 0);

    /* Commit the frame — now data must be available */
    renderer_frame_end(r);

    char out[512];
    int n = (int)read(fds[0], out, sizeof(out) - 1);
    close(fds[0]);
    close(fds[1]);
    arena_free(a);

    ASSERT_TRUE(n > 0);
    out[n] = '\0';
    ASSERT_TRUE(strstr(out, "hello frame") != NULL);
}

/*
 * Framed output must be byte-identical to non-framed output.
 * Also verifies that double renderer_frame_begin() is a no-op.
 */
TEST(test_frame_output_identical) {
    char out_plain[512], out_framed[512];
    int n_plain = render_str("**framed bold**\n", out_plain,
                             sizeof(out_plain), 80);
    ASSERT_TRUE(n_plain > 0);

    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    renderer_set_width(r, 80);

    renderer_frame_begin(r);
    renderer_frame_begin(r); /* double-begin is a no-op */
    renderer_token(r, "**framed bold**\n", 16);
    renderer_flush(r);
    renderer_frame_end(r);

    close(fds[1]);
    int n_framed = (int)read(fds[0], out_framed, sizeof(out_framed) - 1);
    close(fds[0]);
    arena_free(a);

    ASSERT_TRUE(n_framed > 0);
    out_framed[n_framed] = '\0';
    ASSERT_TRUE(strstr(out_framed, "framed bold") != NULL);
    ASSERT_EQ(n_plain, n_framed);
}

/*
 * renderer_frame_end() with no active frame must not crash or corrupt state.
 */
TEST(test_frame_end_no_op_without_begin) {
    int fds[2];
    ASSERT_TRUE(pipe(fds) == 0);
    Arena *a = arena_new(256 * 1024);
    Renderer *r = renderer_new(fds[1], a);
    renderer_set_width(r, 80);

    renderer_frame_end(r); /* no active frame — must be a no-op */
    renderer_token(r, "safe\n", 5);
    renderer_flush(r);
    renderer_free(r);
    close(fds[1]);

    char out[512];
    int n = (int)read(fds[0], out, sizeof(out) - 1);
    close(fds[0]);
    arena_free(a);

    ASSERT_TRUE(n > 0);
    out[n] = '\0';
    ASSERT_TRUE(strstr(out, "safe") != NULL);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    RUN_TEST(test_plain_text);
    RUN_TEST(test_plain_text_streaming);
    RUN_TEST(test_bold);
    RUN_TEST(test_bold_streaming);
    RUN_TEST(test_bold_with_spaces);
    RUN_TEST(test_italic);
    RUN_TEST(test_italic_streaming);
    RUN_TEST(test_inline_code);
    RUN_TEST(test_inline_code_streaming);
    RUN_TEST(test_h1);
    RUN_TEST(test_h2);
    RUN_TEST(test_header_not_plain_hash);
    RUN_TEST(test_unordered_list_dash);
    RUN_TEST(test_unordered_list_star);
    RUN_TEST(test_ordered_list);
    RUN_TEST(test_fence_plain);
    RUN_TEST(test_fence_c_keywords);
    RUN_TEST(test_fence_python_keywords);
    RUN_TEST(test_fence_json);
    RUN_TEST(test_fence_streaming);
    RUN_TEST(test_word_wrap);
    RUN_TEST(test_word_wrap_no_break_long_word);
    RUN_TEST(test_streaming_100_tokens);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_multiple_flush);
    RUN_TEST(test_crlf_stripped);
    RUN_TEST(test_frame_batches_output);
    RUN_TEST(test_frame_output_identical);
    RUN_TEST(test_frame_end_no_op_without_begin);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
