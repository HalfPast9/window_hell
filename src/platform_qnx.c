// platform_qnx.c — QNX Screen + EGL implementation of platform.h.
// Follows QNX Screen Developer's Guide patterns (PRD §5.4). Compiles clean
// against SDP 8.0 headers for aarch64le and x86_64; not yet run on target.
// TODO(bringup): SCREEN_PROPERTY_SIZE is left at its default — PRD §5.4 step 3
// wants it set to the display's native mode so Screen hardware-scales the
// fixed 1280x720 buffer. Needs a real display to verify; do it in M3.
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/keycodes.h>

typedef struct {
    screen_context_t    ctx;
    screen_window_t     win;
    screen_event_t      ev;     // reused across polls; Screen requires it be created
    EGLDisplay           egl_dpy;
    EGLSurface           egl_surf;
    EGLContext           egl_ctx;
} QnxNative;

static uint32_t g_held_keys = 0;
static bool     g_quit = false;
static float    g_mouse_x = 0.0f, g_mouse_y = 0.0f;
static bool     g_mouse_down = false;

static uint32_t sym_to_bit(int sym) {
    switch (sym) {
        case KEYCODE_UP:    case 'w': case 'W': return KEY_UP;
        case KEYCODE_DOWN:  case 's': case 'S': return KEY_DOWN;
        case KEYCODE_LEFT:  case 'a': case 'A': return KEY_LEFT;
        case KEYCODE_RIGHT: case 'd': case 'D': return KEY_RIGHT;
        case 'j': case 'J': case 'z': case 'Z': return KEY_SHOOT;
        case 'k': case 'K': case 'x': case 'X': return KEY_FOCUS;
        case 'r': case 'R': return KEY_RESTART;
        case KEYCODE_F1: return KEY_HUD;
        case KEYCODE_F2: return KEY_REC;
        case KEYCODE_F3: return KEY_PLAY;
        default: return 0;
    }
}

bool plat_init(PlatformWindow* w, int desired_w, int desired_h) {
    QnxNative* n = calloc(1, sizeof(QnxNative));
    if (!n) return false;

    if (screen_create_context(&n->ctx, SCREEN_APPLICATION_CONTEXT) != 0) {
        fprintf(stderr, "plat_init: screen_create_context failed\n");
        free(n);
        return false;
    }

    if (screen_create_event(&n->ev) != 0) {
        fprintf(stderr, "plat_init: screen_create_event failed\n");
        return false;
    }

    if (screen_create_window(&n->win, n->ctx) != 0) {
        fprintf(stderr, "plat_init: screen_create_window failed\n");
        return false;
    }

    int usage = SCREEN_USAGE_OPENGL_ES2;
    screen_set_window_property_iv(n->win, SCREEN_PROPERTY_USAGE, &usage);

    int format = SCREEN_FORMAT_RGBX8888;
    screen_set_window_property_iv(n->win, SCREEN_PROPERTY_FORMAT, &format);

    // D4: fixed internal buffer size (1280x720); Screen hardware-scales to display.
    int buffer_size[2] = { desired_w, desired_h };
    screen_set_window_property_iv(n->win, SCREEN_PROPERTY_BUFFER_SIZE, buffer_size);

    if (screen_create_window_buffers(n->win, 2) != 0) {
        fprintf(stderr, "plat_init: screen_create_window_buffers failed\n");
        return false;
    }

    n->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
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

    // Seed the cursor at center so aim is well-defined before the first
    // pointer event arrives (and stays sane if none ever does — see the
    // unverified-pointer note in plat_poll).
    g_mouse_x = (float)desired_w * 0.5f;
    g_mouse_y = (float)desired_h * 0.5f;

    w->width  = desired_w;
    w->height = desired_h;
    w->native = n;
    return true;
}

void plat_shutdown(PlatformWindow* w) {
    QnxNative* n = (QnxNative*)w->native;
    if (!n) return;
    if (n->egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(n->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (n->egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(n->egl_dpy, n->egl_ctx);
        if (n->egl_surf != EGL_NO_SURFACE) eglDestroySurface(n->egl_dpy, n->egl_surf);
        eglTerminate(n->egl_dpy);
    }
    if (n->win) screen_destroy_window(n->win);
    if (n->ev)  screen_destroy_event(n->ev);
    if (n->ctx) screen_destroy_context(n->ctx);
    free(n);
    w->native = NULL;
}

void plat_poll(PlatformWindow* w, PlatformInput* out) {
    QnxNative* n = (QnxNative*)w->native;
    for (;;) {
        if (screen_get_event(n->ctx, n->ev, 0) != 0) break; // 0 timeout: non-blocking

        int type = SCREEN_EVENT_NONE;
        screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_TYPE, &type);
        if (type == SCREEN_EVENT_NONE) break;

        if (type == SCREEN_EVENT_KEYBOARD) {
            int sym = 0, flags = 0;
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_SYM, &sym);
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_FLAGS, &flags);
            uint32_t bit = sym_to_bit(sym);
            if (getenv("WH_KEYDEBUG") && (flags & SCREEN_FLAG_KEY_DOWN))
                fprintf(stderr, "KEYDBG sym=0x%x (%d) '%c' flags=0x%x bit=%u\n",
                        sym, sym, (sym >= 32 && sym < 127) ? sym : '?', flags, bit);
            if (flags & SCREEN_FLAG_KEY_DOWN) g_held_keys |= bit;
            else g_held_keys &= ~bit;
            if (sym == KEYCODE_ESCAPE && (flags & SCREEN_FLAG_KEY_DOWN)) g_quit = true;
        } else if (type == SCREEN_EVENT_POINTER) {
            // TODO(bringup): never exercised on target — no QNX mouse precedent
            // in this codebase. Symbols verified against SDP 8.0 screen.h, but
            // behavior is unverified. KEY_SHOOT (J/Z) stays wired as the
            // fallback path precisely because of this. Test with a USB mouse
            // during the M3 first-hour spike, alongside the keyboard check
            // (PRD §5.4 step 7).
            int pos[2] = { 0, 0 };
            int buttons = 0;
            // For pointer events SCREEN_PROPERTY_POSITION is the window-relative
            // contact point; SOURCE_POSITION would be display-absolute.
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_POSITION, pos);
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_BUTTONS, &buttons);
            // Window buffer is fixed at the internal resolution (D4), so
            // window-relative coords are already internal-space.
            g_mouse_x = (float)pos[0];
            g_mouse_y = (float)pos[1];
            g_mouse_down = (buttons & SCREEN_LEFT_MOUSE_BUTTON) != 0;
        }
    }
    out->keys = g_held_keys;
    out->mouse_x = g_mouse_x;
    out->mouse_y = g_mouse_y;
    out->mouse_down = g_mouse_down;
    out->quit_requested = g_quit;
}

void plat_swap(PlatformWindow* w) {
    QnxNative* n = (QnxNative*)w->native;
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
    // STRETCH S1: second background window + SCREEN_PROPERTY_POSITION/SIZE
    // mapping. Not implemented until M6 is frozen (PRD §11).
    (void)w; (void)x; (void)y; (void)wd; (void)ht;
}
