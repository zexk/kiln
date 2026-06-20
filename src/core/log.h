#pragma once
#include <stdarg.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
    LOG_LEVEL_COUNT,
} log_level_t;

typedef enum {
    LOG_CAT_WORLD = 0,
    LOG_CAT_RENDERER,
    LOG_CAT_UI,
    LOG_CAT_INPUT,
    LOG_CAT_PLATFORM,
    LOG_CAT_COUNT,
} log_category_t;

void log_init(const char *filename);
void log_shutdown(void);
void log_set_level(log_level_t level);
void log_write(log_level_t level, log_category_t cat, const char *fmt, ...);

#ifdef ENABLE_LOGGER
#  define LOG_ERROR(cat, ...) log_write(LOG_ERROR, cat, __VA_ARGS__)
#  define LOG_WARN(cat, ...)  log_write(LOG_WARN,  cat, __VA_ARGS__)
#  define LOG_INFO(cat, ...)  log_write(LOG_INFO,  cat, __VA_ARGS__)
#  define LOG_DEBUG(cat, ...) log_write(LOG_DEBUG, cat, __VA_ARGS__)
#else
#  define LOG_ERROR(cat, ...) ((void)0)
#  define LOG_WARN(cat, ...)  ((void)0)
#  define LOG_INFO(cat, ...)  ((void)0)
#  define LOG_DEBUG(cat, ...) ((void)0)
#endif
