// platform_qnx.c — QNX Screen + EGL implementation of platform.h.
// Follows QNX Screen Developer's Guide patterns (PRD §5.4). Verified on
// target (RPi5, QNX 8.0 quickstart desktop image) as of M3.
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/keycodes.h>
// sys/keycodes.h #defines KEY_DOWN as 0x00000001 ("key was pressed", a flag
// for its own event-flags bitfield) — an unrelated meaning that collides
// with platform.h's KEY_DOWN enum constant (our "move down" input bit,
// value 2). Being a macro, it silently rewrites every later `KEY_DOWN` in
// this file to 1 (== KEY_UP's value), which is why S/Down-arrow moved the
// player up instead of down. undef it here to restore the enum constant.
#undef KEY_DOWN

typedef struct {
    screen_context_t    ctx;
    screen_window_t     win;
    screen_event_t      ev;     // reused across polls; Screen requires it be created
    EGLDisplay           egl_dpy;
    EGLSurface           egl_surf;
    EGLContext           egl_ctx;
    int                  buffer_w, buffer_h;   // fixed internal resolution (D4)
    int                  onscreen_w, onscreen_h; // window's actual on-screen size (display-native)
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
    n->buffer_w = desired_w;
    n->buffer_h = desired_h;

    // PRD §5.4 step 3: size the window to the display's native mode so Screen
    // hardware-scales the fixed 1280x720 buffer up to fill it. M3 finding
    // (RPi5 hardware): the window already renders fullscreen by Screen's own
    // default (visually confirmed on target) — but SCREEN_EVENT_POINTER still
    // reports positions in on-screen/display pixels (1920x1080), not the
    // internal 1280x720 buffer space sim.c expects. Left unhandled, raw coords
    // ran up to (1725,838), well past internal bounds, corrupting the aim
    // vector (mouse - player). Explicitly setting position=(0,0) and
    // size=display-native makes that scale factor well-defined regardless of
    // whatever Screen's default placement actually is (window covers the
    // whole display at the origin either way, so no separate offset term is
    // needed) — belt-and-suspenders with the observed default, not a fix to
    // a visible placement bug.
    screen_display_t disp = NULL;
    if (screen_get_window_property_pv(n->win, SCREEN_PROPERTY_DISPLAY, (void**)&disp) == 0 && disp) {
        int display_size[2] = { desired_w, desired_h };
        if (screen_get_display_property_iv(disp, SCREEN_PROPERTY_SIZE, display_size) == 0) {
            int win_pos[2] = { 0, 0 };
            screen_set_window_property_iv(n->win, SCREEN_PROPERTY_POSITION, win_pos);
            screen_set_window_property_iv(n->win, SCREEN_PROPERTY_SIZE, display_size);
            n->onscreen_w = display_size[0];
            n->onscreen_h = display_size[1];
        }
    }
    if (n->onscreen_w <= 0 || n->onscreen_h <= 0) {
        // No display/property support (or query failed): fall back to
        // window size == buffer size, 1:1 scaling, matches old behavior.
        n->onscreen_w = desired_w;
        n->onscreen_h = desired_h;
    }

    if (screen_create_window_buffers(n->win, 2) != 0) {
        fprintf(stderr, "plat_init: screen_create_window_buffers failed\n");
        return false;
    }

    if (getenv("WH_KEYDEBUG")) {
        int win_size[2] = { 0, 0 };
        screen_get_window_property_iv(n->win, SCREEN_PROPERTY_SIZE, win_size);
        fprintf(stderr, "plat_init: buffer_size=%dx%d onscreen(requested)=%dx%d "
                        "window_size(SCREEN_PROPERTY_SIZE, actual)=%dx%d\n",
                buffer_size[0], buffer_size[1], n->onscreen_w, n->onscreen_h,
                win_size[0], win_size[1]);
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
            // OS auto-repeat while a key is held arrives as separate
            // SCREEN_EVENT_KEYBOARD pulses flagged SCREEN_FLAG_KEY_REPEAT —
            // the QNX Screen counterpart to X11's synthetic repeat
            // release/press pairs (platform_linux.c's XkbSetDetectableAutoRepeat
            // fix). g_held_keys already has the bit set from the real press;
            // reprocessing repeat pulses is a no-op for held-key gameplay
            // movement but a real bug for edge-triggered UI navigation
            // (MODE_SELECT/COLOR_SELECT/UPGRADE cursors, sim.c's key_edge()):
            // it made cursor movement fire once per repeat pulse instead of
            // once per press, reading as "scrolls through on its own" while
            // held. Skip repeat pulses outright — genuine press/release only.
            if (flags & SCREEN_FLAG_KEY_REPEAT) continue;
            if (flags & SCREEN_FLAG_KEY_DOWN) g_held_keys |= bit;
            else g_held_keys &= ~bit;
            if (sym == KEYCODE_ESCAPE && (flags & SCREEN_FLAG_KEY_DOWN)) g_quit = true;
        } else if (type == SCREEN_EVENT_POINTER) {
            // M3 finding (RPi5 hardware): SCREEN_PROPERTY_POSITION on this
            // image reports coordinates in on-screen/display pixels, not
            // internal-buffer pixels — measured running up to (1725,838) on
            // a 1920x1080 display against a nominally 1280x720 window/buffer.
            // Scale by buffer/onscreen (plat_init sizes+positions the window
            // to exactly cover the display at (0,0), so no offset term is
            // needed — see the plat_init comment) and clamp defensively.
            int pos[2] = { 0, 0 };
            int buttons = 0;
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_POSITION, pos);
            screen_get_event_property_iv(n->ev, SCREEN_PROPERTY_BUTTONS, &buttons);
            float sx = (float)pos[0] * ((float)n->buffer_w / (float)n->onscreen_w);
            float sy = (float)pos[1] * ((float)n->buffer_h / (float)n->onscreen_h);
            if (sx < 0.0f) sx = 0.0f;
            if (sx > (float)n->buffer_w) sx = (float)n->buffer_w;
            if (sy < 0.0f) sy = 0.0f;
            if (sy > (float)n->buffer_h) sy = (float)n->buffer_h;
            g_mouse_x = sx;
            g_mouse_y = sy;
            g_mouse_down = (buttons & SCREEN_LEFT_MOUSE_BUTTON) != 0;
            if (getenv("WH_KEYDEBUG"))
                fprintf(stderr, "MOUSEDBG raw=(%d,%d) scaled=(%.1f,%.1f) buttons=0x%x down=%d\n",
                        pos[0], pos[1], sx, sy, buttons, g_mouse_down);
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
