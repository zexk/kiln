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

    /* pointer/cursor capture state */
    cursor_mode_t cursor_mode;
    Cursor blank_cursor; /* invisible cursor, created lazily */
    bool has_last;       /* last_x/last_y hold a valid previous position */
    int last_x;
    int last_y;
    bool warp_pending;   /* a warp we issued is yet to echo back as motion */
    int warp_x;          /* the position that pending warp lands on */
    int warp_y;
    int restore_x;       /* pointer position to restore when leaving DISABLED */
    int restore_y;
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

/* A 1x1 fully-transparent cursor — the dependency-free way to hide the pointer
   (core Xlib, no Xfixes). Created on first capture and reused. */
static Cursor blank_cursor(window_t *window) {
    if (window->blank_cursor) {
        return window->blank_cursor;
    }
    char zero[1] = {0};
    Pixmap pm = XCreateBitmapFromData(window->display, window->handle, zero, 1, 1);
    XColor black = {0};
    window->blank_cursor =
        XCreatePixmapCursor(window->display, pm, pm, &black, &black, 0, 0);
    XFreePixmap(window->display, pm);
    return window->blank_cursor;
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
        if (window->cursor_mode == CURSOR_DISABLED) {
            XUngrabPointer(window->display, CurrentTime);
        }
        if (window->blank_cursor) {
            XFreeCursor(window->display, window->blank_cursor);
        }
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
    case MotionNotify: {
        int mx = ev.xmotion.x;
        int my = ev.xmotion.y;

        /* Swallow the synthetic motion our own re-centring warp produces, so it
           never shows up as a spurious delta. */
        if (window->warp_pending && mx == window->warp_x &&
            my == window->warp_y) {
            window->warp_pending = false;
            window->last_x = mx;
            window->last_y = my;
            window->has_last = true;
            break; /* out->type stays EVENT_NONE */
        }

        int dx = 0, dy = 0;
        if (window->has_last) {
            dx = mx - window->last_x;
            dy = my - window->last_y;
        }
        window->last_x = mx;
        window->last_y = my;
        window->has_last = true;

        out->type = EVENT_MOUSE_MOVE;
        out->motion.x = mx;
        out->motion.y = my;
        out->motion.dx = dx;
        out->motion.dy = dy;

        /* When captured, keep the pointer away from the window edge (where a
           confined pointer would stall) by warping back to the centre. Deltas
           stay continuous because we measure them from last_x/last_y, not the
           centre. */
        if (window->cursor_mode == CURSOR_DISABLED) {
            int w = (int)window->width;
            int h = (int)window->height;
            if (mx < w / 4 || mx > (w * 3) / 4 || my < h / 4 ||
                my > (h * 3) / 4) {
                int cx = w / 2;
                int cy = h / 2;
                XWarpPointer(window->display, None, window->handle, 0, 0, 0, 0,
                             cx, cy);
                window->warp_pending = true;
                window->warp_x = cx;
                window->warp_y = cy;
            }
        }
        break;
    }
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

void window_set_cursor_mode(window_t *window, cursor_mode_t mode) {
    if (window->cursor_mode == mode) {
        return;
    }

    if (mode == CURSOR_DISABLED) {
        /* Remember where the visible cursor was so we can put it back later. */
        Window root, child;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (XQueryPointer(window->display, window->handle, &root, &child, &rx,
                          &ry, &wx, &wy, &mask)) {
            window->restore_x = wx;
            window->restore_y = wy;
        }

        Cursor c = blank_cursor(window);
        XDefineCursor(window->display, window->handle, c);
        XGrabPointer(window->display, window->handle, True,
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, window->handle, c,
                     CurrentTime);

        int cx = (int)window->width / 2;
        int cy = (int)window->height / 2;
        XWarpPointer(window->display, None, window->handle, 0, 0, 0, 0, cx, cy);
        window->warp_pending = true;
        window->warp_x = cx;
        window->warp_y = cy;
        window->has_last = false; /* delta baseline re-established at centre */
    } else {
        XUngrabPointer(window->display, CurrentTime);
        XUndefineCursor(window->display, window->handle);
        XWarpPointer(window->display, None, window->handle, 0, 0, 0, 0,
                     window->restore_x, window->restore_y);
        window->warp_pending = true;
        window->warp_x = window->restore_x;
        window->warp_y = window->restore_y;
        window->has_last = false;
    }

    window->cursor_mode = mode;
    XFlush(window->display);
}

cursor_mode_t window_cursor_mode(const window_t *window) {
    return window->cursor_mode;
}

void *window_x11_display(const window_t *window) {
    return window->display;
}

unsigned long window_x11_window(const window_t *window) {
    return window->handle;
}
