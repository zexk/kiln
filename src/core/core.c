#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void core_init(void) {
}

/* Directory containing the running executable (Linux), cached. Empty on
   failure. */
static const char *exe_dir(void) {
    static char dir[1024];
    static int resolved = 0;
    if (!resolved) {
        ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
        if (n <= 0) {
            dir[0] = '\0';
        } else {
            dir[n] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
            }
        }
        resolved = 1;
    }
    return dir;
}

void core_resource_dir(char *out, size_t cap, const char *env_var,
                       const char *name, const char *fallback) {
    const char *env = env_var ? getenv(env_var) : NULL;
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return;
    }

    const char *exe = exe_dir();
    if (exe[0]) {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/../share/kiln/%s", exe,
                 name);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, cap, "%s", candidate);
            return;
        }
    }

    snprintf(out, cap, "%s", fallback);
}
