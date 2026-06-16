#pragma once

#include <stddef.h>

/* A bump (linear) allocator: allocations are carved sequentially out of large
   backing blocks and are never freed individually — the whole arena is reset or
   destroyed at once. Backing blocks are chained, so an arena grows without bound
   and a request larger than the block size simply gets its own dedicated block.

   Intended for create-once data whose lifetime matches the arena's (e.g. the ECS
   world and its archetype tables, all reclaimed in one shot at world teardown).
   It deliberately offers no per-allocation free or realloc. */
typedef struct arena arena_t;

/* `block_size` is the capacity of each backing block; pass 0 for a sane default.
   Aborts on allocation failure. */
arena_t *arena_create(size_t block_size);
void arena_destroy(arena_t *arena);

/* Allocate `size` bytes aligned to the platform's max alignment. Contents are
   uninitialized. Never returns NULL (aborts on out-of-memory). */
void *arena_alloc(arena_t *arena, size_t size);

/* As arena_alloc, but with explicit `align` (must be a non-zero power of two). */
void *arena_alloc_aligned(arena_t *arena, size_t size, size_t align);

/* Allocate `count * size` zeroed bytes (aborts on size overflow). */
void *arena_calloc(arena_t *arena, size_t count, size_t size);

/* Rewind to empty, retaining the backing blocks so the memory is reused on the
   next round of allocations. Pointers handed out before the reset are invalid. */
void arena_reset(arena_t *arena);
