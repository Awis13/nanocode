/*
 * test_buf.c — unit tests for dynamic byte buffer
 */

#include "test.h"
#include "../src/util/buf.h"

#include <string.h>

TEST(test_buf_init) {
    Buf b;
    buf_init(&b);
    ASSERT_NULL(b.data);
    ASSERT_EQ(b.len, 0);
    ASSERT_EQ(b.cap, 0);
}

TEST(test_buf_append_single) {
    Buf b;
    buf_init(&b);

    int r = buf_append(&b, "hello", 5);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(b.len, 5);
    ASSERT_MEM_EQ(b.data, "hello", 5);

    buf_destroy(&b);
}

TEST(test_buf_append_str) {
    Buf b;
    buf_init(&b);

    int r = buf_append_str(&b, "world");
    ASSERT_EQ(r, 0);
    ASSERT_EQ(b.len, 5);

    buf_destroy(&b);
}

TEST(test_buf_append_multiple) {
    Buf b;
    buf_init(&b);

    buf_append_str(&b, "foo");
    buf_append_str(&b, "bar");
    buf_append_str(&b, "baz");

    ASSERT_EQ(b.len, 9);
    ASSERT_MEM_EQ(b.data, "foobarbaz", 9);

    buf_destroy(&b);
}

TEST(test_buf_str) {
    Buf b;
    buf_init(&b);

    buf_append_str(&b, "hello");
    const char *s = buf_str(&b);
    ASSERT_STR_EQ(s, "hello");
    ASSERT_EQ(b.len, 5); /* buf_str must not change len */

    buf_destroy(&b);
}

TEST(test_buf_str_empty) {
    Buf b;
    buf_init(&b);

    const char *s = buf_str(&b);
    /* Must return a valid (possibly empty) string, not NULL or garbage */
    ASSERT_NOT_NULL(s);

    buf_destroy(&b);
}

TEST(test_buf_reset) {
    Buf b;
    buf_init(&b);

    buf_append_str(&b, "some data");
    size_t cap_before = b.cap;

    buf_reset(&b);
    ASSERT_EQ(b.len, 0);
    ASSERT_EQ(b.cap, cap_before); /* capacity should be retained */

    buf_destroy(&b);
}

TEST(test_buf_reset_and_reuse) {
    Buf b;
    buf_init(&b);

    buf_append_str(&b, "first");
    buf_reset(&b);
    buf_append_str(&b, "second");

    ASSERT_EQ(b.len, 6);
    ASSERT_STR_EQ(buf_str(&b), "second");

    buf_destroy(&b);
}

TEST(test_buf_destroy_clears) {
    Buf b;
    buf_init(&b);

    buf_append_str(&b, "data");
    buf_destroy(&b);

    ASSERT_NULL(b.data);
    ASSERT_EQ(b.len, 0);
    ASSERT_EQ(b.cap, 0);
}

TEST(test_buf_grow_across_initial_cap) {
    Buf b;
    buf_init(&b);

    /* Write more than BUF_INITIAL_CAP (4096) bytes to force multiple reallocs */
    char chunk[256];
    memset(chunk, 'x', sizeof(chunk));

    for (int i = 0; i < 20; i++) {
        int r = buf_append(&b, chunk, sizeof(chunk));
        ASSERT_EQ(r, 0);
    }

    ASSERT_EQ(b.len, 20 * 256);
    ASSERT_TRUE(b.cap >= b.len);

    buf_destroy(&b);
}

TEST(test_buf_append_zero_len) {
    Buf b;
    buf_init(&b);

    int r = buf_append(&b, "ignored", 0);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(b.len, 0);

    buf_destroy(&b);
}

TEST(test_buf_binary_data) {
    Buf b;
    buf_init(&b);

    /* Buffers must handle embedded NULs */
    const char data[] = {0x00, 0x01, 0x02, 0xFF};
    buf_append(&b, data, sizeof(data));

    ASSERT_EQ(b.len, 4);
    ASSERT_MEM_EQ(b.data, data, 4);

    buf_destroy(&b);
}

int main(void)
{
    fprintf(stderr, "=== test_buf ===\n");
    RUN_TEST(test_buf_init);
    RUN_TEST(test_buf_append_single);
    RUN_TEST(test_buf_append_str);
    RUN_TEST(test_buf_append_multiple);
    RUN_TEST(test_buf_str);
    RUN_TEST(test_buf_str_empty);
    RUN_TEST(test_buf_reset);
    RUN_TEST(test_buf_reset_and_reuse);
    RUN_TEST(test_buf_destroy_clears);
    RUN_TEST(test_buf_grow_across_initial_cap);
    RUN_TEST(test_buf_append_zero_len);
    RUN_TEST(test_buf_binary_data);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
