// snapshot.h — SimSnapshot (PRD §6.3) + lock-free triple buffer, sim -> render.
//
// Classic triple buffer: one atomic "state" byte packs the index of the
// currently-unclaimed ("middle") slot plus a dirty bit. Sim exclusively owns
// write_idx, render exclusively owns read_idx; the two never contend on
// anything but the single atomic exchange. Render always gets the newest
// complete snapshot; sim never blocks waiting on render and vice versa.
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>
#include <stdatomic.h>

#define MAX_BULLETS 4096
#define MAX_ENEMIES 128
#define MAX_PSHOTS  256
#define MAX_BEAMS   4

// SnapEntity.sprite: 0=player 1=triangle 2=circle 3=octagon 4=spiker
//                    5=enemy_bullet 6=player_shot 7=coin(unused v1)
// `age` is ticks since spawn (saturating). The renderer needs it to scale the
// spawn "pop" in (§8.6) — flags bit 4 says a pop is in progress, age says how
// far through it. Fits in the struct's existing tail padding.
typedef struct { float x, y; uint8_t sprite; uint8_t flags; uint8_t age; } SnapEntity;
// SnapEntity.flags bits: 1=outside_window(dim) 2=invuln_blink 4=spawn_pop 8=hit_white

// Length of the spawn-pop, in ticks. Part of the sim->render contract (it's
// what SnapEntity.age is measured against), so it lives here rather than in
// sim.h — the renderer must not include the sim's internals.
#define BULLET_POP_TICKS 4

// SnapBeam.state: 0=telegraph(dotted) 1=firing
typedef struct { float x, y, angle, len, width; uint8_t state; } SnapBeam;

enum { SIM_STATE_MENU = 0, SIM_STATE_PLAY, SIM_STATE_UPGRADE, SIM_STATE_DEAD };

typedef struct {
    uint64_t tick;

    // window (all internal-res coords; animated/eased edges, PRD §8.1)
    float playfield_x, playfield_y, playfield_w, playfield_h;
    float danger;              // 0..1 telegraph: border cyan->red + pulse rate
    float edge_flash[4];       // L,R,T,B white-flash intensity 0..1 on push

    // boss window (Spiker only). Fixed size, rides the boss's teleports. While
    // it overlaps the playfield rect the two windows MERGE into one playable
    // space (boss_vulnerable doubles as the "merged" flag): the renderer opens
    // the border where the rects cross, and the sim lets the player walk
    // through into the boss's room. Pushing your walls out to reach it IS how
    // you damage it.
    float   boss_win_x, boss_win_y, boss_win_w, boss_win_h;
    uint8_t boss_win_active;
    uint8_t boss_vulnerable;

    // teleport telegraph: ghost outline of where the boss window is about to
    // jump. Drawn for the whole telegraph so a player inside the boss's room
    // has warning to leave before the floor vanishes.
    uint8_t boss_tp_active;
    float   boss_tp_x, boss_tp_y;      // destination (boss center)
    float   boss_tp_progress;          // 0..1 through the telegraph

    // entities
    SnapEntity player;
    uint16_t bullet_count; SnapEntity bullets[MAX_BULLETS];
    uint16_t enemy_count;  SnapEntity enemies[MAX_ENEMIES];
    uint16_t pshot_count;  SnapEntity pshots[MAX_PSHOTS];
    uint16_t beam_count;   SnapBeam   beams[MAX_BEAMS];

    // run state + juice
    uint32_t score;
    uint8_t  lives, wave, state;    // state: SIM_STATE_*
    uint8_t  upgrade_a, upgrade_b;  // UPGRADE state: two offered ids (PRD §8.3a)
    uint8_t  upgrade_selected;      // 0 = a, 1 = b
    float    hit_flash, shake_x, shake_y;
} SimSnapshot;

#define SNAPSHOT_INDEX_MASK 0x3u
#define SNAPSHOT_DIRTY_BIT  0x4u

typedef struct {
    SimSnapshot slots[3];
    _Atomic uint32_t state;  // packed: (dirty<<2) | middle_index — shared handoff point
    int write_idx;           // owned by sim thread only
    int read_idx;            // owned by render thread only
} SnapshotBuffer;

static inline void snapshot_buffer_init(SnapshotBuffer* sb) {
    sb->write_idx = 0;
    sb->read_idx = 2;
    atomic_store_explicit(&sb->state, 1u, memory_order_relaxed); // middle = slot 1, not dirty
}

// sim thread: slot to fill in for this tick
static inline SimSnapshot* snapshot_begin_write(SnapshotBuffer* sb) {
    return &sb->slots[sb->write_idx];
}

// sim thread: publish the just-filled slot, reclaim the previous middle slot
static inline void snapshot_publish(SnapshotBuffer* sb) {
    uint32_t new_state = ((uint32_t)sb->write_idx & SNAPSHOT_INDEX_MASK) | SNAPSHOT_DIRTY_BIT;
    uint32_t old_state = atomic_exchange_explicit(&sb->state, new_state, memory_order_acq_rel);
    sb->write_idx = (int)(old_state & SNAPSHOT_INDEX_MASK);
}

// render thread: returns the newest published snapshot (only swaps if a newer one exists)
static inline const SimSnapshot* snapshot_acquire_read(SnapshotBuffer* sb) {
    uint32_t cur = atomic_load_explicit(&sb->state, memory_order_acquire);
    if (cur & SNAPSHOT_DIRTY_BIT) {
        uint32_t new_state = (uint32_t)sb->read_idx & SNAPSHOT_INDEX_MASK;
        uint32_t old_state = atomic_exchange_explicit(&sb->state, new_state, memory_order_acq_rel);
        sb->read_idx = (int)(old_state & SNAPSHOT_INDEX_MASK);
    }
    return &sb->slots[sb->read_idx];
}

#endif // SNAPSHOT_H
