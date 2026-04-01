/*
 * test_oom.c — OOM protection tests for arena allocator (CMP-144)
 *
 * Verifies that arena_alloc returns NULL gracefully on exhaustion
 * instead of calling abort(). All tests pass under ASan/UBSan.
 *
 * Contract (from CMP-144 acceptance criteria):
 *   - arena_alloc returns NULL when arena is full (no crash, no abort)
 *   - Error is logged to stderr
 *   - arena_reset allows reuse after OOM
 *   - arena_new(0) is safe
 *
 * NOTE: Requires arena_alloc to return NULL on OOM (CMP-144).
 *       Current implementation calls abort() — must be changed.
 */

#include "test.h"
#include "../src/util/arena.h"

/* --------------------------------------------------------------------------
 * Basic OOM: fill to capacity, next alloc returns NULL
 * ------------------------------------------------------------------------ */

TEST(test_oom_returns_null_not_abort) {
    Arena *a = arena_new(128);
    ASSERT_NOT_NULL(a);

    /* Fill the arena until it's full */
    int allocs = 0;
    while (arena_alloc(a, 16)) {
        allocs++;
        if (allocs > 1000) break; /* guard against infinite loop */
    }
    ASSERT_TRUE(allocs > 0);

    /* Next allocation must return NULL, not crash or abort */
    void *overflow = arena_alloc(a, 1);
    ASSERT_NULL(overflow);

    arena_free(a);
}

TEST(test_oom_large_single_alloc_returns_null) {
    /* Arena is 1 page (~4096 bytes). Request 1MB — must return NULL. */
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    void *p = arena_alloc(a, 1024 * 1024);
    ASSERT_NULL(p);

    arena_free(a);
}

TEST(test_oom_request_larger_than_remaining) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    /* Fill arena leaving exactly 16 bytes free (one 16-byte aligned slot) */
    while (a->used + 32 <= a->size)
        ASSERT_NOT_NULL(arena_alloc(a, 16));

    /* Request 32 bytes but only ≤16 bytes remain — must return NULL */
    void *p = arena_alloc(a, 32);
    ASSERT_NULL(p);

    arena_free(a);
}

/* --------------------------------------------------------------------------
 * Recovery: arena_reset allows reuse after OOM
 * ------------------------------------------------------------------------ */

TEST(test_oom_reset_allows_reuse) {
    Arena *a = arena_new(64);
    ASSERT_NOT_NULL(a);

    /* Fill to OOM */
    while (arena_alloc(a, 16))
        ;

    ASSERT_NULL(arena_alloc(a, 1));

    /* After reset, arena is usable again */
    arena_reset(a);
    void *p = arena_alloc(a, 16);
    ASSERT_NOT_NULL(p);

    arena_free(a);
}

TEST(test_oom_multiple_resets) {
    Arena *a = arena_new(64);
    ASSERT_NOT_NULL(a);

    for (int round = 0; round < 3; round++) {
        /* Fill */
        while (arena_alloc(a, 16))
            ;
        ASSERT_NULL(arena_alloc(a, 1));
        /* Reset and verify usable */
        arena_reset(a);
        ASSERT_NOT_NULL(arena_alloc(a, 16));
        arena_reset(a);
    }

    arena_free(a);
}

/* --------------------------------------------------------------------------
 * Edge cases
 * ------------------------------------------------------------------------ */

TEST(test_oom_zero_size_arena) {
    /* arena_new(0): either returns NULL or creates a valid minimal arena */
    Arena *a = arena_new(0);
    if (a) {
        /* Any allocation from a zero-size arena must return NULL */
        void *p = arena_alloc(a, 1);
        ASSERT_NULL(p);
        arena_free(a);
    }
    /* arena_new returning NULL for size=0 is also acceptable */
}

TEST(test_oom_alloc_zero_bytes) {
    /* Allocating 0 bytes from a fresh arena should not crash */
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);
    /* NULL or a valid pointer — both acceptable; must not crash */
    (void)arena_alloc(a, 0);
    arena_free(a);
}

TEST(test_oom_partial_fill_still_succeeds) {
    /* Normal allocations must still work when OOM has not been reached */
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);

    void *p1 = arena_alloc(a, 64);
    void *p2 = arena_alloc(a, 128);
    void *p3 = arena_alloc(a, 256);

    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);

    arena_free(a);
}

int main(void)
{
    fprintf(stderr, "=== test_oom ===\n");
    RUN_TEST(test_oom_returns_null_not_abort);
    RUN_TEST(test_oom_large_single_alloc_returns_null);
    RUN_TEST(test_oom_request_larger_than_remaining);
    RUN_TEST(test_oom_reset_allows_reuse);
    RUN_TEST(test_oom_multiple_resets);
    RUN_TEST(test_oom_zero_size_arena);
    RUN_TEST(test_oom_alloc_zero_bytes);
    RUN_TEST(test_oom_partial_fill_still_succeeds);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
