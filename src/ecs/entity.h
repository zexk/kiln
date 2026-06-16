#pragma once

#include <stdint.h>

typedef uint64_t entity_t;

#define ECS_ENTITY_NULL ((entity_t)0)

#define ECS_ENTITY_INDEX(e) ((uint32_t)(e))
#define ECS_ENTITY_GEN(e) ((uint32_t)((e) >> 32))
#define ECS_ENTITY_MAKE(index, gen) (((entity_t)(gen) << 32) | (entity_t)(uint32_t)(index))
