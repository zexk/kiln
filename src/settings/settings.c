#include "settings.h"

#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- key name table ---------------------------------------------------- */

static const struct {
    keycode_t   code;
    const char *name;
} KEY_NAMES[] = {
    { KEY_UNKNOWN,   "UNKNOWN"   },
    { KEY_ESCAPE,    "ESCAPE"    },
    { KEY_SPACE,     "SPACE"     },
    { KEY_RETURN,    "RETURN"    },
    { KEY_TAB,       "TAB"       },
    { KEY_BACKSPACE, "BACKSPACE" },
    { KEY_DELETE,    "DELETE"    },
    { KEY_A, "A" }, { KEY_B, "B" }, { KEY_C, "C" }, { KEY_D, "D" },
    { KEY_E, "E" }, { KEY_F, "F" }, { KEY_G, "G" }, { KEY_H, "H" },
    { KEY_I, "I" }, { KEY_J, "J" }, { KEY_K, "K" }, { KEY_L, "L" },
    { KEY_M, "M" }, { KEY_N, "N" }, { KEY_O, "O" }, { KEY_P, "P" },
    { KEY_Q, "Q" }, { KEY_R, "R" }, { KEY_S, "S" }, { KEY_T, "T" },
    { KEY_U, "U" }, { KEY_V, "V" }, { KEY_W, "W" }, { KEY_X, "X" },
    { KEY_Y, "Y" }, { KEY_Z, "Z" },
    { KEY_0, "0" }, { KEY_1, "1" }, { KEY_2, "2" }, { KEY_3, "3" },
    { KEY_4, "4" }, { KEY_5, "5" }, { KEY_6, "6" }, { KEY_7, "7" },
    { KEY_8, "8" }, { KEY_9, "9" },
    { KEY_LEFT,  "LEFT"  }, { KEY_RIGHT, "RIGHT" },
    { KEY_UP,    "UP"    }, { KEY_DOWN,  "DOWN"  },
    { KEY_F1,  "F1"  }, { KEY_F2,  "F2"  }, { KEY_F3,  "F3"  },
    { KEY_F4,  "F4"  }, { KEY_F5,  "F5"  }, { KEY_F6,  "F6"  },
    { KEY_F7,  "F7"  }, { KEY_F8,  "F8"  }, { KEY_F9,  "F9"  },
    { KEY_F10, "F10" }, { KEY_F11, "F11" }, { KEY_F12, "F12" },
};
#define KEY_NAMES_COUNT (int)(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]))

const char *key_code_to_name(keycode_t code) {
    for (int i = 0; i < KEY_NAMES_COUNT; i++) {
        if (KEY_NAMES[i].code == code) return KEY_NAMES[i].name;
    }
    return "UNKNOWN";
}

keycode_t key_name_to_code(const char *name) {
    for (int i = 0; i < KEY_NAMES_COUNT; i++) {
        if (strcmp(KEY_NAMES[i].name, name) == 0) return KEY_NAMES[i].code;
    }
    return KEY_UNKNOWN;
}

/* ---- binding helpers --------------------------------------------------- */

keycode_t settings_get_key(const settings_t *s, const char *action) {
    for (int i = 0; i < s->bindings.count; i++) {
        if (strcmp(s->bindings.entries[i].action, action) == 0)
            return s->bindings.entries[i].key;
    }
    return KEY_UNKNOWN;
}

void settings_set_key(settings_t *s, const char *action, keycode_t key) {
    for (int i = 0; i < s->bindings.count; i++) {
        if (strcmp(s->bindings.entries[i].action, action) == 0) {
            s->bindings.entries[i].key = key;
            return;
        }
    }
    if (s->bindings.count >= SETTINGS_MAX_BINDINGS) return;
    key_binding_t *b = &s->bindings.entries[s->bindings.count++];
    strncpy(b->action, action, SETTINGS_ACTION_LEN - 1);
    b->action[SETTINGS_ACTION_LEN - 1] = '\0';
    b->key = key;
}

/* ---- init + defaults --------------------------------------------------- */

void settings_init(settings_t *s,
                   const key_binding_t *default_bindings, int n) {
    s->engine.width          = 1280;
    s->engine.height         = 720;
    s->engine.vsync          = true;
    s->engine.fps_limit      = 0.0f;
    s->engine.bloom           = true;
    s->engine.bloom_threshold = 0.8f;
    s->engine.bloom_strength  = 0.5f;
    s->engine.bloom_exposure  = 1.0f;
    s->engine.fov             = 60.0f;
    s->engine.mouse_sensitivity = 0.002f;

    s->bindings.count = 0;
    for (int i = 0; i < n && i < SETTINGS_MAX_BINDINGS; i++) {
        s->bindings.entries[i] = default_bindings[i];
        s->bindings.count++;
    }
}

