#pragma once

#include <stddef.h>
#include <stdint.h>

#define ECS_MAX_COMPONENTS 128

typedef uint16_t component_id_t;

typedef struct {
    const char *name;
    size_t size;
    size_t align;
} component_info_t;
