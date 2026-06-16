#include "arena.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <stdint.h>
#include <string.h>

static int is_aligned(const void *p, size_t align) {
    return ((uintptr_t)p & (align - 1)) == 0;
}

Test(arena, alloc_returns_distinct_nonoverlapping_chunks) {
    arena_t *a = arena_create(0);

    char *p = arena_alloc(a, 64);
    char *q = arena_alloc(a, 64);
    char *r = arena_alloc(a, 64);

    cr_assert(ne(ptr, p, NULL));
    cr_assert(ne(ptr, q, NULL));
    cr_assert(ne(ptr, r, NULL));

    /* writing one chunk must not disturb the others */
    memset(p, 0xAA, 64);
    memset(q, 0xBB, 64);
    memset(r, 0xCC, 64);

    for (int i = 0; i < 64; i++) {
        cr_assert(eq(int, (unsigned char)p[i], 0xAA));
        cr_assert(eq(int, (unsigned char)q[i], 0xBB));
        cr_assert(eq(int, (unsigned char)r[i], 0xCC));
    }

    arena_destroy(a);
}

Test(arena, default_alignment_is_respected) {
    arena_t *a = arena_create(0);

    /* odd-sized allocations must still leave the next pointer max-aligned */
    for (int i = 0; i < 32; i++) {
        void *p = arena_alloc(a, (size_t)(i * 3 + 1));
        cr_assert(is_aligned(p, __alignof__(long double)));
    }

    arena_destroy(a);
}

Test(arena, explicit_alignment_is_respected) {
    arena_t *a = arena_create(0);

    size_t aligns[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        arena_alloc(a, 1); /* misalign the cursor */
        void *p = arena_alloc_aligned(a, 48, aligns[i]);
        cr_assert(is_aligned(p, aligns[i]), "alignment %zu not satisfied", aligns[i]);
    }

    arena_destroy(a);
}

Test(arena, calloc_zeroes_memory) {
    arena_t *a = arena_create(0);

    /* dirty the arena first so calloc can't accidentally hand back fresh zeros */
    memset(arena_alloc(a, 256), 0xFF, 256);

    unsigned char *p = arena_calloc(a, 100, sizeof(unsigned char));
    for (int i = 0; i < 100; i++) {
        cr_assert(eq(int, p[i], 0));
    }

    arena_destroy(a);
}

Test(arena, oversized_request_gets_dedicated_block) {
    arena_t *a = arena_create(64); /* tiny blocks */

    /* far larger than the block size: must still succeed and be usable */
    size_t big = 64 * 1024;
    unsigned char *p = arena_alloc(a, big);
    cr_assert(ne(ptr, p, NULL));

    memset(p, 0x5A, big);
    cr_assert(eq(int, p[0], 0x5A));
    cr_assert(eq(int, p[big - 1], 0x5A));

    arena_destroy(a);
}

Test(arena, growth_across_many_blocks_keeps_data_intact) {
    arena_t *a = arena_create(128); /* force many block transitions */

    enum { N = 500 };
    int *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = arena_alloc(a, sizeof(int));
        *ptrs[i] = i;
    }

    /* every earlier allocation must still hold its value */
    for (int i = 0; i < N; i++) {
        cr_assert(eq(int, *ptrs[i], i));
    }

    arena_destroy(a);
}

Test(arena, reset_reuses_memory) {
    arena_t *a = arena_create(0);

    void *first = arena_alloc(a, 100);
    arena_alloc(a, 100);
    arena_alloc(a, 100);

    arena_reset(a);

    void *again = arena_alloc(a, 100);
    cr_assert(eq(ptr, again, first), "reset should hand back the same memory");

    arena_destroy(a);
}
