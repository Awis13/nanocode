/*
 * renderer.h — ANSI streaming markdown renderer
 *
 * Renders streaming LLM tokens to a terminal with markdown formatting,
 * word wrap, and syntax highlighting for fenced code blocks.
 */

#ifndef RENDERER_H
#define RENDERER_H

#include <stddef.h>
#include "util/arena.h"

typedef struct Renderer Renderer;

/*
 * Create a new renderer writing to file descriptor `fd`.
 * The Renderer struct itself is allocated from `arena`.
 * Terminal width is queried via TIOCGWINSZ; falls back to 80.
 * 256-colour support is detected from TERM / COLORTERM env vars.
 */
Renderer *renderer_new(int fd, Arena *arena);

/*
 * Feed a streaming token. `tok` need not be NUL-terminated.
 * Output may be written to `fd` immediately or held in a small buffer;
 * at most 3 bytes of lookahead are ever retained between calls.
 */
void renderer_token(Renderer *r, const char *tok, size_t len);

/*
 * Flush all buffered output, reset inline styles, and emit a trailing
 * newline if the cursor is not already at column 0.
 * Must be called at the end of every streaming response.
 */
void renderer_flush(Renderer *r);

/*
 * Flush the internal write buffer to the file descriptor.
 * (renderer_flush already calls this; exposed for unit tests.)
 */
void renderer_free(Renderer *r);

/* Override terminal width — useful for deterministic unit tests. */
void renderer_set_width(Renderer *r, int width);

#endif /* RENDERER_H */
