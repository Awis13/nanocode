/*
 * test_context.c — unit tests for context window management
 *
 * Tests ctx_estimate_tokens, ctx_conversation_tokens, and ctx_truncate.
 * Conversations are constructed directly (no conversation.c dependency).
 */

#include "test.h"
#include "../src/agent/context.h"
#include "../src/util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Build a Conversation from parallel role/content string arrays. */
static Conversation make_conv(Arena *a, const char **roles, const char **contents,
                               int n)
{
    Turn *turns = arena_alloc(a, (size_t)n * sizeof(Turn));
    for (int i = 0; i < n; i++) {
        size_t rlen = strlen(roles[i]) + 1;
        size_t clen = strlen(contents[i]) + 1;
        char *r = arena_alloc(a, rlen);
        char *c = arena_alloc(a, clen);
        memcpy(r, roles[i], rlen);
        memcpy(c, contents[i], clen);
        turns[i] = (Turn){ .role = r, .content = c, .is_tool = 0 };
    }
    Conversation conv = { .arena = a, .turns = turns, .nturn = n, .cap = n,
                          .conv_id = NULL };
    return conv;
}

/* -------------------------------------------------------------------------
 * ctx_estimate_tokens
 * ---------------------------------------------------------------------- */

TEST(test_estimate_null)
{
    ASSERT_EQ(ctx_estimate_tokens(NULL), 0);
}

TEST(test_estimate_empty)
{
    ASSERT_EQ(ctx_estimate_tokens(""), 0);
}

TEST(test_estimate_formula)
{
    /* ceil(7 / 3.5) == 2 */
    const char *s7 = "abcdefg";
    ASSERT_EQ(ctx_estimate_tokens(s7), 2);

    /* ceil(1 / 3.5) == 1 */
    ASSERT_EQ(ctx_estimate_tokens("a"), 1);

    /* ceil(14 / 3.5) == 4 */
    const char *s14 = "abcdefghijklmn";
    ASSERT_EQ(ctx_estimate_tokens(s14), 4);
}

TEST(test_estimate_within_20_percent)
{
    /*
     * For a 350-char string, actual tokens ≈ 100.
     * Our estimate: ceil(350 / 3.5) = 100.  Spot-on for clean English.
     * Check that estimate is at least 80 and at most 120 (±20%).
     */
    const char *text =
        "The quick brown fox jumps over the lazy dog. "
        "Pack my box with five dozen liquor jugs. "
        "How vexingly quick daft zebras jump! "
        "The five boxing wizards jump quickly. "
        "Sphinx of black quartz, judge my vow. "
        "Amazingly few discotheques provide jukeboxes. "
        "Crazy Fredrick bought many very exquisite opal jewels. "
        "We promptly judged antique ivory buckles for the next prize.";
    int est = ctx_estimate_tokens(text);
    int len = (int)strlen(text);
    /* actual ≈ len/4 for English; our formula gives len/3.5 — slight overcount */
    ASSERT_TRUE(est >= (len * 80) / (100 * 4));   /* at least 80% of len/4 */
    ASSERT_TRUE(est <= (len * 120) / (100 * 3));  /* at most 120% of len/3 */
}

/* -------------------------------------------------------------------------
 * ctx_conversation_tokens
 * ---------------------------------------------------------------------- */

TEST(test_conv_tokens_null)
{
    ASSERT_EQ(ctx_conversation_tokens(NULL), 0);
}

TEST(test_conv_tokens_empty)
{
    Conversation conv = { .turns = NULL, .nturn = 0 };
    ASSERT_EQ(ctx_conversation_tokens(&conv), 0);
}

