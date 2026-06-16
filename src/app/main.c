#include "core.h"
#include "platform.h"
#include "ecs.h"
#include "render.h"
#include "assets.h"

int main(void) {
    core_init();
    platform_init();

    world_t *world = world_create();

    render_init();
    assets_init();

    world_destroy(world);
    return 0;
}
