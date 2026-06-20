#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Opaque OS window + input handle. One per top-level surface. */
typedef struct window window_t;

typedef enum {
    KEY_UNKNOWN = 0,
    KEY_ESCAPE,
    KEY_SPACE,
    KEY_RETURN,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_W, KEY_A, KEY_S, KEY_D,
    KEY_Q, KEY_E,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
    KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_COUNT,
} keycode_t;

typedef enum {
    MOUSE_BUTTON_NONE = 0,
    MOUSE_BUTTON_LEFT,
    MOUSE_BUTTON_MIDDLE,
    MOUSE_BUTTON_RIGHT,
} mouse_button_t;

typedef enum {
    CURSOR_NORMAL = 0,
    CURSOR_DISABLED,
} cursor_mode_t;

typedef enum {
    EVENT_NONE = 0,
    EVENT_QUIT,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_MOUSE_BUTTON,  /* replaces EVENT_BUTTON_DOWN / EVENT_BUTTON_UP */
    EVENT_MOUSE_MOTION,  /* replaces EVENT_MOUSE_MOVE  */
    EVENT_SCROLL,
    EVENT_TEXT,
    EVENT_RESIZE,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct {
            keycode_t      code;
            unsigned long  keysym; /* raw platform keysym for input.c key lookup */
            bool           down;
        } key;
        struct {
            mouse_button_t button;
            int32_t        x;
            int32_t        y;
            bool           down;
        } button;
        struct {
            int32_t x;
            int32_t y;
            int32_t dx;
            int32_t dy;
        } motion;
        struct {
            float delta;
        } scroll;
        struct {
            char c;
        } text;
        struct {
            uint32_t width;
            uint32_t height;
        } resize;
    };
} event_t;

/* Native window-system handles for Vulkan surface creation.
   Callers that need X11 types cast display to Display* and window to XID;
   Win32 callers cast display to HINSTANCE and window to HWND. */
typedef struct platform_native_handles {
    void         *display;
    unsigned long window;
} platform_native_handles_t;

/* Create a mapped, visible window. Returns NULL on failure. */
window_t *window_create(const char *title, uint32_t width, uint32_t height);
void      window_destroy(window_t *window);

bool window_poll_event(window_t *window, event_t *out);
void window_wait_events(window_t *window);
void window_size(const window_t *window, uint32_t *width, uint32_t *height);

void          window_set_cursor_mode(window_t *window, cursor_mode_t mode);
cursor_mode_t window_cursor_mode(const window_t *window);

/* Fine-grained cursor/pointer controls (alternative to cursor_mode for
   callers that need independent visibility and grab state). */
void window_hide_cursor(window_t *window, bool hidden);
void window_grab_mouse(window_t *window, bool grabbed);
void window_warp_mouse(window_t *window, int x, int y);

/* Generic handle accessor that works without including Xlib or windows.h. */
platform_native_handles_t window_get_native_handles(const window_t *window);

/* Return a heap-allocated absolute path to `path` resolved relative to the
   directory that contains the running executable. Caller must free(). */
char *platform_resolve_path(const char *path);
