/*
 * context.h — context window management: token counting, truncation
 *
 * Prevents context overflow by tracking token usage and truncating
 * the conversation history when it approaches provider limits.
 *
 * Truncation priority order (highest to lowest):
 *   1. System prompt turns (role "system") — always kept
 *   2. Last CTX_KEEP_LAST_TURNS body turns — always kept
 *   3. Oldest body turns — dropped first, replaced with omission marker
 */

#ifndef CONTEXT_H
#define CONTEXT_H

#include "conversation.h"

/* Number of most-recent body turns always preserved during truncation. */
#define CTX_KEEP_LAST_TURNS 4

/* Provider token limits (configurable at runtime via ctx_truncate max_tokens). */
#define CTX_MAX_TOKENS_CLAUDE  180000
#define CTX_MAX_TOKENS_OPENAI  128000
#define CTX_MAX_TOKENS_OLLAMA    8000

/*
 * Estimate token count for a NUL-terminated string.
 * Uses conservative heuristic: ceil(len / 3.5).
 * Accurate to within ~20% for typical English/code text.
 * Returns 0 for NULL input.
 */
int ctx_estimate_tokens(const char *text);

/*
 * Estimate total tokens for all turns in a conversation.
 * Includes both role and content fields of each turn.
 * Returns 0 for NULL or empty conversation.
 */
int ctx_conversation_tokens(const Conversation *conv);

/*
 * Truncate conv so it fits within max_tokens.
 *
 * Drops the oldest body turns (after any system turns) first, replacing
 * the dropped range with a single user turn:
 *   "[N earlier turns omitted]"
 *
 * System turns and the last CTX_KEEP_LAST_TURNS body turns are never
 * dropped. If the protected turns alone exceed max_tokens, the
 * conversation is left unchanged.
 *
 * All new allocations use arena. The original Turn array is not freed
 * (arena semantics — no individual frees).
 */
void ctx_truncate(Conversation *conv, int max_tokens, Arena *arena);

/* Compaction threshold — compact when tokens exceed this fraction of max. */
#define CTX_COMPACT_THRESHOLD 0.95f

/*
 * Callback type for Phase 2 summarization.
 * Receives the prompt string and must write an arena-allocated summary to *out.
 * Returns 0 on success, non-zero on failure.
 */
typedef int (*ctx_compact_fn)(
    const char *prompt,
    char      **out,
    Arena      *arena,
    void       *userdata
);

/*
 * Compact conv to bring token usage under compact_threshold * max_tokens.
 *
 * Phase 1: replaces content of is_tool turns in the eligible range
 *          (after system turns, before last CTX_KEEP_LAST_TURNS) with
 *          "[tool output cleared]". Re-measures; returns 1 if now under
 *          threshold.
 *
 * Phase 2: if still over threshold and compact_fn != NULL, builds a
 *          summarization prompt from the eligible range (one "role: content"
 *          line per turn), calls compact_fn, and replaces the entire
 *          eligible range with a single user turn:
 *          "[Context summary: <output>]". Returns 1.
 *
 * If compact_fn is NULL, Phase 2 is skipped; caller falls back to
 * ctx_truncate. Returns 1 if Phase 1 ran.
 *
 * Returns 0 if tokens <= compact_threshold * max_tokens (no action needed).
 * All allocations use arena.
 */
int ctx_compact(Conversation *conv, int max_tokens, float compact_threshold,
                ctx_compact_fn compact_fn, void *fn_userdata, Arena *arena);

#endif /* CONTEXT_H */
