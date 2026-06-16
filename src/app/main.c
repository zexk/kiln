#include "core.h"
#include "platform.h"
#include "ecs.h"
#include "render.h"
#include "assets.h"

int main(void) {
    core_init();
    platform_init();
    ecs_init();
    render_init();
    assets_init();
    return 0;
}
