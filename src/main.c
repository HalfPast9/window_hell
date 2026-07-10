// main.c — M0 scaffold: opens the platform window, clears the screen, polls
// input until quit. Sim thread / render batching / snapshot wiring is M1+.
#include "platform.h"

#include <GLES2/gl2.h>
#include <stdio.h>

#define INTERNAL_W 1280
#define INTERNAL_H 720

int main(void) {
    PlatformWindow win = {0};
    if (!plat_init(&win, INTERNAL_W, INTERNAL_H)) {
        fprintf(stderr, "main: plat_init failed\n");
        return 1;
    }

    glViewport(0, 0, win.width, win.height);
    glClearColor(0.0392f, 0.0392f, 0.0706f, 1.0f); // void color #0A0A12

    PlatformInput input = {0};
    while (!input.quit_requested) {
        plat_poll(&win, &input);
        glClear(GL_COLOR_BUFFER_BIT);
        plat_swap(&win);
    }

    plat_shutdown(&win);
    return 0;
}
