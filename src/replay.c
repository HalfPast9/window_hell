#include "replay.h"

#include <stdio.h>
#include <string.h>

void replay_record_init(ReplayRecorder* r) {
    memset(r, 0, sizeof(*r));
    atomic_store_explicit(&r->count, 0, memory_order_relaxed);
    atomic_store_explicit(&r->active, false, memory_order_relaxed);
}

void replay_record_start(ReplayRecorder* r, uint64_t seed) {
    atomic_store_explicit(&r->count, 0, memory_order_relaxed);
    atomic_store_explicit(&r->total_ticks, 0, memory_order_relaxed);
    r->seed = seed;
    r->last_keys = 0;
    r->last_aim_q = 0;
    r->have_last = false;
    atomic_store_explicit(&r->active, true, memory_order_release);
}

void replay_record_tick(ReplayRecorder* r, uint64_t tick, uint16_t keys, uint16_t aim_q) {
    if (!atomic_load_explicit(&r->active, memory_order_acquire)) return;

    atomic_store_explicit(&r->total_ticks, tick, memory_order_release);

    if (r->have_last && keys == r->last_keys && aim_q == r->last_aim_q) return;

    uint32_t n = atomic_load_explicit(&r->count, memory_order_relaxed);
    if (n >= REPLAY_MAX_RECORDS) {
        atomic_store_explicit(&r->active, false, memory_order_release);  // full: stop cleanly
        return;
    }

    r->records[n] = (ReplayRecord){ .tick = (uint32_t)tick, .keys = keys, .aim_q = aim_q };
    atomic_store_explicit(&r->count, n + 1, memory_order_release);

    r->last_keys = keys;
    r->last_aim_q = aim_q;
    r->have_last = true;
}

size_t replay_record_save(ReplayRecorder* r, const char* path) {
    uint32_t n = atomic_load_explicit(&r->count, memory_order_acquire);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "replay: cannot open '%s' for writing\n", path);
        return 0;
    }

    ReplayHeader h = {
        .magic = REPLAY_MAGIC,
        .version = REPLAY_VERSION,
        .seed = r->seed,
        .tick_count = atomic_load_explicit(&r->total_ticks, memory_order_acquire),
    };
    size_t written = fwrite(&h, sizeof(h), 1, f) * sizeof(h);
    written += fwrite(r->records, sizeof(ReplayRecord), n, f) * sizeof(ReplayRecord);
    fclose(f);

    return written;
}

void replay_player_init(ReplayPlayer* p) {
    memset(p, 0, sizeof(*p));
}

bool replay_load(ReplayPlayer* p, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open '%s'\n", path);
        return false;
    }

    ReplayHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fprintf(stderr, "replay: '%s' is truncated (no header)\n", path);
        fclose(f);
        return false;
    }
    if (h.magic != REPLAY_MAGIC) {
        fprintf(stderr, "replay: '%s' is not a replay file\n", path);
        fclose(f);
        return false;
    }
    if (h.version != REPLAY_VERSION) {
        fprintf(stderr, "replay: '%s' is version %u, this build reads %u\n",
                 path, h.version, REPLAY_VERSION);
        fclose(f);
        return false;
    }

    size_t n = fread(p->records, sizeof(ReplayRecord), REPLAY_MAX_RECORDS, f);
    fclose(f);

    p->count = (uint32_t)n;
    p->cursor = 0;
    p->seed = h.seed;
    p->tick_count = h.tick_count;
    p->cur_keys = 0;
    p->cur_aim_q = 0;
    p->playing = true;
    return true;
}

bool replay_input_for_tick(ReplayPlayer* p, uint64_t tick, uint16_t* out_keys, uint16_t* out_aim_q) {
    if (!p->playing) return false;

    // Apply every record whose tick has arrived. The log is change-only, so
    // between records the previous input simply stays in force.
    while (p->cursor < p->count && p->records[p->cursor].tick <= tick) {
        p->cur_keys = p->records[p->cursor].keys;
        p->cur_aim_q = p->records[p->cursor].aim_q;
        p->cursor++;
    }

    if (p->cursor >= p->count && tick > p->tick_count) {
        p->playing = false;
        return false;
    }

    *out_keys = p->cur_keys;
    *out_aim_q = p->cur_aim_q;
    return true;
}