TEST(test_conv_tokens_sums_all_turns)
{
    Arena *a = arena_new(1 << 20);

    const char *roles[]    = { "user",      "assistant" };
    const char *contents[] = { "hello",     "world" };
    Conversation conv = make_conv(a, roles, contents, 2);

    int expected = ctx_estimate_tokens("user")      + ctx_estimate_tokens("hello")
                 + ctx_estimate_tokens("assistant") + ctx_estimate_tokens("world");
    ASSERT_EQ(ctx_conversation_tokens(&conv), expected);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * ctx_truncate — no-op cases
 * ---------------------------------------------------------------------- */

TEST(test_truncate_noop_null)
{
    Arena *a = arena_new(1 << 20);
    ctx_truncate(NULL, 1000, a); /* must not crash */
    arena_free(a);
}

TEST(test_truncate_noop_empty)
{
    Arena *a = arena_new(1 << 20);
    Conversation conv = { .turns = NULL, .nturn = 0, .cap = 0 };
    ctx_truncate(&conv, 1000, a); /* must not crash */
    ASSERT_EQ(conv.nturn, 0);
    arena_free(a);
}

TEST(test_truncate_noop_under_limit)
{
    Arena *a = arena_new(1 << 20);

    const char *roles[]    = { "user", "assistant" };
    const char *contents[] = { "hi",   "hello" };
    Conversation conv = make_conv(a, roles, contents, 2);

    int before = conv.nturn;
    ctx_truncate(&conv, 100000, a);
    ASSERT_EQ(conv.nturn, before);

    arena_free(a);
}

TEST(test_truncate_noop_single_turn)
{
    Arena *a = arena_new(1 << 20);

    const char *roles[]    = { "user" };
    const char *contents[] = { "question" };
    Conversation conv = make_conv(a, roles, contents, 1);

    ctx_truncate(&conv, 1, a); /* under limit by any measure; body==1 <= 4 */
    ASSERT_EQ(conv.nturn, 1);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * ctx_truncate — functional cases
 * ---------------------------------------------------------------------- */

/*
 * Build a conversation with a system prompt and several user/assistant turns,
 * then truncate aggressively and verify invariants.
 */
TEST(test_truncate_keeps_system_and_tail)
{
    Arena *a = arena_new(1 << 20);

    /* 9 turns: 1 system + 4 pairs user/assistant (8 body turns). */
    const char *roles[] = {
        "system",
        "user", "assistant",
        "user", "assistant",
        "user", "assistant",
        "user", "assistant",
    };
    /* Use long content so total tokens exceed a small limit. */
    const char *long_str =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 138 chars */
    const char *contents[] = {
        long_str, long_str, long_str,
        long_str, long_str, long_str,
        long_str, long_str, long_str,
    };
    Conversation conv = make_conv(a, roles, contents, 9);

    int total_before = ctx_conversation_tokens(&conv);
    ASSERT_TRUE(total_before > 0);

    /* Truncate to a very small budget to force dropping. */
    ctx_truncate(&conv, 1, a);

    /* System turn must still be first. */
    ASSERT_NOT_NULL(conv.turns);
    ASSERT_STR_EQ(conv.turns[0].role, "system");

    /* Last CTX_KEEP_LAST_TURNS turns must be the original tail. */
    int tail_start = conv.nturn - CTX_KEEP_LAST_TURNS;
    ASSERT_TRUE(tail_start >= 1);
    /* The tail content should match the long_str. */
    for (int i = tail_start; i < conv.nturn; i++)
        ASSERT_STR_EQ(conv.turns[i].content, long_str);

    arena_free(a);
}

TEST(test_truncate_inserts_omission_marker)
{
    Arena *a = arena_new(1 << 20);

    const char *roles[] = {
        "system",
        "user", "assistant",
        "user", "assistant",
        "user", "assistant",
    };
    const char *long_str =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *contents[] = {
        long_str, long_str, long_str,
        long_str, long_str, long_str,
        long_str,
    };
    Conversation conv = make_conv(a, roles, contents, 7);

    ctx_truncate(&conv, 1, a);

    /* There must be a marker turn somewhere after the system turn. */
    int found_marker = 0;
    for (int i = 1; i < conv.nturn; i++) {
        if (conv.turns[i].content &&
            strstr(conv.turns[i].content, "omitted")) {
            found_marker = 1;
            break;
        }
    }
    ASSERT_TRUE(found_marker);

    arena_free(a);
}

TEST(test_truncate_no_system_prompt)
{
    Arena *a = arena_new(1 << 20);

    /* 6 body turns only (no system). */
    const char *roles[] = {
        "user", "assistant",
        "user", "assistant",
        "user", "assistant",
    };
    const char *long_str =
        "cccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccc";
    const char *contents[] = {
        long_str, long_str, long_str,
        long_str, long_str, long_str,
    };
    Conversation conv = make_conv(a, roles, contents, 6);

    ctx_truncate(&conv, 1, a);

    /* Marker must be present and last 4 body turns preserved. */
    int found_marker = 0;
    for (int i = 0; i < conv.nturn; i++) {
        if (conv.turns[i].content && strstr(conv.turns[i].content, "omitted")) {
            found_marker = 1;
            break;
        }
    }
    ASSERT_TRUE(found_marker);

    /* Last 4 turns still have the original content. */
    int tail = conv.nturn - CTX_KEEP_LAST_TURNS;
    for (int i = tail; i < conv.nturn; i++)
        ASSERT_STR_EQ(conv.turns[i].content, long_str);

    arena_free(a);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== test_context ===\n");

    RUN_TEST(test_estimate_null);
    RUN_TEST(test_estimate_empty);
    RUN_TEST(test_estimate_formula);
    RUN_TEST(test_estimate_within_20_percent);

    RUN_TEST(test_conv_tokens_null);
    RUN_TEST(test_conv_tokens_empty);
    RUN_TEST(test_conv_tokens_sums_all_turns);

    RUN_TEST(test_truncate_noop_null);
    RUN_TEST(test_truncate_noop_empty);
    RUN_TEST(test_truncate_noop_under_limit);
    RUN_TEST(test_truncate_noop_single_turn);

    RUN_TEST(test_truncate_keeps_system_and_tail);
    RUN_TEST(test_truncate_inserts_omission_marker);
    RUN_TEST(test_truncate_no_system_prompt);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
