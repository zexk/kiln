#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

struct window {
    Display *display;
    Window   handle;
    Atom     wm_delete_window;
    uint32_t width;
    uint32_t height;

    cursor_mode_t cursor_mode;
    Cursor blank_cursor;
    bool   has_last;
    int    last_x;
    int    last_y;
    bool   warp_pending;
    int    warp_x;
    int    warp_y;
    int    restore_x;
    int    restore_y;

    /* one-slot lookahead for EVENT_TEXT generated alongside KEY_DOWN */
    bool    has_pending;
    event_t pending;
};

static keycode_t keysym_to_code(KeySym ks) {
    switch (ks) {
    case XK_Escape:    return KEY_ESCAPE;
    case XK_space:     return KEY_SPACE;
    case XK_Return:    return KEY_RETURN;
    case XK_Tab:       return KEY_TAB;
    case XK_BackSpace: return KEY_BACKSPACE;
    case XK_w: case XK_W: return KEY_W;
    case XK_a: case XK_A: return KEY_A;
    case XK_s: case XK_S: return KEY_S;
    case XK_d: case XK_D: return KEY_D;
    case XK_q: case XK_Q: return KEY_Q;
    case XK_e: case XK_E: return KEY_E;
    case XK_Left:  return KEY_LEFT;
    case XK_Right: return KEY_RIGHT;
    case XK_Up:    return KEY_UP;
    case XK_Down:  return KEY_DOWN;
    case XK_0: return KEY_0;
    case XK_1: return KEY_1;
    case XK_2: return KEY_2;
    case XK_3: return KEY_3;
    case XK_4: return KEY_4;
    case XK_5: return KEY_5;
    case XK_6: return KEY_6;
    case XK_7: return KEY_7;
    case XK_8: return KEY_8;
    case XK_9: return KEY_9;
    default:   return KEY_UNKNOWN;
    }
}

static mouse_button_t x11_button(unsigned int b) {
    switch (b) {
    case Button1: return MOUSE_BUTTON_LEFT;
    case Button2: return MOUSE_BUTTON_MIDDLE;
    case Button3: return MOUSE_BUTTON_RIGHT;
    default:      return MOUSE_BUTTON_NONE;
    }
}

static Cursor make_blank_cursor(window_t *w) {
    if (w->blank_cursor) return w->blank_cursor;
    char zero[1] = {0};
    Pixmap pm = XCreateBitmapFromData(w->display, w->handle, zero, 1, 1);
    XColor black = {0};
    w->blank_cursor = XCreatePixmapCursor(w->display, pm, pm, &black, &black, 0, 0);
    XFreePixmap(w->display, pm);
    return w->blank_cursor;
}

window_t *window_create(const char *title, uint32_t width, uint32_t height) {
    window_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->display = XOpenDisplay(NULL);
    if (!w->display) { free(w); return NULL; }

    w->width  = width;
    w->height = height;

    int    screen = DefaultScreen(w->display);
    Window root   = RootWindow(w->display, screen);

    XSetWindowAttributes attrs = {0};
    attrs.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    w->handle = XCreateWindow(w->display, root, 0, 0, width, height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWEventMask, &attrs);

    XStoreName(w->display, w->handle, title);

    w->wm_delete_window = XInternAtom(w->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->display, w->handle, &w->wm_delete_window, 1);

    XMapWindow(w->display, w->handle);
    XFlush(w->display);
    return w;
}

void window_destroy(window_t *w) {
    if (!w) return;
    if (w->display) {
        if (w->cursor_mode == CURSOR_DISABLED) XUngrabPointer(w->display, CurrentTime);
        if (w->blank_cursor) XFreeCursor(w->display, w->blank_cursor);
        XDestroyWindow(w->display, w->handle);
        XCloseDisplay(w->display);
    }
    free(w);
}

