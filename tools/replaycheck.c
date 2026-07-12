// tools/replaycheck.c — determinism CI (PRD §9.1) + replay round-trip proof.
//
// Three runs over the same scripted input:
//   A. live-style input          -> hash A   (and record a replay log)
//   B. same scripted input again -> hash B   (§9.1: sim is deterministic)
//   C. replay A's log            -> hash C   (playback reproduces the run)
// Asserts A == B == C. C is the one that backs the demo line: "here's that
// exact run replayed from a tiny file."
//
// Headless: no GL, no X, no platform layer. Build: make replaycheck
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform.h"   // KEY_*
#include "../src/replay.h"
#include "../src/sim.h"

#define CHECK_TICKS 10000

// Deterministic scripted input: an orbiting cursor, periodic fire, and a
// rotating movement pattern. Exercises aim (which changes nearly every tick),
// shooting, and movement.
static void scripted_input(uint64_t tick, uint32_t* keys, float* mx, float* my, bool* fire) {
    float t = (float)tick;
    *mx = 640.0f + 400.0f * cosf(t * 0.011f);
    *my = 360.0f + 200.0f * sinf(t * 0.017f);
    *fire = ((tick / 37) % 3) == 0;

    uint32_t k = 0;
    switch ((tick / 61) % 4) {
        case 0: k |= KEY_LEFT;  break;
        case 1: k |= KEY_UP;    break;
        case 2: k |= KEY_RIGHT; break;
        default: k |= KEY_DOWN; break;
    }
    if ((tick / 211) % 5 == 0) k |= KEY_FOCUS;
    *keys = k;
}

// sim_hash() covers the raw struct, including InputState. Live input carries a
// cursor position; replayed input carries an aim angle instead. Both produce
// identical *gameplay*, so the comparison must exclude the input record itself
// — otherwise we'd be diffing how the tick was driven, not what it did.
static uint64_t gameplay_hash(const Sim* sim) {
    Sim tmp = *sim;
    memset(tmp.input, 0, sizeof(tmp.input));
    return sim_hash(&tmp);
}

static uint64_t run_live(uint64_t seed, ReplayRecorder* rec) {
    Sim sim;
    sim_init(&sim, seed);
    if (rec) replay_record_start(rec, seed);

    for (uint64_t i = 1; i <= CHECK_TICKS; i++) {
        uint32_t keys; float mx, my; bool fire;
        scripted_input(i, &keys, &mx, &my, &fire);

        sim.input[0].keys = keys;
        sim.input[0].mouse_x = mx;
        sim.input[0].mouse_y = my;
        sim.input[0].mouse_down = fire;
        sim.input[0].use_aim_q = false;

        sim_step(&sim);

        if (rec) {
            uint16_t k16 = (uint16_t)(sim.input[0].keys & REPLAY_KEY_MASK);
            if (sim.input[0].mouse_down) k16 |= REPLAY_KEY_MOUSE_FIRE;
            replay_record_tick(rec, sim.tick, k16, sim.players[0].aim_q);
        }
    }
    return gameplay_hash(&sim);
}

static uint64_t run_playback(ReplayPlayer* p) {
    Sim sim;
    sim_init(&sim, p->seed);

    for (uint64_t i = 1; i <= CHECK_TICKS; i++) {
        uint16_t k16, aim_q;
        if (!replay_input_for_tick(p, i, &k16, &aim_q)) break;

        sim.input[0].keys = (uint32_t)(k16 & REPLAY_KEY_MASK);
        sim.input[0].mouse_down = (k16 & REPLAY_KEY_MOUSE_FIRE) != 0;
        sim.input[0].aim_q = aim_q;
        sim.input[0].use_aim_q = true;

        sim_step(&sim);
    }
    return gameplay_hash(&sim);
}

int main(void) {
    const uint64_t seed = 0xC0FFEEull;
    const char* path = "replaycheck.replay";

    static ReplayRecorder rec;
    static ReplayPlayer   ply;
    replay_record_init(&rec);
    replay_player_init(&ply);

    uint64_t a = run_live(seed, &rec);
    uint64_t b = run_live(seed, NULL);

    uint32_t n_records = atomic_load(&rec.count);
    size_t bytes = replay_record_save(&rec, path);

    if (!replay_load(&ply, path)) {
        fprintf(stderr, "replaycheck: FAIL - could not reload '%s'\n", path);
        return 1;
    }
    uint64_t c = run_playback(&ply);

    printf("ticks           %d (%.1f s of sim @ %d Hz)\n",
            CHECK_TICKS, (double)CHECK_TICKS / SIM_HZ, SIM_HZ);
    printf("replay records  %u  (%zu bytes on disk, %.1f bytes/tick)\n",
            n_records, bytes, (double)bytes / CHECK_TICKS);
    printf("hash A (live)     %016llx\n", (unsigned long long)a);
    printf("hash B (live x2)  %016llx  %s\n", (unsigned long long)b,
            a == b ? "== A  determinism OK" : "!= A  NON-DETERMINISTIC");
    printf("hash C (replay)   %016llx  %s\n", (unsigned long long)c,
            a == c ? "== A  replay is bit-identical" : "!= A  REPLAY DIVERGED");

    remove(path);

    if (a != b) { fprintf(stderr, "\nreplaycheck: FAIL - sim is not deterministic\n"); return 1; }
    if (a != c) { fprintf(stderr, "\nreplaycheck: FAIL - replay diverged from the live run\n"); return 1; }
    printf("\nreplaycheck: PASS\n");
    return 0;
}
