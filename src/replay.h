// replay.h — input-log record/playback (PRD §6.5).
//
// The log stores *input*, not state: a run is reproduced by re-running the
// deterministic sim from (seed, input log). Records are written only when the
// input actually changes, so a log is a list of "at tick T the input became X".
//
// Threading: the sim thread appends records into a fixed-capacity buffer (no
// allocation, no I/O — §6.4 forbids both inside the sim). The render thread
// serializes that buffer to disk once recording stops. A blocking write() on
// the 240 Hz thread would register as a tick overrun on the very HUD that
// exists to show we never miss one.
#ifndef REPLAY_H
#define REPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

#define REPLAY_MAGIC   0x50524857u  // "WHRP"
#define REPLAY_VERSION 1u
#define REPLAY_PATH    "last.replay"

#define REPLAY_MAX_RECORDS 65536

// Mouse-fire rides in the key bitmask's first free bit (KEY_* uses bits 0..9),
// so one uint16 carries the whole trigger state. REPLAY_KEY_MASK must cover
// ONLY the real KEY_* bits — if it overlapped the mouse bit, unpacking would
// smuggle a phantom key into sim.input.keys and diverge playback.
#define REPLAY_KEY_MOUSE_FIRE (1u << 10)
#define REPLAY_KEY_MASK       0x03FFu  // bits 0..9 == KEY_UP .. KEY_PLAY

typedef struct {
    uint32_t tick;
    uint16_t keys;   // KEY_* bits | REPLAY_KEY_MOUSE_FIRE
    uint16_t aim_q;  // quantized aim angle, see sim.h AIM_Q_STEPS
} ReplayRecord;

typedef struct {
    uint32_t magic, version;
    uint64_t seed;
    uint64_t tick_count;
} ReplayHeader;

// --- recorder: sim thread appends, render thread flushes to disk ---
typedef struct {
    ReplayRecord      records[REPLAY_MAX_RECORDS];
    _Atomic uint32_t  count;      // release-stored by sim, acquire-read by render
    _Atomic bool      active;
    uint64_t          seed;
    // Total ticks observed, NOT the tick of the last record. Records are
    // change-only, so the final record can land long before the run ends;
    // using it as the length would truncate playback.
    _Atomic uint64_t  total_ticks;
    uint16_t          last_keys;
    uint16_t          last_aim_q;
    bool              have_last;
} ReplayRecorder;

void replay_record_init(ReplayRecorder* r);
void replay_record_start(ReplayRecorder* r, uint64_t seed);
// sim thread only; appends only when (keys, aim_q) differ from the last record
void replay_record_tick(ReplayRecorder* r, uint64_t tick, uint16_t keys, uint16_t aim_q);
// render thread only; call after recording has stopped. Returns bytes written, 0 on failure.
size_t replay_record_save(ReplayRecorder* r, const char* path);
static inline bool replay_record_active(const ReplayRecorder* r) {
    return atomic_load_explicit(&r->active, memory_order_acquire);
}

// --- player ---
typedef struct {
    ReplayRecord records[REPLAY_MAX_RECORDS];
    uint32_t     count;
    uint32_t     cursor;      // index of the next record to apply
    uint64_t     seed;
    uint64_t     tick_count;
    uint16_t     cur_keys;
    uint16_t     cur_aim_q;
    bool         playing;
} ReplayPlayer;

void replay_player_init(ReplayPlayer* p);
bool replay_load(ReplayPlayer* p, const char* path);   // render thread (file I/O)
// sim thread only; advances the log to `tick` and yields the input in force.
// Returns false once the log is exhausted (playback should stop).
bool replay_input_for_tick(ReplayPlayer* p, uint64_t tick, uint16_t* out_keys, uint16_t* out_aim_q);

#endif // REPLAY_H