/* ---- INI parser -------------------------------------------------------- */

typedef enum { SEC_NONE, SEC_ENGINE, SEC_BINDINGS } section_t;

static void trim_right(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static const char *trim_left(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void parse_line(settings_t *s, section_t *sec, char *line) {
    trim_right(line);
    const char *p = trim_left(line);
    if (!*p || *p == '#' || *p == ';') return;

    if (*p == '[') {
        p++;
        if (strncmp(p, "engine]",   7) == 0) { *sec = SEC_ENGINE;   return; }
        if (strncmp(p, "bindings]", 9) == 0) { *sec = SEC_BINDINGS; return; }
        *sec = SEC_NONE;
        return;
    }

    const char *eq = strchr(p, '=');
    if (!eq) return;

    /* extract key (strip trailing whitespace) */
    char key[SETTINGS_ACTION_LEN];
    int klen = (int)(eq - p);
    while (klen > 0 && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) klen--;
    if (klen <= 0 || klen >= (int)sizeof(key)) return;
    memcpy(key, p, (size_t)klen);
    key[klen] = '\0';

    /* extract value (strip leading whitespace) */
    const char *val = trim_left(eq + 1);

    if (*sec == SEC_ENGINE) {
        if      (strcmp(key, "width")          == 0) s->engine.width          = (uint32_t)atoi(val);
        else if (strcmp(key, "height")         == 0) s->engine.height         = (uint32_t)atoi(val);
        else if (strcmp(key, "vsync")          == 0) s->engine.vsync          = atoi(val) != 0;
        else if (strcmp(key, "fps_limit")      == 0) s->engine.fps_limit      = (float)atof(val);
        else if (strcmp(key, "bloom")          == 0) s->engine.bloom          = atoi(val) != 0;
        else if (strcmp(key, "bloom_threshold")== 0) s->engine.bloom_threshold= (float)atof(val);
        else if (strcmp(key, "bloom_strength") == 0) s->engine.bloom_strength = (float)atof(val);
        else if (strcmp(key, "bloom_exposure")    == 0) s->engine.bloom_exposure    = (float)atof(val);
        else if (strcmp(key, "fov")               == 0) s->engine.fov               = (float)atof(val);
        else if (strcmp(key, "mouse_sensitivity") == 0) s->engine.mouse_sensitivity = (float)atof(val);
    } else if (*sec == SEC_BINDINGS) {
        settings_set_key(s, key, key_name_to_code(val));
    }
}

bool settings_load(settings_t *s, const char *path) {
    char *buf = fs_read_file(path, NULL);
    if (!buf) return false;

    section_t sec = SEC_NONE;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        char tmp[256];
        if (nl) {
            size_t len = (size_t)(nl - line);
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, line, len);
            tmp[len] = '\0';
            line = nl + 1;
        } else {
            strncpy(tmp, line, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            line += strlen(line);
        }
        parse_line(s, &sec, tmp);
    }

    free(buf);
    return true;
}

/* ---- save -------------------------------------------------------------- */

static bool write_fn(FILE *f, void *ctx) {
    const settings_t *s = ctx;

    fprintf(f, "[engine]\n");
    fprintf(f, "width=%u\n",          s->engine.width);
    fprintf(f, "height=%u\n",         s->engine.height);
    fprintf(f, "vsync=%d\n",          s->engine.vsync ? 1 : 0);
    fprintf(f, "fps_limit=%g\n",      (double)s->engine.fps_limit);
    fprintf(f, "bloom=%d\n",          s->engine.bloom ? 1 : 0);
    fprintf(f, "bloom_threshold=%g\n",(double)s->engine.bloom_threshold);
    fprintf(f, "bloom_strength=%g\n", (double)s->engine.bloom_strength);
    fprintf(f, "bloom_exposure=%g\n",    (double)s->engine.bloom_exposure);
    fprintf(f, "fov=%g\n",              (double)s->engine.fov);
    fprintf(f, "mouse_sensitivity=%g\n",(double)s->engine.mouse_sensitivity);

    if (s->bindings.count > 0) {
        fprintf(f, "\n[bindings]\n");
        for (int i = 0; i < s->bindings.count; i++) {
            const key_binding_t *b = &s->bindings.entries[i];
            fprintf(f, "%s=%s\n", b->action, key_code_to_name(b->key));
        }
    }

    return ferror(f) == 0;
}

bool settings_save(const settings_t *s, const char *path) {
    return fs_write_atomic(path, write_fn, (void *)s);
}
