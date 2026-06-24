#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring buffer capacity (must be a power of two). */
#define EV_CAP  128
#define EV_MASK (EV_CAP - 1)

static const char *WCLASS = "KilnWindow";

struct window {
    HWND      hwnd;
    HINSTANCE hinstance;
    uint32_t  width, height;

    cursor_mode_t cursor_mode;
    bool          cursor_hidden; /* ShowCursor is a counter; gate on this flag */

    bool has_last;
    int  last_x, last_y;
    int  warp_count; /* number of SetCursorPos warps whose WM_MOUSEMOVE is still pending */
    int  restore_x, restore_y; /* screen coords saved on CURSOR_DISABLED enter */

    /* Lock-free single-producer / single-consumer ring buffer.
       WndProc pushes; window_poll_event pops. Both run on the same thread
       (Win32 dispatches to the same thread that called PeekMessage). */
    event_t evbuf[EV_CAP];
    int     ev_head; /* next slot to read  */
    int     ev_tail; /* next slot to write */
};

/* -------------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------------- */

static void push_event(struct window *w, event_t ev) {
    int next = (w->ev_tail + 1) & EV_MASK;
    if (next == w->ev_head) return; /* full — drop oldest */
    w->evbuf[w->ev_tail] = ev;
    w->ev_tail = next;
}

static keycode_t vk_to_code(WPARAM vk) {
    switch (vk) {
    case VK_ESCAPE: return KEY_ESCAPE;
    case VK_SPACE:  return KEY_SPACE;
    case VK_RETURN: return KEY_RETURN;
    case VK_TAB:    return KEY_TAB;
    case VK_BACK:   return KEY_BACKSPACE;
    case 'W': return KEY_W;
    case 'A': return KEY_A;
    case 'S': return KEY_S;
    case 'D': return KEY_D;
    case 'Q': return KEY_Q;
    case 'E': return KEY_E;
    case VK_LEFT:  return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_UP:    return KEY_UP;
    case VK_DOWN:  return KEY_DOWN;
    case '0': return KEY_0;
    case '1': return KEY_1;
    case '2': return KEY_2;
    case '3': return KEY_3;
    case '4': return KEY_4;
    case '5': return KEY_5;
    case '6': return KEY_6;
    case '7': return KEY_7;
    case '8': return KEY_8;
    case '9': return KEY_9;
    default:  return KEY_UNKNOWN;
    }
}

/* Normalise VK to a lowercase-ASCII keysym so keys[keysym & 0xFF] matches
   X11 convention (XK_w=0x77, XK_a=0x61, etc.). */
static unsigned long vk_to_keysym(WPARAM vk) {
    return (unsigned long)tolower((unsigned char)(UINT)vk);
}

