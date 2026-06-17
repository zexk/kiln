#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdlib.h>
#include <string.h>

struct window {
    Display *display;
    Window handle;
    Atom wm_delete_window;
    uint32_t width;
    uint32_t height;
};

static keycode_t keysym_to_code(KeySym keysym) {
    switch (keysym) {
    case XK_Escape: return KEY_ESCAPE;
    case XK_space:  return KEY_SPACE;
    case XK_Return: return KEY_RETURN;
    case XK_w: case XK_W: return KEY_W;
    case XK_a: case XK_A: return KEY_A;
    case XK_s: case XK_S: return KEY_S;
    case XK_d: case XK_D: return KEY_D;
    case XK_Left:  return KEY_LEFT;
    case XK_Right: return KEY_RIGHT;
    case XK_Up:    return KEY_UP;
    case XK_Down:  return KEY_DOWN;
    default:       return KEY_UNKNOWN;
    }
}

static mouse_button_t x11_button_to_code(unsigned int button) {
    switch (button) {
    case Button1: return MOUSE_BUTTON_LEFT;
    case Button2: return MOUSE_BUTTON_MIDDLE;
    case Button3: return MOUSE_BUTTON_RIGHT;
    default:      return MOUSE_BUTTON_NONE;
    }
}

window_t *window_create(const char *title, uint32_t width, uint32_t height) {
    window_t *window = calloc(1, sizeof(*window));
    if (!window) {
        return NULL;
    }

    window->display = XOpenDisplay(NULL);
    if (!window->display) {
        free(window);
        return NULL;
    }

    window->width = width;
    window->height = height;

    int screen = DefaultScreen(window->display);
    Window root = RootWindow(window->display, screen);

    XSetWindowAttributes attrs;
    attrs.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    window->handle = XCreateWindow(
        window->display, root,
        0, 0, width, height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask, &attrs);

    XStoreName(window->display, window->handle, title);

    /* Route the WM close button through the event queue instead of
     * killing the connection. */
    window->wm_delete_window =
        XInternAtom(window->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(window->display, window->handle,
                    &window->wm_delete_window, 1);

    XMapWindow(window->display, window->handle);
    XFlush(window->display);

    return window;
}

void window_destroy(window_t *window) {
    if (!window) {
        return;
    }
    if (window->display) {
        XDestroyWindow(window->display, window->handle);
        XCloseDisplay(window->display);
    }
    free(window);
}

bool window_poll_event(window_t *window, event_t *out) {
    if (!XPending(window->display)) {
        return false;
    }

    XEvent ev;
    XNextEvent(window->display, &ev);

    memset(out, 0, sizeof(*out));
    out->type = EVENT_NONE;

    switch (ev.type) {
    case KeyPress:
        out->type = EVENT_KEY_DOWN;
        out->key.code = keysym_to_code(XLookupKeysym(&ev.xkey, 0));
        break;
    case KeyRelease:
        out->type = EVENT_KEY_UP;
        out->key.code = keysym_to_code(XLookupKeysym(&ev.xkey, 0));
        break;
    case MotionNotify:
        out->type = EVENT_MOUSE_MOVE;
        out->motion.x = ev.xmotion.x;
        out->motion.y = ev.xmotion.y;
        break;
    case ButtonPress:
    case ButtonRelease:
        /* X11 buttons: 1 left, 2 middle, 3 right, 4/5 wheel up/down. */
        switch (ev.xbutton.button) {
        case Button4:
        case Button5:
            if (ev.type == ButtonPress) { /* wheel only reports on press */
                out->type = EVENT_SCROLL;
                out->scroll.delta = (ev.xbutton.button == Button4) ? 1.0f
                                                                   : -1.0f;
            }
            break;
        default:
            out->type =
                (ev.type == ButtonPress) ? EVENT_BUTTON_DOWN : EVENT_BUTTON_UP;
            out->button.button = x11_button_to_code(ev.xbutton.button);
            out->button.x = ev.xbutton.x;
            out->button.y = ev.xbutton.y;
            break;
        }
        break;
    case ConfigureNotify:
        if ((uint32_t)ev.xconfigure.width != window->width ||
            (uint32_t)ev.xconfigure.height != window->height) {
            window->width = (uint32_t)ev.xconfigure.width;
            window->height = (uint32_t)ev.xconfigure.height;
            out->type = EVENT_RESIZE;
            out->resize.width = window->width;
            out->resize.height = window->height;
        }
        break;
    case ClientMessage:
        if ((Atom)ev.xclient.data.l[0] == window->wm_delete_window) {
            out->type = EVENT_QUIT;
        }
        break;
    default:
        break;
    }

    return true;
}

void window_wait_events(window_t *window) {
    /* Blocks until an event is queued, leaving it for window_poll_event. */
    XEvent ev;
    XPeekEvent(window->display, &ev);
}

void window_size(const window_t *window, uint32_t *width, uint32_t *height) {
    if (width) {
        *width = window->width;
    }
    if (height) {
        *height = window->height;
    }
}

void *window_x11_display(const window_t *window) {
    return window->display;
}

unsigned long window_x11_window(const window_t *window) {
    return window->handle;
}
