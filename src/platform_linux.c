// platform_linux.c — X11 + EGL dev implementation of platform.h.
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    Display*     dpy;
    Window       win;
    Atom         wm_delete;
    EGLDisplay   egl_dpy;
    EGLSurface   egl_surf;
    EGLContext   egl_ctx;
} LinuxNative;

static uint32_t keysym_to_bit(KeySym sym) {
    switch (sym) {
        case XK_Up:    case XK_w: case XK_W: return KEY_UP;
        case XK_Down:  case XK_s: case XK_S: return KEY_DOWN;
        case XK_Left:  case XK_a: case XK_A: return KEY_LEFT;
        case XK_Right: case XK_d: case XK_D: return KEY_RIGHT;
        case XK_j:     case XK_J: case XK_z: case XK_Z: return KEY_SHOOT;
        case XK_k:     case XK_K: case XK_x: case XK_X: return KEY_FOCUS;
        case XK_r:     case XK_R: return KEY_RESTART;
        case XK_F1:    return KEY_HUD;
        case XK_F2:    return KEY_REC;
        case XK_F3:    return KEY_PLAY;
        default: return 0;
    }
}

static uint32_t g_held_keys = 0;
static bool     g_quit = false;

bool plat_init(PlatformWindow* w, int desired_w, int desired_h) {
    LinuxNative* n = calloc(1, sizeof(LinuxNative));
    if (!n) return false;

    n->dpy = XOpenDisplay(NULL);
    if (!n->dpy) {
        fprintf(stderr, "plat_init: XOpenDisplay failed\n");
        free(n);
        return false;
    }

    int screen = DefaultScreen(n->dpy);
    Window root = RootWindow(n->dpy, screen);

    XSetWindowAttributes swa = {0};
    swa.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask;
    n->win = XCreateWindow(n->dpy, root, 0, 0, desired_w, desired_h, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWEventMask, &swa);
    XStoreName(n->dpy, n->win, "windowed-hell");
    n->wm_delete = XInternAtom(n->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(n->dpy, n->win, &n->wm_delete, 1);
    XMapWindow(n->dpy, n->win);

    n->egl_dpy = eglGetDisplay((EGLNativeDisplayType)n->dpy);
    if (n->egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "plat_init: eglGetDisplay failed\n");
        return false;
    }
    EGLint egl_maj, egl_min;
    if (!eglInitialize(n->egl_dpy, &egl_maj, &egl_min)) {
        fprintf(stderr, "plat_init: eglInitialize failed\n");
        return false;
    }

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint num_cfg;
    if (!eglChooseConfig(n->egl_dpy, cfg_attribs, &cfg, 1, &num_cfg) || num_cfg < 1) {
        fprintf(stderr, "plat_init: eglChooseConfig failed\n");
        return false;
    }

    n->egl_surf = eglCreateWindowSurface(n->egl_dpy, cfg, (EGLNativeWindowType)n->win, NULL);
    if (n->egl_surf == EGL_NO_SURFACE) {
        fprintf(stderr, "plat_init: eglCreateWindowSurface failed\n");
        return false;
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    n->egl_ctx = eglCreateContext(n->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (n->egl_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "plat_init: eglCreateContext failed\n");
        return false;
    }

    if (!eglMakeCurrent(n->egl_dpy, n->egl_surf, n->egl_surf, n->egl_ctx)) {
        fprintf(stderr, "plat_init: eglMakeCurrent failed\n");
        return false;
    }

    w->width  = desired_w;
    w->height = desired_h;
    w->native = n;
    return true;
}

void plat_shutdown(PlatformWindow* w) {
    LinuxNative* n = (LinuxNative*)w->native;
    if (!n) return;
    if (n->egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(n->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (n->egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(n->egl_dpy, n->egl_ctx);
        if (n->egl_surf != EGL_NO_SURFACE) eglDestroySurface(n->egl_dpy, n->egl_surf);
        eglTerminate(n->egl_dpy);
    }
    if (n->dpy) {
        XDestroyWindow(n->dpy, n->win);
        XCloseDisplay(n->dpy);
    }
    free(n);
    w->native = NULL;
}

void plat_poll(PlatformWindow* w, PlatformInput* out) {
    LinuxNative* n = (LinuxNative*)w->native;
    while (XPending(n->dpy) > 0) {
        XEvent ev;
        XNextEvent(n->dpy, &ev);
        switch (ev.type) {
            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape) { g_quit = true; break; }
                g_held_keys |= keysym_to_bit(sym);
                break;
            }
            case KeyRelease: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                g_held_keys &= ~keysym_to_bit(sym);
                break;
            }
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == n->wm_delete) g_quit = true;
                break;
            default: break;
        }
    }
    out->keys = g_held_keys;
    out->quit_requested = g_quit;
}

void plat_swap(PlatformWindow* w) {
    LinuxNative* n = (LinuxNative*)w->native;
    eglSwapBuffers(n->egl_dpy, n->egl_surf);
}

uint64_t plat_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t abs_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(abs_ns / 1000000000ull);
    ts.tv_nsec = (long)(abs_ns % 1000000000ull);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

void plat_set_playfield_rect(PlatformWindow* w, int x, int y, int wd, int ht) {
    (void)w; (void)x; (void)y; (void)wd; (void)ht;
    // no-op on Linux dev build; compositor mode is QNX-only (STRETCH S1).
}
