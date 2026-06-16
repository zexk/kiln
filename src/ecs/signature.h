#pragma once

#include <stdint.h>

#include "component.h"

#define ECS_SIGNATURE_WORDS ((ECS_MAX_COMPONENTS + 63) / 64)

typedef struct {
    uint64_t bits[ECS_SIGNATURE_WORDS];
} signature_t;

static inline void signature_clear(signature_t *s) {
    for (int i = 0; i < ECS_SIGNATURE_WORDS; i++) {
        s->bits[i] = 0;
    }
}

static inline void signature_set(signature_t *s, component_id_t id) {
    s->bits[id / 64] |= (uint64_t)1 << (id % 64);
}

static inline void signature_unset(signature_t *s, component_id_t id) {
    s->bits[id / 64] &= ~((uint64_t)1 << (id % 64));
}

static inline int signature_has(const signature_t *s, component_id_t id) {
    return (int)((s->bits[id / 64] >> (id % 64)) & 1u);
}

static inline int signature_equal(const signature_t *a, const signature_t *b) {
    for (int i = 0; i < ECS_SIGNATURE_WORDS; i++) {
        if (a->bits[i] != b->bits[i]) return 0;
    }
    return 1;
}

/* does `s` contain every bit set in `required`? */
static inline int signature_is_superset(const signature_t *s, const signature_t *required) {
    for (int i = 0; i < ECS_SIGNATURE_WORDS; i++) {
        if ((s->bits[i] & required->bits[i]) != required->bits[i]) return 0;
    }
    return 1;
}

/* does `s` share any bit with `other`? */
static inline int signature_intersects(const signature_t *s, const signature_t *other) {
    for (int i = 0; i < ECS_SIGNATURE_WORDS; i++) {
        if (s->bits[i] & other->bits[i]) return 1;
    }
    return 0;
}
