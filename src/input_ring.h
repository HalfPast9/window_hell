// input_ring.h — SPSC lock-free ring of InputFrame, render thread -> sim
// thread (PRD §4). Producer (render) pushes on plat_poll; consumer (sim)
// drains all pending frames each tick, folding them into its InputState —
// latest keymask/mouse state wins within a tick.
//
// Mouse rides through this same path (rather than sim reading the platform
// directly) so D6 determinism holds: sim's only input is the folded
// InputState, which is exactly what a replay log can reproduce.
#ifndef INPUT_RING_H
#define INPUT_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    uint32_t keys;
    float    mouse_x, mouse_y;  // internal 1280x720 space (live input)
    bool     mouse_down;
    // Replay playback supplies the aim angle directly instead of a cursor
    // position, since the recorded log stores the angle (see replay.h).
    uint16_t aim_q;
    bool     use_aim_q;
    uint64_t tick;
} InputFrame;

#define INPUT_RING_CAP 64  // must be a power of two

typedef struct {
    InputFrame       buf[INPUT_RING_CAP];
    _Atomic uint32_t head;  // producer-owned
    _Atomic uint32_t tail;  // consumer-owned
} InputRing;

static inline void input_ring_init(InputRing* r) {
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

// render thread only
static inline bool input_ring_push(InputRing* r, InputFrame frame) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= INPUT_RING_CAP) return false; // full: sim is behind, drop oldest-wins is fine
    r->buf[head & (INPUT_RING_CAP - 1)] = frame;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}

// sim thread only
static inline bool input_ring_pop(InputRing* r, InputFrame* out) {
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) return false; // empty
    *out = r->buf[tail & (INPUT_RING_CAP - 1)];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

#endif // INPUT_RING_H
