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
    KEY_W,
    KEY_A,
    KEY_S,
    KEY_D,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_COUNT,
} keycode_t;

typedef enum {
    MOUSE_BUTTON_NONE = 0,
    MOUSE_BUTTON_LEFT,
    MOUSE_BUTTON_MIDDLE,
    MOUSE_BUTTON_RIGHT,
} mouse_button_t;

typedef enum {
    EVENT_NONE = 0,
    EVENT_QUIT,     /* WM close request (window manager "X" button). */
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_RESIZE,
    EVENT_MOUSE_MOVE,   /* pointer moved; carries absolute window coords. */
    EVENT_BUTTON_DOWN,
    EVENT_BUTTON_UP,
    EVENT_SCROLL,       /* wheel; delta is +1 (up/away) or -1 (down/toward). */
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct {
            keycode_t code;
        } key;
        struct {
            uint32_t width;
            uint32_t height;
        } resize;
        struct {
            int32_t x; /* absolute, window top-left origin, +y down */
            int32_t y;
        } motion;
        struct {
            mouse_button_t button;
            int32_t x;
            int32_t y;
        } button;
        struct {
            float delta;
        } scroll;
    };
} event_t;

/* Create a mapped, visible window. Returns NULL on failure. */
window_t *window_create(const char *title, uint32_t width, uint32_t height);
void window_destroy(window_t *window);

/* Drain one queued event. Returns false when the queue is empty.
 * Drives the game loop: poll-drain each frame, never blocks. */
bool window_poll_event(window_t *window, event_t *out);

/* Block until at least one event is queued. Pair with window_poll_event
 * to idle without spinning when there is nothing to render. */
void window_wait_events(window_t *window);

void window_size(const window_t *window, uint32_t *width, uint32_t *height);

/* Native X11 handles for VkXlibSurfaceKHR creation. Returned as void* and
   unsigned long so callers need not include Xlib; the renderer casts the
   display back to Display* and the window to an XID. */
void *window_x11_display(const window_t *window);
unsigned long window_x11_window(const window_t *window);
