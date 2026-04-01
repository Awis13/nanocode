/*
 * message.h — Message type for provider API
 *
 * Extracted from provider.h so data-model headers (conversation.h, context.h)
 * can use Message without pulling in the I/O layer (provider.h → loop.h).
 */

#ifndef MESSAGE_H
#define MESSAGE_H

typedef struct {
    const char *role;     /* "user" or "assistant" */
    const char *content;
} Message;

#endif /* MESSAGE_H */
