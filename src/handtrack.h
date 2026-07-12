// handtrack.h — optional camera/hand-tracking control input. A tracker
// process (on the Pi or a dev laptop) sends HtPacket over UDP; ht_merge()
// folds the result into the same InputFrame that keyboard/mouse populate,
// so the sim never knows or cares where its input came from (determinism
// and replay are unaffected — see main.c's use of InputFrame).
//
// Keyboard/mouse are never disabled: hand bits OR into the keymask and
// mouse_down, so a live keyboard always overrides/backs up a dead or absent
// tracker. A 250ms packet-staleness timeout does the same automatically.
#ifndef HANDTRACK_H
#define HANDTRACK_H

#include <stdint.h>
#include <stdbool.h>

#include "platform.h"    // KEY_* bitmask used when merging into InputFrame
#include "input_ring.h"

#define HT_MAGIC        0x31525448u  // "HTR1" (little-endian on the wire)
#define HT_VERSION      1
#define HT_PORT_DEFAULT 47800

// buttons (packet-level gesture state, already debounced by the sender)
#define HT_BTN_SHOOT (1u << 0)  // right-hand fist
#define HT_BTN_FOCUS (1u << 1)  // left-hand fist

// flags (per-frame tracking validity)
#define HT_FLAG_MOVE_VALID (1u << 0)  // left hand tracked this frame
#define HT_FLAG_AIM_VALID  (1u << 1)  // right hand tracked this frame

// Wire packet, 16 bytes, little-endian on both ends (x86-64 dev box and
// aarch64le Pi are both LE, so a packed-struct memcpy is safe as long as
// magic/version/size are all validated on receipt).
typedef struct {
    uint32_t magic;     // HT_MAGIC
    uint8_t  version;   // HT_VERSION
    uint8_t  buttons;   // HT_BTN_*
    uint16_t seq;       // sender-incremented, wraps; receiver keeps newest
    int16_t  move_x_q;  // -32767..32767 == -1..+1; +x = screen right
    int16_t  move_y_q;  // -32767..32767 == -1..+1; +y = screen DOWN
    uint16_t aim_q;     // 1/65536 turn; same convention as sim.h AIM_Q_STEPS
    uint8_t  flags;     // HT_FLAG_*
    uint8_t  reserved;  // 0
} HtPacket;
_Static_assert(sizeof(HtPacket) == 16, "HtPacket must match the documented wire size");

typedef struct {
    int      fd;              // -1 = disabled (init failed or not requested)
    uint32_t held_keys;       // hysteresis output: KEY_UP/DOWN/LEFT/RIGHT/FOCUS
    bool     shoot_down;
    uint16_t aim_q;
    bool     aim_valid;
    uint16_t last_seq;
    bool     have_seq;
    uint64_t last_packet_ns;
    bool     debug;           // WH_HTDEBUG=1
} HtState;

// Binds a non-blocking UDP socket on `port` (INADDR_ANY, so a dev-laptop
// sender works over LAN, not just loopback). Returns false on failure —
// callers must treat that as non-fatal and keep running keyboard-only.
bool ht_init(HtState* s, uint16_t port);

// Drains all pending packets, applies staleness/hysteresis, and merges the
// result into *f. Call once per render frame, between plat_poll() and
// input_ring_push() — see main.c.
void ht_merge(HtState* s, InputFrame* f, uint64_t now_ns);

void ht_shutdown(HtState* s);

#endif // HANDTRACK_H