bool window_poll_event(window_t *w, event_t *out) {
    /* drain any event we queued internally (e.g. TEXT after KEY_DOWN) */
    if (w->has_pending) {
        *out = w->pending;
        w->has_pending = false;
        return true;
    }

    if (!XPending(w->display)) return false;

    XEvent ev;
    XNextEvent(w->display, &ev);
    memset(out, 0, sizeof(*out));

    switch (ev.type) {
    case KeyPress: {
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        out->type       = EVENT_KEY_DOWN;
        out->key.code   = keysym_to_code(ks);
        out->key.keysym = ks;
        out->key.down   = true;

        /* queue a TEXT event for printable characters */
        char buf[8];
        int  n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, NULL, NULL);
        if (n == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
            w->pending.type   = EVENT_TEXT;
            w->pending.text.c = buf[0];
            w->has_pending    = true;
        }
        break;
    }
    case KeyRelease: {
        KeySym ks       = XLookupKeysym(&ev.xkey, 0);
        out->type       = EVENT_KEY_UP;
        out->key.code   = keysym_to_code(ks);
        out->key.keysym = ks;
        out->key.down   = false;
        break;
    }
    case MotionNotify: {
        int mx = ev.xmotion.x;
        int my = ev.xmotion.y;

        if (w->warp_pending && mx == w->warp_x && my == w->warp_y) {
            w->warp_pending = false;
            w->last_x = mx;
            w->last_y = my;
            w->has_last = true;
            break;
        }

        int dx = 0, dy = 0;
        if (w->has_last) { dx = mx - w->last_x; dy = my - w->last_y; }
        w->last_x = mx;
        w->last_y = my;
        w->has_last = true;

        out->type      = EVENT_MOUSE_MOTION;
        out->motion.x  = mx;
        out->motion.y  = my;
        out->motion.dx = dx;
        out->motion.dy = dy;

        if (w->cursor_mode == CURSOR_DISABLED) {
            int W = (int)w->width, H = (int)w->height;
            if (mx < W / 4 || mx > (W * 3) / 4 || my < H / 4 || my > (H * 3) / 4) {
                int cx = W / 2, cy = H / 2;
                XWarpPointer(w->display, None, w->handle, 0, 0, 0, 0, cx, cy);
                w->warp_pending = true;
                w->warp_x = cx;
                w->warp_y = cy;
            }
        }
        break;
    }
    case ButtonPress:
    case ButtonRelease:
        switch (ev.xbutton.button) {
        case Button4:
        case Button5:
            if (ev.type == ButtonPress) {
                out->type         = EVENT_SCROLL;
                out->scroll.delta = (ev.xbutton.button == Button4) ? 1.0f : -1.0f;
            }
            break;
        default:
            out->type          = EVENT_MOUSE_BUTTON;
            out->button.button = x11_button(ev.xbutton.button);
            out->button.x      = ev.xbutton.x;
            out->button.y      = ev.xbutton.y;
            out->button.down   = (ev.type == ButtonPress);
            break;
        }
        break;
    case ConfigureNotify:
        if ((uint32_t)ev.xconfigure.width  != w->width ||
            (uint32_t)ev.xconfigure.height != w->height) {
            w->width  = (uint32_t)ev.xconfigure.width;
            w->height = (uint32_t)ev.xconfigure.height;
            out->type          = EVENT_RESIZE;
            out->resize.width  = w->width;
            out->resize.height = w->height;
        }
        break;
    case ClientMessage:
        if ((Atom)ev.xclient.data.l[0] == w->wm_delete_window)
            out->type = EVENT_QUIT;
        break;
    default:
        break;
    }

    return true;
}

void window_wait_events(window_t *w) {
    XEvent ev;
    XPeekEvent(w->display, &ev);
}

void window_size(const window_t *w, uint32_t *width, uint32_t *height) {
    if (width)  *width  = w->width;
    if (height) *height = w->height;
}

void window_set_cursor_mode(window_t *w, cursor_mode_t mode) {
    if (w->cursor_mode == mode) return;

    if (mode == CURSOR_DISABLED) {
        Window root, child;
        int rx, ry, wx, wy; unsigned int mask;
        if (XQueryPointer(w->display, w->handle, &root, &child,
                          &rx, &ry, &wx, &wy, &mask)) {
            w->restore_x = wx;
            w->restore_y = wy;
        }
        Cursor c = make_blank_cursor(w);
        XDefineCursor(w->display, w->handle, c);
        XGrabPointer(w->display, w->handle, True,
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, w->handle, c, CurrentTime);
        int cx = (int)w->width / 2, cy = (int)w->height / 2;
        XWarpPointer(w->display, None, w->handle, 0, 0, 0, 0, cx, cy);
        w->warp_pending = true;
        w->warp_x = cx;
        w->warp_y = cy;
        w->has_last = false;
    } else {
        XUngrabPointer(w->display, CurrentTime);
        XUndefineCursor(w->display, w->handle);
        XWarpPointer(w->display, None, w->handle, 0, 0, 0, 0,
                     w->restore_x, w->restore_y);
        w->warp_pending = true;
        w->warp_x = w->restore_x;
        w->warp_y = w->restore_y;
        w->has_last = false;
    }

    w->cursor_mode = mode;
    XFlush(w->display);
}

cursor_mode_t window_cursor_mode(const window_t *w) { return w->cursor_mode; }

void window_hide_cursor(window_t *w, bool hidden) {
    if (hidden)
        XDefineCursor(w->display, w->handle, make_blank_cursor(w));
    else
        XUndefineCursor(w->display, w->handle);
    XFlush(w->display);
}

void window_grab_mouse(window_t *w, bool grabbed) {
    if (grabbed)
        XGrabPointer(w->display, w->handle, True,
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, w->handle, None, CurrentTime);
    else
        XUngrabPointer(w->display, CurrentTime);
    XFlush(w->display);
}

void window_warp_mouse(window_t *w, int x, int y) {
    XWarpPointer(w->display, None, w->handle, 0, 0, 0, 0, x, y);
    w->warp_pending = true;
    w->warp_x = x;
    w->warp_y = y;
    XFlush(w->display);
}

platform_native_handles_t window_get_native_handles(const window_t *w) {
    return (platform_native_handles_t){ .display = w->display,
                                        .window  = (unsigned long)w->handle };
}

char *platform_resolve_path(const char *path) {
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return strdup(path);
    exe[len] = '\0';

    char *slash = strrchr(exe, '/');
    if (!slash) return strdup(path);
    *slash = '\0';

    size_t n = (size_t)(slash - exe) + 1 + strlen(path) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s/%s", exe, path);
    return out;
}
