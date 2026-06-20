#ifndef ECS_H
#define ECS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ECS_MAX_COMPONENTS 32
#define ECS_NULL           0

typedef uint32_t Entity;

typedef struct ECS {
    int max_entities;
    uint32_t *masks;    /* component bitmask per entity */
    bool     *alive;    /* alive flag per entity */
    int      *free_list;
    int       free_count;

    size_t    comp_sizes[ECS_MAX_COMPONENTS];
    char     *comp_data[ECS_MAX_COMPONENTS];
    int       comp_count;
} ECS;

void    ecs_init(ECS *ecs, int max_entities);
void    ecs_shutdown(ECS *ecs);

int     ecs_register(ECS *ecs, size_t comp_size);
Entity  ecs_spawn(ECS *ecs);
void    ecs_despawn(ECS *ecs, Entity e);
bool    ecs_alive(const ECS *ecs, Entity e);

void*   ecs_add(ECS *ecs, Entity e, int comp_id);
void*   ecs_get(const ECS *ecs, Entity e, int comp_id);
void    ecs_remove(ECS *ecs, Entity e, int comp_id);
bool    ecs_has(const ECS *ecs, Entity e, int comp_id);

int     ecs_count(const ECS *ecs);

#endif // ECS_H
