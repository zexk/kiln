#include "arena.h"
#include "core.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)

/* Maximal scalar alignment without relying on C11's max_align_t. */
typedef union {
    long long ll;
    long double ld;
    void *p;
} arena_max_align_t;
#define ARENA_DEFAULT_ALIGN (__alignof__(arena_max_align_t))

typedef struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t capacity;
    unsigned char data[]; /* flexible array member (C99) */
} arena_block_t;

struct arena {
    arena_block_t *first;   /* head of the block list */
    arena_block_t *current; /* block currently being filled */
    size_t block_size;      /* default capacity for new blocks */
};

/* Invariant: blocks before `current` are filled this cycle; blocks after it are
   stale (used == 0) and available for reuse. Each fresh block is spliced in
   immediately after `current` and becomes the new `current`, so the invariant
   holds across both fresh allocation and reuse. */

static arena_block_t *block_create(size_t capacity) {
    arena_block_t *b = malloc(sizeof(arena_block_t) + capacity);
    CORE_CHECK_ALLOC(b);
    b->next = NULL;
    b->used = 0;
    b->capacity = capacity;
    return b;
}

/* Bump-allocate from `b`, aligning the returned address (not just the offset,
   since data[] is not necessarily over-aligned). Returns NULL if it won't fit. */
static void *block_try_alloc(arena_block_t *b, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)b->data;
    uintptr_t p = (base + b->used + (align - 1)) & ~((uintptr_t)align - 1);
    size_t offset = (size_t)(p - base);

    if (offset + size > b->capacity) {
        return NULL;
    }
    b->used = offset + size;
    return (void *)p;
}

arena_t *arena_create(size_t block_size) {
    if (block_size == 0) {
        block_size = ARENA_DEFAULT_BLOCK_SIZE;
    }

    arena_t *arena = malloc(sizeof(arena_t));
    CORE_CHECK_ALLOC(arena);
    arena->block_size = block_size;
    arena->first = block_create(block_size);
    arena->current = arena->first;
    return arena;
}

void arena_destroy(arena_t *arena) {
    arena_block_t *b = arena->first;
    while (b) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(arena);
}

void *arena_alloc_aligned(arena_t *arena, size_t size, size_t align) {
    assert(align != 0 && (align & (align - 1)) == 0); /* power of two */

    void *p = block_try_alloc(arena->current, size, align);
    if (p) {
        return p;
    }

    /* current is full; reuse the next stale block if the request fits there. */
    arena_block_t *next = arena->current->next;
    if (next) {
        next->used = 0;
        p = block_try_alloc(next, size, align);
        if (p) {
            arena->current = next;
            return p;
        }
    }

    /* Otherwise splice in a fresh block, oversized if the request demands it. */
    size_t cap = arena->block_size;
    size_t need = size + align; /* worst-case alignment padding from data[] */
    if (need > cap) {
        cap = need;
    }

    arena_block_t *fresh = block_create(cap);
    fresh->next = arena->current->next;
    arena->current->next = fresh;
    arena->current = fresh;

    p = block_try_alloc(fresh, size, align);
    assert(p != NULL);
    return p;
}

void *arena_alloc(arena_t *arena, size_t size) {
    return arena_alloc_aligned(arena, size, ARENA_DEFAULT_ALIGN);
}

void *arena_calloc(arena_t *arena, size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size) {
        abort(); /* multiplication overflow */
    }

    void *p = arena_alloc_aligned(arena, total, ARENA_DEFAULT_ALIGN);
    memset(p, 0, total);
    return p;
}

void arena_reset(arena_t *arena) {
    for (arena_block_t *b = arena->first; b; b = b->next) {
        b->used = 0;
    }
    arena->current = arena->first;
}
