// tools/mp_check.c — the lockstep analogue of replaycheck.c: proves the
// REAL netplay wire path (actual UDP sockets, actual packet pack/unpack,
// actual input-delay buffering — nothing mocked) produces bit-identical
// Sim state on both ends of a co-op session.
//
// Two Sims, two NetplayStates, one HOST and one JOIN, talking over real
// loopback UDP within this one process. Both are driven with different
// scripted per-player input for CHECK_TICKS ticks, then compared via
// mp_hash() below — the multiplayer analogue of replaycheck.c's
// gameplay_hash(). Unlike SP replay (live vs. replayed input differ in
// shape), MP's two sides resolve the IDENTICAL dual InputState[2] every
// tick before stepping, so the only field that's legitimately asymmetric
// by design is net_role itself (each side knows its own HOST/JOIN role) —
// mp_hash() excludes just that one field, making this a STRONGER check
// than SP's, not a weaker one.
//
// Headless: no GL, no X, no platform layer. Build: make mp-check
#define _POSIX_C_SOURCE 200809L  // exposes clock_gettime()/CLOCK_MONOTONIC under -std=c11
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/netplay.h"
#include "../src/platform.h"  // KEY_*
#include "../src/sim.h"

#define TEST_PORT   47999
#define CHECK_TICKS 3000

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Distinct per-player patterns so the two ships visibly diverge if lockstep
// ever mixed up which input went to which slot.
static InputState scripted_input(int player_idx, uint64_t local_tick) {
    InputState s = {0};
    float t = (float)local_tick;
    float cx = (player_idx == 0) ? 400.0f : 900.0f;
    s.mouse_x = cx + 200.0f * cosf(t * 0.013f + (float)player_idx);
    s.mouse_y = 360.0f + 150.0f * sinf(t * 0.019f + (float)player_idx);
    s.mouse_down = ((int)(local_tick / 41) % 3) == (player_idx % 3);
    uint32_t k = 0;
    switch ((int)((local_tick / 53) % 4)) {
        case 0: k |= KEY_LEFT;  break;
        case 1: k |= KEY_UP;    break;
        case 2: k |= KEY_RIGHT; break;
        default: k |= KEY_DOWN; break;
    }
    s.keys = k;
    s.use_aim_q = false;
    return s;
}

// sim_hash() covers the raw struct, and net_role (HOST vs JOIN) is the one
// field each side legitimately sets to a different value on purpose — every
// other byte, including both players' full input history, is supposed to be
// bit-identical by construction.
static uint64_t mp_hash(const Sim* sim) {
    Sim tmp = *sim;
    tmp.net_role = 0;
    return sim_hash(&tmp);
}

int main(void) {
    Sim sim_host, sim_join;
    sim_init(&sim_host, 0);
    sim_init(&sim_join, 0);

    NetplayState net_host, net_join;
    netplay_init(&net_host, NET_ROLE_HOST, NULL, TEST_PORT);
    netplay_init(&net_join, NET_ROLE_JOIN, "127.0.0.1", TEST_PORT);
    if (net_host.fd < 0 || net_join.fd < 0) {
        fprintf(stderr, "mp_check: FAIL - could not bind test sockets (port %d busy?)\n", TEST_PORT);
        return 1;
    }

    printf("mp_check: handshaking over real loopback UDP...\n");
    long handshake_iters = 0;
    while (!(net_host.established && net_join.established)) {
        uint64_t now = now_ns();
        netplay_service(&net_host, &sim_host, sim_host.input[0], now);
        netplay_service(&net_join, &sim_join, sim_join.input[0], now);
        if (++handshake_iters > 2000000) {
            fprintf(stderr, "mp_check: FAIL - handshake never completed\n");
            return 1;
        }
    }
    printf("mp_check: established — seed host=%llu join=%llu, delay host=%d join=%d, "
            "local_idx host=%d join=%d\n",
            (unsigned long long)sim_host.seed, (unsigned long long)sim_join.seed,
            net_host.input_delay_ticks, net_join.input_delay_ticks,
            net_host.local_player_idx, net_join.local_player_idx);

    if (sim_host.seed != sim_join.seed) {
        fprintf(stderr, "mp_check: FAIL - seeds disagree (%llu vs %llu)\n",
                (unsigned long long)sim_host.seed, (unsigned long long)sim_join.seed);
        return 1;
    }
    if (net_host.local_player_idx == net_join.local_player_idx) {
        fprintf(stderr, "mp_check: FAIL - both sides think they're player %d\n", net_host.local_player_idx);
        return 1;
    }

    uint64_t local_tick_host = 0, local_tick_join = 0;
    long stall_host = 0, stall_join = 0, iters = 0;
    bool mismatch = false;

    while (sim_host.tick < CHECK_TICKS || sim_join.tick < CHECK_TICKS) {
        uint64_t now = now_ns();

        if (sim_host.tick < CHECK_TICKS) {
            InputState in = scripted_input(0, local_tick_host);
            if (netplay_service(&net_host, &sim_host, in, now)) {
                sim_step(&sim_host);
                local_tick_host++;
            } else {
                stall_host++;
            }
        }
        if (sim_join.tick < CHECK_TICKS) {
            InputState in = scripted_input(1, local_tick_join);
            if (netplay_service(&net_join, &sim_join, in, now)) {
                sim_step(&sim_join);
                local_tick_join++;
            } else {
                stall_join++;
            }
        }

        if (sim_host.tick > 0 && sim_host.tick == sim_join.tick && sim_host.tick % 100 == 0) {
            uint64_t hh = mp_hash(&sim_host), hj = mp_hash(&sim_join);
            if (hh != hj && !mismatch) {
                fprintf(stderr, "mp_check: FAIL - hash mismatch at tick %llu (host %016llx != join %016llx)\n",
                        (unsigned long long)sim_host.tick, (unsigned long long)hh, (unsigned long long)hj);
                mismatch = true;
            }
        }

        if (++iters > 20000000) {
            fprintf(stderr, "mp_check: FAIL - stalled indefinitely (host tick %llu, join tick %llu)\n",
                    (unsigned long long)sim_host.tick, (unsigned long long)sim_join.tick);
            return 1;
        }
    }

    uint64_t final_h = mp_hash(&sim_host), final_j = mp_hash(&sim_join);
    printf("mp_check: ticks host=%llu join=%llu\n",
            (unsigned long long)sim_host.tick, (unsigned long long)sim_join.tick);
    printf("mp_check: stalls host=%ld join=%ld (near 0 expected on loopback)\n", stall_host, stall_join);
    printf("mp_check: hash host  %016llx\n", (unsigned long long)final_h);
    printf("mp_check: hash join  %016llx  %s\n", (unsigned long long)final_j,
            final_h == final_j ? "== host  lockstep OK" : "!= host  LOCKSTEP DIVERGED");
    printf("mp_check: desync flags host=%d join=%d, peer_lost host=%d join=%d\n",
            net_host.desync, net_join.desync, net_host.peer_lost, net_join.peer_lost);

    netplay_shutdown(&net_host);
    netplay_shutdown(&net_join);

    if (mismatch || final_h != final_j) {
        fprintf(stderr, "\nmp_check: FAIL - hashes diverged\n");
        return 1;
    }
    if (net_host.desync || net_join.desync) {
        fprintf(stderr, "\nmp_check: FAIL - desync flag set despite matching hashes "
                         "(bug in the hash-exchange check itself, see netplay.c)\n");
        return 1;
    }
    printf("\nmp_check: PASS\n");
    return 0;
}
