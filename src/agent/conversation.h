/*
 * conversation.h — conversation data types
 *
 * Types only. Function implementations are in conversation.c (CMP-118).
 * This header is included by context.h so token/truncation logic can
 * operate on Conversation objects without depending on the full impl.
 */

#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "../util/arena.h"

typedef struct {
    char  *role;     /* "user", "assistant", "system", "tool_result" */
    char  *content;  /* text or JSON tool_use/tool_result block */
    int    is_tool;  /* 1 if this is a tool_use or tool_result message */
} Turn;

typedef struct {
    Arena  *arena;
    Turn   *turns;   /* arena-allocated array */
    int     nturn;   /* current count */
    int     cap;     /* allocated capacity */
    char   *conv_id; /* uuid, for persistence */
} Conversation;

#endif /* CONVERSATION_H */
