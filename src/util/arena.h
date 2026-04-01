/*
 * arena.h — arena allocator
 *
 * Single large mmap allocation, bump-pointer allocation within it.
 * No per-allocation frees; reset or free the whole arena at once.
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct {
    char  *base;     /* start of mapped region */
    size_t size;     /* logical capacity (user-requested size) */
    size_t mapsize;  /* actual mmap size (page-rounded, used by arena_free) */
    size_t used;     /* bytes allocated so far */
} Arena;

/* Allocate a new arena of at least `size` bytes. Returns NULL on failure. */
Arena *arena_new(size_t size);

/* Allocate `size` bytes from the arena. Returns NULL on OOM (no abort). */
void  *arena_alloc(Arena *a, size_t size);

/* Reset the bump pointer — all memory is reusable but NOT zeroed. */
void   arena_reset(Arena *a);

/* Unmap the arena's memory and free the Arena struct itself. */
void   arena_free(Arena *a);

#endif /* ARENA_H */
