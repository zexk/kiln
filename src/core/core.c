#include "core.h"
#include "log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  if defined(__GLIBC__)
#    include <execinfo.h>
#  endif
#endif

void core_init(void) {
}

void core_oom_abort(const char *file, int line) {
    fprintf(stderr, "[fatal] out of memory at %s:%d\n", file, line);
    log_shutdown();
    abort();
}

static void crash_handler(int sig) {
    fprintf(stderr, "\n=== CRASH (signal %d) ===\n", sig);
#if !defined(_WIN32) && defined(__GLIBC__)
    void *addrs[32];
    int   n = backtrace(addrs, 32);
    backtrace_symbols_fd(addrs, n, STDERR_FILENO);
#endif
    fprintf(stderr, "========================\n");
    log_shutdown();
#ifdef _WIN32
    ExitProcess(1);
#else
    _Exit(1);
#endif
}

void core_install_crash_handler(void) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);
#ifndef _WIN32
    signal(SIGBUS,  crash_handler);
    signal(SIGILL,  crash_handler);
#endif
}

/* Directory containing the running executable, cached. Empty on failure. */
static const char *exe_dir(void) {
    static char dir[1024];
    static int resolved = 0;
    if (!resolved) {
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, dir, sizeof(dir) - 1);
        if (n > 0) {
            dir[n] = '\0';
            char *slash = strrchr(dir, '\\');
            if (!slash) slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
        } else {
            dir[0] = '\0';
        }
#else
        ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
        if (n <= 0) {
            dir[0] = '\0';
        } else {
            dir[n] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
        }
#endif
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
