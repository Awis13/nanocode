/*
 * context.c — context window management implementation
 */

#include "context.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * ctx_estimate_tokens
 * ---------------------------------------------------------------------- */

int ctx_estimate_tokens(const char *text)
{
    if (!text) return 0;
    size_t len = strlen(text);
    /* ceil(len / 3.5) == (len * 2 + 6) / 7 */
    return (int)((len * 2 + 6) / 7);
}

/* -------------------------------------------------------------------------
 * ctx_conversation_tokens
 * ---------------------------------------------------------------------- */

int ctx_conversation_tokens(const Conversation *conv)
{
    if (!conv || !conv->turns || conv->nturn == 0) return 0;

    int total = 0;
    for (int i = 0; i < conv->nturn; i++) {
        total += ctx_estimate_tokens(conv->turns[i].role);
        total += ctx_estimate_tokens(conv->turns[i].content);
    }
    return total;
}

/* -------------------------------------------------------------------------
 * ctx_truncate
 * ---------------------------------------------------------------------- */

void ctx_truncate(Conversation *conv, int max_tokens, Arena *arena)
{
    if (!conv || !conv->turns || conv->nturn == 0) return;
    if (ctx_conversation_tokens(conv) <= max_tokens) return;

    /* Find end of leading system turns. */
    int nsys = 0;
    while (nsys < conv->nturn &&
           conv->turns[nsys].role != NULL &&
           strcmp(conv->turns[nsys].role, "system") == 0) {
        nsys++;
    }

    int body = conv->nturn - nsys;
    if (body <= CTX_KEEP_LAST_TURNS) return; /* protected section already minimal */

    /* Middle range [mid_start, mid_end) is eligible for dropping. */
    int mid_start = nsys;
    int mid_end   = conv->nturn - CTX_KEEP_LAST_TURNS;

    /* Compute token surplus; account for omission marker (~15 tokens). */
    int total  = ctx_conversation_tokens(conv);
    int excess = total - max_tokens + 15;

    /* Drop oldest middle turns until we cover the excess. */
    int drop_count = 0;
    int freed      = 0;
    for (int i = mid_start; i < mid_end && freed < excess; i++) {
        freed += ctx_estimate_tokens(conv->turns[i].role);
        freed += ctx_estimate_tokens(conv->turns[i].content);
        drop_count++;
    }

    if (drop_count == 0) return;

    /* Build omission marker content string. */
    char tmp[64];
    int mlen = snprintf(tmp, sizeof(tmp), "[%d earlier turn%s omitted]",
                        drop_count, drop_count == 1 ? "" : "s");
    char *mcontent = arena_alloc(arena, (size_t)(mlen + 1));
    memcpy(mcontent, tmp, (size_t)(mlen + 1));

    char *mrole = arena_alloc(arena, 5);
    memcpy(mrole, "user", 5);

    /* Allocate new turns array: system + marker + remaining middle + tail. */
    int new_nturn = nsys + 1 + (mid_end - (mid_start + drop_count))
                    + CTX_KEEP_LAST_TURNS;
    Turn *new_turns = arena_alloc(arena, (size_t)new_nturn * sizeof(Turn));

    int dst = 0;

    /* System turns. */
    for (int i = 0; i < nsys; i++)
        new_turns[dst++] = conv->turns[i];

    /* Omission marker. */
    new_turns[dst++] = (Turn){ .role = mrole, .content = mcontent, .is_tool = 0 };

    /* Remaining middle turns (after the dropped prefix). */
    for (int i = mid_start + drop_count; i < mid_end; i++)
        new_turns[dst++] = conv->turns[i];

    /* Protected tail. */
    for (int i = mid_end; i < conv->nturn; i++)
        new_turns[dst++] = conv->turns[i];

    conv->turns = new_turns;
    conv->nturn = new_nturn;
    conv->cap   = new_nturn;
}

/* -------------------------------------------------------------------------
 * ctx_compact
 * ---------------------------------------------------------------------- */

