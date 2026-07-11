// platform.h — game code includes ONLY this. No OS/windowing headers elsewhere.
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int   width, height;        // framebuffer size actually created
    void* native;               // opaque, owned by platform impl
} PlatformWindow;

typedef struct {
    uint32_t keys;               // bitmask: see KEY_* below
    float    mouse_x, mouse_y;   // cursor position, already converted to internal 1280x720 space
    bool     mouse_down;         // primary button (aim/shoot)
    bool     quit_requested;
} PlatformInput;

enum {
    KEY_UP      = 1 << 0,
    KEY_DOWN    = 1 << 1,
    KEY_LEFT    = 1 << 2,
    KEY_RIGHT   = 1 << 3,
    KEY_SHOOT   = 1 << 4,
    KEY_FOCUS   = 1 << 5,
    KEY_RESTART = 1 << 6,
    KEY_HUD     = 1 << 7,
    KEY_REC     = 1 << 8,
    KEY_PLAY    = 1 << 9,
};

bool     plat_init(PlatformWindow* w, int desired_w, int desired_h); // fullscreen ok
void     plat_shutdown(PlatformWindow* w);
void     plat_poll(PlatformWindow* w, PlatformInput* out);           // non-blocking
void     plat_swap(PlatformWindow* w);                               // eglSwapBuffers
uint64_t plat_time_ns(void);                                         // monotonic
void     plat_sleep_until_ns(uint64_t abs_ns);                       // absolute-deadline sleep

// STRETCH (compositor mode) — may be stubbed to no-op:
void     plat_set_playfield_rect(PlatformWindow* w, int x, int y, int wd, int ht);

#endif // PLATFORM_H
