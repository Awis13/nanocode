/*
 * test_arena.c — unit tests for arena allocator
 */

#include "test.h"
#include "../src/util/arena.h"

#include <stdint.h>

TEST(test_arena_new_and_free) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(a->base);
    ASSERT_EQ(a->used, 0);
    ASSERT_TRUE(a->size >= 4096);
    arena_free(a);
}

TEST(test_arena_alloc_basic) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    void *p = arena_alloc(a, 64);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(a->used >= 64);

    arena_free(a);
}

TEST(test_arena_alloc_alignment) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    /* Allocate odd sizes and verify each returned pointer is 16-byte aligned */
    for (size_t sz = 1; sz <= 33; sz++) {
        void *p = arena_alloc(a, sz);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ((uintptr_t)p % 16, 0);
    }

    arena_free(a);
}

TEST(test_arena_alloc_multiple) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *p1 = arena_alloc(a, 16);
    char *p2 = arena_alloc(a, 16);
    char *p3 = arena_alloc(a, 32);

    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);

    /* Pointers must be distinct and non-overlapping */
    ASSERT_TRUE(p2 >= p1 + 16);
    ASSERT_TRUE(p3 >= p2 + 16);

    arena_free(a);
}

TEST(test_arena_alloc_write_and_read) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    char *buf = arena_alloc(a, 8);
    ASSERT_NOT_NULL(buf);

    buf[0] = 'h'; buf[1] = 'i'; buf[2] = '\0';
    ASSERT_STR_EQ(buf, "hi");

    arena_free(a);
}

TEST(test_arena_reset) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    arena_alloc(a, 256);
    ASSERT_TRUE(a->used > 0);

    size_t used_before = a->used;
    (void)used_before;

    arena_reset(a);
    ASSERT_EQ(a->used, 0);

    /* After reset, allocations should start from the same base */
    char *p = arena_alloc(a, 16);
    ASSERT_EQ((char *)p, a->base);

    arena_free(a);
}

TEST(test_arena_large_alloc) {
    /* Allocate a region larger than the default page, still fits within 1MB */
    Arena *a = arena_new(1024 * 1024);
    ASSERT_NOT_NULL(a);

    char *p = arena_alloc(a, 512 * 1024);
    ASSERT_NOT_NULL(p);

    /* Write to the whole region to trigger any page-fault issues */
    for (size_t i = 0; i < 512 * 1024; i += 4096)
        p[i] = (char)(i & 0xFF);

    arena_free(a);
}

TEST(test_arena_size_rounds_up_to_page) {
    /* Even a 1-byte arena gets at least one page */
    Arena *a = arena_new(1);
    ASSERT_NOT_NULL(a);
    ASSERT_TRUE(a->size >= 4096);
    arena_free(a);
}

int main(void)
{
    fprintf(stderr, "=== test_arena ===\n");
    RUN_TEST(test_arena_new_and_free);
    RUN_TEST(test_arena_alloc_basic);
    RUN_TEST(test_arena_alloc_alignment);
    RUN_TEST(test_arena_alloc_multiple);
    RUN_TEST(test_arena_alloc_write_and_read);
    RUN_TEST(test_arena_reset);
    RUN_TEST(test_arena_large_alloc);
    RUN_TEST(test_arena_size_rounds_up_to_page);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
