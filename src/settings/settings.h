#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

/* Engine (graphical/window) settings persisted to disk.
   width/height are startup-only (passed to window_create); vsync and
   fps_limit can be applied or changed at runtime via the renderer API. */
typedef struct {
    uint32_t width;
    uint32_t height;
    bool     vsync;
    float    fps_limit;    /* Hz; 0 = unlimited */
    bool     bloom;
    float    bloom_threshold;
    float    bloom_strength;
    float    bloom_exposure;
} engine_settings_t;

/* Maps a named game action to a physical key. */
#define SETTINGS_ACTION_LEN   32
#define SETTINGS_MAX_BINDINGS 64

typedef struct {
    char      action[SETTINGS_ACTION_LEN];
    keycode_t key;
} key_binding_t;

typedef struct {
    key_binding_t entries[SETTINGS_MAX_BINDINGS];
    int           count;
} key_bindings_t;

typedef struct {
    engine_settings_t engine;
    key_bindings_t    bindings;
} settings_t;

/* Fill *s with sensible defaults. default_bindings/n register the game's
   named actions with their default keys (pass NULL/0 for engine-only use).
   Call before settings_load so unknown actions keep their defaults. */
void settings_init(settings_t *s,
                   const key_binding_t *default_bindings, int n);

/* Load a settings file into *s.  Missing keys keep their current values;
   unknown keys are silently ignored.  Returns false only on I/O failure. */
bool settings_load(settings_t *s, const char *path);

/* Persist *s to path atomically (writes a .tmp sibling then renames). */
bool settings_save(const settings_t *s, const char *path);

/* Return the key bound to action, or KEY_UNKNOWN if not registered. */
keycode_t settings_get_key(const settings_t *s, const char *action);

/* Rebind action to key.  Adds a new entry if the action is not already
   registered (up to SETTINGS_MAX_BINDINGS). */
void settings_set_key(settings_t *s, const char *action, keycode_t key);

/* Convert between keycode_t and its canonical name string ("W", "SPACE", …).
   key_name_to_code returns KEY_UNKNOWN for unrecognised names. */
const char *key_code_to_name(keycode_t code);
keycode_t   key_name_to_code(const char *name);
