#pragma once
/* Shim: maps kyub logger API onto kiln's log API. */
#include "log.h"

static inline void logger_init(const char *f) { log_init(f); }
static inline void logger_shutdown(void)       { log_shutdown(); }

#define CAT_WORLD    LOG_CAT_WORLD
#define CAT_RENDERER LOG_CAT_RENDERER
#define CAT_UI       LOG_CAT_UI
#define CAT_INPUT    LOG_CAT_INPUT
#define CAT_PLATFORM LOG_CAT_PLATFORM