static void center_cursor(struct window *w) {
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    int cx = (cr.right  - cr.left) / 2;
    int cy = (cr.bottom - cr.top)  / 2;
    POINT pt = { cx, cy };
    ClientToScreen(w->hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    w->warp_count++;
}

/* -------------------------------------------------------------------------
   WndProc
   ------------------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* During WM_NCCREATE the pointer isn't stored yet — set it now. */
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    struct window *w = (struct window *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcA(hwnd, msg, wp, lp);

    event_t ev = {0};

    switch (msg) {
    case WM_CLOSE:
        ev.type = EVENT_QUIT;
        push_event(w, ev);
        return 0; /* don't call DefWindowProc: that would DestroyWindow */

    case WM_SIZE:
        w->width  = (uint32_t)LOWORD(lp);
        w->height = (uint32_t)HIWORD(lp);
        if (w->width > 0 && w->height > 0) {
            ev.type          = EVENT_RESIZE;
            ev.resize.width  = w->width;
            ev.resize.height = w->height;
            push_event(w, ev);
            if (w->cursor_mode == CURSOR_DISABLED) {
                RECT cr;
                GetClientRect(w->hwnd, &cr);
                MapWindowPoints(w->hwnd, NULL, (LPPOINT)&cr, 2);
                ClipCursor(&cr);
            }
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        ev.type       = EVENT_KEY_DOWN;
        ev.key.code   = vk_to_code(wp);
        ev.key.keysym = vk_to_keysym(wp);
        ev.key.down   = true;
        push_event(w, ev);
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        ev.type       = EVENT_KEY_UP;
        ev.key.code   = vk_to_code(wp);
        ev.key.keysym = vk_to_keysym(wp);
        ev.key.down   = false;
        push_event(w, ev);
        break;

    case WM_CHAR:
        if (wp >= 32 && wp < 127) {
            ev.type   = EVENT_TEXT;
            ev.text.c = (char)(UINT)wp;
            push_event(w, ev);
        }
        break;

    case WM_MOUSEMOVE: {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);

        /* Swallow our own re-centring warps (one suppressed event per warp). */
        if (w->warp_count > 0) {
            w->warp_count--;
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

        ev.type      = EVENT_MOUSE_MOTION;
        ev.motion.x  = mx;
        ev.motion.y  = my;
        ev.motion.dx = dx;
        ev.motion.dy = dy;
        push_event(w, ev);

        if (w->cursor_mode == CURSOR_DISABLED) {
            int W = (int)w->width, H = (int)w->height;
            if (mx < W / 4 || mx > (W * 3) / 4 || my < H / 4 || my > (H * 3) / 4)
                center_cursor(w);
        }
        break;
    }

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        ev.type          = EVENT_MOUSE_BUTTON;
        ev.button.button = MOUSE_BUTTON_LEFT;
        ev.button.x      = (int)(short)LOWORD(lp);
        ev.button.y      = (int)(short)HIWORD(lp);
        ev.button.down   = (msg == WM_LBUTTONDOWN);
        push_event(w, ev);
        break;

    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        ev.type          = EVENT_MOUSE_BUTTON;
        ev.button.button = MOUSE_BUTTON_MIDDLE;
        ev.button.x      = (int)(short)LOWORD(lp);
        ev.button.y      = (int)(short)HIWORD(lp);
        ev.button.down   = (msg == WM_MBUTTONDOWN);
        push_event(w, ev);
        break;

    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        ev.type          = EVENT_MOUSE_BUTTON;
        ev.button.button = MOUSE_BUTTON_RIGHT;
        ev.button.x      = (int)(short)LOWORD(lp);
        ev.button.y      = (int)(short)HIWORD(lp);
        ev.button.down   = (msg == WM_RBUTTONDOWN);
        push_event(w, ev);
        break;

    case WM_MOUSEWHEEL:
        ev.type         = EVENT_SCROLL;
        ev.scroll.delta = ((short)HIWORD(wp) > 0) ? 1.0f : -1.0f;
        push_event(w, ev);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    return 0;
}

/* -------------------------------------------------------------------------
   Lifecycle
   ------------------------------------------------------------------------- */

window_t *window_create(const char *title, uint32_t width, uint32_t height) {
    struct window *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->hinstance = GetModuleHandleA(NULL);

    /* Borderless fullscreen at the primary monitor's resolution.
       Requested width/height are ignored on Win32 — we don't rely on the
       WM to maximise/tile us because Wine windows are not tiled by external
       WMs (Wine sets WM_NORMAL_HINTS min=max=created_size, preventing it). */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    w->width  = (uint32_t)sw;
    w->height = (uint32_t)sh;
    (void)width; (void)height;

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = w->hinstance;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = WCLASS;
    RegisterClassExA(&wc);

    w->hwnd = CreateWindowExA(
        WS_EX_APPWINDOW, WCLASS, title, WS_POPUP,
        0, 0, sw, sh,
        NULL, NULL, w->hinstance,
        w /* passed as lpCreateParams, retrieved in WM_NCCREATE */);

    if (!w->hwnd) { free(w); return NULL; }

    /* Show and activate first so the WM grants keyboard focus normally.
       Then promote to topmost without re-activating, so Wine sets
       _NET_WM_STATE_FULLSCREEN on the X11 window, signalling tiling WMs
       to float it at full screen. */
    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);
    SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* Explicitly take the foreground and keyboard focus. A borderless WS_POPUP
       promoted to TOPMOST without activation does not receive X11 input focus
       under Wine, so WM_KEYDOWN/WM_KEYUP never reach WndProc and keyboard input
       silently does nothing. SetForegroundWindow activates the window at map
       time (when tiling WMs most readily grant focus); SetActiveWindow +
       SetFocus direct keyboard input to it. */
    SetForegroundWindow(w->hwnd);
    SetActiveWindow(w->hwnd);
    SetFocus(w->hwnd);
    return w;
}

void window_destroy(window_t *w) {
    if (!w) return;
    if (w->hwnd) {
        if (w->cursor_mode == CURSOR_DISABLED) {
            ClipCursor(NULL);
            ReleaseCapture();
        }
        if (w->cursor_hidden) ShowCursor(TRUE);
        DestroyWindow(w->hwnd);
        UnregisterClassA(WCLASS, w->hinstance);
    }
    free(w);
}

/* -------------------------------------------------------------------------
   Event loop
   ------------------------------------------------------------------------- */

bool window_poll_event(window_t *w, event_t *out) {
    MSG msg;
    /* Pump Win32 messages until our queue has at least one event or the
       message queue is empty. Non-event messages (WM_PAINT etc.) are handled
       by DefWindowProc without producing an event_t. */
    while (w->ev_head == w->ev_tail) {
        if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) return false;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    *out = w->evbuf[w->ev_head];
    w->ev_head = (w->ev_head + 1) & EV_MASK;
    return true;
}

void window_wait_events(window_t *w) {
    if (w->ev_head != w->ev_tail) return; /* already have events */
    WaitMessage();
}

void window_size(const window_t *w, uint32_t *width, uint32_t *height) {
    if (width)  *width  = w->width;
    if (height) *height = w->height;
}

/* -------------------------------------------------------------------------
   Cursor / capture
   ------------------------------------------------------------------------- */

void window_set_cursor_mode(window_t *w, cursor_mode_t mode) {
    if (w->cursor_mode == mode) return;

    if (mode == CURSOR_DISABLED) {
        /* Save restore position in screen coords. */
        POINT pt;
        GetCursorPos(&pt);
        w->restore_x = pt.x;
        w->restore_y = pt.y;

        SetForegroundWindow(w->hwnd); /* request keyboard focus from the WM */
        SetActiveWindow(w->hwnd);
        SetFocus(w->hwnd);
        if (!w->cursor_hidden) { ShowCursor(FALSE); w->cursor_hidden = true; }
        SetCapture(w->hwnd);

        RECT cr;
        GetClientRect(w->hwnd, &cr);
        MapWindowPoints(w->hwnd, NULL, (LPPOINT)&cr, 2);
        ClipCursor(&cr);

        w->warp_count = 0;
        center_cursor(w);
        w->has_last = false;
    } else {
        w->warp_count = 0;
        ClipCursor(NULL);
        ReleaseCapture();
        if (w->cursor_hidden) { ShowCursor(TRUE); w->cursor_hidden = false; }
        SetCursorPos(w->restore_x, w->restore_y);
        w->has_last = false;
    }

    w->cursor_mode = mode;
}

cursor_mode_t window_cursor_mode(const window_t *w) { return w->cursor_mode; }

void window_hide_cursor(window_t *w, bool hidden) {
    if (hidden == w->cursor_hidden) return;
    if (hidden) ShowCursor(FALSE); else ShowCursor(TRUE);
    w->cursor_hidden = hidden;
}

void window_grab_mouse(window_t *w, bool grabbed) {
    if (grabbed) SetCapture(w->hwnd); else ReleaseCapture();
}

void window_warp_mouse(window_t *w, int x, int y) {
    POINT pt = { x, y };
    ClientToScreen(w->hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    w->warp_count++;
}

platform_native_handles_t window_get_native_handles(const window_t *w) {
    return (platform_native_handles_t){
        .display = (void *)w->hinstance,
        .window  = (unsigned long)(uintptr_t)w->hwnd,
    };
}

/* -------------------------------------------------------------------------
   Path resolution
   ------------------------------------------------------------------------- */

char *platform_resolve_path(const char *path) {
    /* Absolute paths (rooted drive letter or UNC) — pass through unchanged. */
    if (path && (path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':')))
        return strdup(path);

    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (len == 0) return strdup(path);

    /* Strip the filename — keep only the directory. */
    char *slash = strrchr(exe, '\\');
    if (!slash) slash = strrchr(exe, '/');
    if (!slash) return strdup(path);
    *slash = '\0';

    size_t n = strlen(exe) + 1 + strlen(path) + 1;
    char  *out = malloc(n);
    if (out) snprintf(out, n, "%s\\%s", exe, path);
    return out;
}
