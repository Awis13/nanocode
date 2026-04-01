/*
 * conversation.h — conversation data types and management functions
 *
 * Types only were here previously. Function implementations are in
 * conversation.c (CMP-118).
 * This header is included by context.h so token/truncation logic can
 * operate on Conversation objects without depending on the full impl.
 */

#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "../util/arena.h"
#include "../api/provider.h"  /* for Message type */

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

/* Create a new conversation in the given arena. Returns NULL on failure. */
Conversation *conv_new(Arena *arena);

/*
 * Free a conversation (arena semantics — NULLify pointers only).
 * Safe to call on NULL.
 */
void          conv_free(Conversation *conv);

/* Add a user/assistant/system turn. Silently drops on allocation failure. */
void          conv_add(Conversation *conv, const char *role, const char *content);

/*
 * Add an assistant tool_use turn.
 * Builds Claude-format JSON: [{"type":"tool_use","id":"...","name":"...","input":...}]
 */
void          conv_add_tool_use(Conversation *conv, const char *id,
                                const char *name, const char *input_json);

/*
 * Add a user tool_result turn.
 * Builds Claude-format JSON: [{"type":"tool_result","tool_use_id":"...","content":"..."}]
 * Content string is JSON-escaped.
 */
void          conv_add_tool_result(Conversation *conv, const char *tool_use_id,
                                   const char *content);

/*
 * Convert conversation turns to a Message array (arena-allocated).
 * Sets *out_nmsg to the number of messages.
 * Returns NULL if conv is NULL or empty.
 */
Message      *conv_to_messages(const Conversation *conv, int *out_nmsg, Arena *arena);

/* Serialize conversation to JSON file. Returns 0 on success, -1 on failure. */
int           conv_save(const Conversation *conv, const char *path);

/* Load a conversation from a JSON file. Returns NULL on failure. */
Conversation *conv_load(Arena *arena, const char *path);

#endif /* CONVERSATION_H */
