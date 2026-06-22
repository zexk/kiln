#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define LOG_ROTATE_BYTES (10 * 1024 * 1024)

static FILE        *g_file;
static log_level_t  g_level   = LOG_INFO;
static size_t       g_bytes   = 0;
static char         g_path[4096];

static const char *s_levels[LOG_LEVEL_COUNT] = { "ERROR", "WARN ", "INFO ", "DEBUG" };
static const char *s_cats[LOG_CAT_COUNT]     = { "WORLD", "RENDER", "UI", "INPUT", "PLATFORM" };

static log_level_t parse_level(const char *s) {
    if (!s) return LOG_INFO;
    if (strcmp(s, "error") == 0) return LOG_ERROR;
    if (strcmp(s, "warn")  == 0) return LOG_WARN;
    if (strcmp(s, "info")  == 0) return LOG_INFO;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    return LOG_INFO;
}

static void open_file(void) {
    if (g_path[0] == '\0') return;
    g_file  = fopen(g_path, "a");
    g_bytes = 0;
}

static void rotate(void) {
    if (!g_file) return;
    fclose(g_file);
    g_file = NULL;

    char old_path[sizeof(g_path) + 4];
    snprintf(old_path, sizeof(old_path), "%s.old", g_path);
    rename(g_path, old_path);

    open_file();
}

void log_init(const char *filename) {
    g_level = parse_level(getenv("KILN_LOG"));

    if (filename) {
        snprintf(g_path, sizeof(g_path), "%s", filename);
        open_file();
    }
}

void log_shutdown(void) {
    if (g_file) { fclose(g_file); g_file = NULL; }
}

void log_set_level(log_level_t level) { g_level = level; }

void log_write(log_level_t level, log_category_t cat, const char *fmt, ...) {
    if (level > g_level) return;

    struct tm tm;
    int ms;
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    tm.tm_year = st.wYear - 1900; tm.tm_mon  = st.wMonth - 1;
    tm.tm_mday = st.wDay;         tm.tm_hour = st.wHour;
    tm.tm_min  = st.wMinute;      tm.tm_sec  = st.wSecond;
    ms = (int)st.wMilliseconds;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    ms = (int)(ts.tv_nsec / 1000000);
#endif

    char tsbuf[32];
    snprintf(tsbuf, sizeof(tsbuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);

    const char *lvl = (level < LOG_LEVEL_COUNT) ? s_levels[level] : "?????";
    const char *cat_s = (cat < LOG_CAT_COUNT)   ? s_cats[cat]     : "?";

    va_list ap;

    /* write to file */
    if (g_file) {
        va_start(ap, fmt);
        int n = fprintf(g_file, "[%s] [%s] [%s] ", tsbuf, lvl, cat_s);
        n    += vfprintf(g_file, fmt, ap);
        n    += fprintf(g_file, "\n");
        va_end(ap);
        fflush(g_file);

        g_bytes += (size_t)(n > 0 ? n : 0);
        if (g_bytes >= LOG_ROTATE_BYTES) rotate();
    }

    /* always mirror to stderr */
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [%s] [%s] ", tsbuf, lvl, cat_s);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
