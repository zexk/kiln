#include "app.h"

int main(void) {
    app_t app = {0};

    if (!app_init(&app)) {
        return 1;
    }

    app_run(&app);
    app_shutdown(&app);
    return 0;
}
