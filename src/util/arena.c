/*
 * arena.c — arena allocator implementation
 */

#include "arena.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>

/* Align allocations to 16 bytes — satisfies all fundamental type alignment. */
#define ARENA_ALIGN 16

static size_t align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

Arena *arena_new(size_t size)
{
    if (size == 0)
        return NULL;

    Arena *a = malloc(sizeof(Arena));
    if (!a)
        return NULL;

    size_t map_size = align_up(size, 4096);
    void *mem = mmap(NULL, map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (mem == MAP_FAILED) {
        free(a);
        return NULL;
    }

    a->base    = mem;
    a->size    = size;      /* logical capacity — enforced by arena_alloc */
    a->mapsize = map_size;  /* actual mmap size — used only by arena_free */
    a->used    = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size)
{
    if (size == 0)
        return NULL;
    size_t aligned = align_up(size, ARENA_ALIGN);
    if (a->used + aligned > a->size) {
        fprintf(stderr, "arena: OOM — requested %zu, used %zu/%zu\n",
                aligned, a->used, a->size);
        return NULL;
    }
    void *ptr = a->base + a->used;
    a->used += aligned;
    return ptr;
}

void arena_reset(Arena *a)
{
    a->used = 0;
}

void arena_free(Arena *a)
{
    munmap(a->base, a->mapsize);
    free(a);
}
