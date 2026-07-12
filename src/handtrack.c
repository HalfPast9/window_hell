// handtrack.c — see handtrack.h. Pure BSD sockets; builds unchanged on
// Linux and QNX (QNX links -lsocket; see Makefile). Deliberately does not
// include sys/keycodes.h anywhere near this file (it #defines KEY_DOWN as
// a flag bit, colliding with platform.h's KEY_DOWN enum member).
#define _POSIX_C_SOURCE 200809L  // exposes fcntl()/close() prototypes under -std=c11 (see main.c)
#include "handtrack.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Movement hysteresis thresholds, in the packet's -1..+1 space. The 0.15
// gap between press and release is sized well above typical MediaPipe
// palm-center jitter so a hand held near a boundary can't chatter a key.
#define HT_PRESS_T   0.35f
#define HT_RELEASE_T 0.20f

// Packet-staleness timeout: no packet for this long means the tracker is
// gone (crashed, hand out of frame with no keepalive, laptop asleep, etc).
// All hand-derived input is cleared and keyboard/mouse silently resumes
// full control — no stuck keys, no dead man's switch to reset by hand.
#define HT_TIMEOUT_NS (250ull * 1000000ull)

bool ht_init(HtState* s, uint16_t port) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "handtrack: socket() failed: %s\n", strerror(errno));
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "handtrack: fcntl(O_NONBLOCK) failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "handtrack: bind(port %u) failed: %s\n", (unsigned)port, strerror(errno));
        close(fd);
        return false;
    }

    s->fd = fd;
    s->debug = getenv("WH_HTDEBUG") != NULL;
    printf("handtrack: listening on UDP :%u\n", (unsigned)port);
    return true;
}

// Latest-wins by sequence number, tolerant of 16-bit wraparound: treat the
// gap as a signed 16-bit delta, so e.g. seq 5 after seq 65534 (a wrap) is
// still recognized as "newer".
static bool ht_seq_is_newer(uint16_t new_seq, uint16_t old_seq) {
    return (int16_t)(new_seq - old_seq) > 0;
}

static void ht_apply_axis(uint32_t* keys, uint32_t neg_bit, uint32_t pos_bit, float v) {
    bool was_neg = (*keys & neg_bit) != 0;
    bool was_pos = (*keys & pos_bit) != 0;
    bool neg = was_neg, pos = was_pos;

    if (v <= -HT_PRESS_T)      { neg = true;  pos = false; }
    else if (v >= HT_PRESS_T)  { pos = true;  neg = false; }
    else if (fabsf(v) <= HT_RELEASE_T) { neg = false; pos = false; }
    // else: within the hysteresis band — keep whatever was already held

    *keys = (*keys & ~(neg_bit | pos_bit)) | (neg ? neg_bit : 0) | (pos ? pos_bit : 0);
}

void ht_merge(HtState* s, InputFrame* f, uint64_t now_ns) {
    if (s->fd < 0) return;

    HtPacket pkt;
    bool got_one = false;
    for (;;) {
        ssize_t n = recv(s->fd, &pkt, sizeof(pkt), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "handtrack: recv() failed: %s\n", strerror(errno));
            }
            break;
        }
        if ((size_t)n != sizeof(pkt) || pkt.magic != HT_MAGIC || pkt.version != HT_VERSION) {
            continue;  // malformed/foreign datagram on our port — ignore
        }
        if (got_one && !ht_seq_is_newer(pkt.seq, s->last_seq)) {
            continue;  // reordered/duplicate within this drain — keep the newer one
        }
        got_one = true;
        s->last_seq = pkt.seq;
        s->have_seq = true;
        s->last_packet_ns = now_ns;

        float mx = (float)pkt.move_x_q / 32767.0f;
        float my = (float)pkt.move_y_q / 32767.0f;

        if (pkt.flags & HT_FLAG_MOVE_VALID) {
            ht_apply_axis(&s->held_keys, KEY_LEFT, KEY_RIGHT, mx);
            ht_apply_axis(&s->held_keys, KEY_UP, KEY_DOWN, my);
            if (pkt.buttons & HT_BTN_FOCUS) s->held_keys |= KEY_FOCUS;
            else                            s->held_keys &= ~(uint32_t)KEY_FOCUS;
        } else {
            // Hand explicitly not tracked this frame (sender still sent a
            // keepalive) — release movement immediately rather than waiting
            // for the full staleness timeout.
            s->held_keys &= ~(uint32_t)(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_FOCUS);
        }

        s->shoot_down = (pkt.buttons & HT_BTN_SHOOT) != 0;

        if (pkt.flags & HT_FLAG_AIM_VALID) {
            s->aim_q = pkt.aim_q;
            s->aim_valid = true;
        } else {
            s->aim_valid = false;
        }

        if (s->debug) {
            fprintf(stderr, "handtrack: seq=%u move=(%.2f,%.2f) aim_q=%u btn=%02x flags=%02x\n",
                    (unsigned)pkt.seq, mx, my, (unsigned)pkt.aim_q,
                    (unsigned)pkt.buttons, (unsigned)pkt.flags);
        }
    }

    if (!s->have_seq || (now_ns - s->last_packet_ns) > HT_TIMEOUT_NS) {
        s->held_keys = 0;
        s->shoot_down = false;
        s->aim_valid = false;
    }

    f->keys |= s->held_keys;
    f->mouse_down = f->mouse_down || s->shoot_down;
    if (s->aim_valid) {
        f->aim_q = s->aim_q;
        f->use_aim_q = true;
    }
    // else: leave f->use_aim_q as the caller set it (false for live input),
    // so mouse-cursor aim resumes automatically — see step_aim in sim.c.
}

void ht_shutdown(HtState* s) {
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