int ctx_compact(Conversation *conv, int max_tokens, float compact_threshold,
                ctx_compact_fn compact_fn, void *fn_userdata, Arena *arena)
{
    if (!conv || !conv->turns || conv->nturn == 0) return 0;

    int total = ctx_conversation_tokens(conv);
    if ((float)total <= compact_threshold * (float)max_tokens) return 0;

    /* Find end of leading system turns. */
    int nsys = 0;
    while (nsys < conv->nturn &&
           conv->turns[nsys].role != NULL &&
           strcmp(conv->turns[nsys].role, "system") == 0) {
        nsys++;
    }

    int body = conv->nturn - nsys;
    if (body <= CTX_KEEP_LAST_TURNS) return 0;

    int mid_start = nsys;
    int mid_end   = conv->nturn - CTX_KEEP_LAST_TURNS;

    /* Phase 1: replace tool output content with "[tool output cleared]". */
    static const char cleared[] = "[tool output cleared]";
    for (int i = mid_start; i < mid_end; i++) {
        if (conv->turns[i].is_tool) {
            char *c = arena_alloc(arena, sizeof(cleared));
            if (c) memcpy(c, cleared, sizeof(cleared));
            conv->turns[i].content = c;
        }
    }

    total = ctx_conversation_tokens(conv);
    if ((float)total <= compact_threshold * (float)max_tokens) return 1;

    /* Phase 2: summarize eligible range via compact_fn. */
    if (!compact_fn) return 1;

    /* Build summarization prompt: "role: content\n" per eligible turn. */
    size_t prompt_len = 0;
    for (int i = mid_start; i < mid_end; i++) {
        const char *r = conv->turns[i].role    ? conv->turns[i].role    : "";
        const char *c = conv->turns[i].content ? conv->turns[i].content : "";
        prompt_len += strlen(r) + 2 + strlen(c) + 1; /* "r: c\n" */
    }

    char *prompt = arena_alloc(arena, prompt_len + 1);
    if (!prompt) return 1;

    char *p = prompt;
    for (int i = mid_start; i < mid_end; i++) {
        const char *r = conv->turns[i].role    ? conv->turns[i].role    : "";
        const char *c = conv->turns[i].content ? conv->turns[i].content : "";
        size_t rlen = strlen(r), clen = strlen(c);
        memcpy(p, r, rlen); p += rlen;
        memcpy(p, ": ", 2); p += 2;
        memcpy(p, c, clen); p += clen;
        *p++ = '\n';
    }
    *p = '\0';

    char *summary = NULL;
    if (compact_fn(prompt, &summary, arena, fn_userdata) != 0 || !summary)
        return 1;

    /* Build summary turn content: "[Context summary: <summary>]" */
    static const char pfx[] = "[Context summary: ";
    static const char sfx[] = "]";
    size_t slen = (sizeof(pfx) - 1) + strlen(summary) + sizeof(sfx);
    char *scontent = arena_alloc(arena, slen);
    if (!scontent) return 1;
    char *sp = scontent;
    memcpy(sp, pfx, sizeof(pfx) - 1); sp += sizeof(pfx) - 1;
    size_t sumlen = strlen(summary);
    memcpy(sp, summary, sumlen); sp += sumlen;
    memcpy(sp, sfx, sizeof(sfx)); /* includes NUL */

    char *srole = arena_alloc(arena, 5);
    if (!srole) return 1;
    memcpy(srole, "user", 5);

    /* Replace eligible range with single summary turn. */
    int new_nturn = nsys + 1 + CTX_KEEP_LAST_TURNS;
    Turn *new_turns = arena_alloc(arena, (size_t)new_nturn * sizeof(Turn));
    if (!new_turns) return 1;

    int dst = 0;
    for (int i = 0; i < nsys; i++)
        new_turns[dst++] = conv->turns[i];

    new_turns[dst++] = (Turn){ .role = srole, .content = scontent, .is_tool = 0 };

    /* Protected tail. */
    for (int i = mid_end; i < conv->nturn; i++)
        new_turns[dst++] = conv->turns[i];

    conv->turns = new_turns;
    conv->nturn = new_nturn;
    conv->cap   = new_nturn;

    return 1;
}
