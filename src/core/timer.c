#include "timer.h"

#ifdef _WIN32
#include <windows.h>
double kln_timer_now(void) {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
double kln_timer_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

void kln_timer_sleep(double seconds) {
    if (seconds <= 0.0) return;
    double target = kln_timer_now() + seconds;
    /* Coarse OS sleep for everything except the last millisecond. */
    double coarse = seconds - 0.001;
    if (coarse > 0.0) {
#ifdef _WIN32
        Sleep((DWORD)(coarse * 1000.0));
#else
        struct timespec ts;
        ts.tv_sec  = (time_t)coarse;
        ts.tv_nsec = (long)((coarse - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
#endif
    }
    /* Busy-wait for the remaining sub-millisecond budget. */
    while (kln_timer_now() < target) {}
}

void kln_timer_init(kln_timer_t *t) { t->start = kln_timer_now(); }

double kln_timer_elapsed(const kln_timer_t *t) { return kln_timer_now() - t->start; }

double kln_timer_reset(kln_timer_t *t) {
    double now = kln_timer_now();
    double elapsed = now - t->start;
    t->start = now;
    return elapsed;
}
