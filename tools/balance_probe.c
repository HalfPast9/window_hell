// tools/balance_probe.c — headless sim harness for balance tuning.
//
// Drives the real sim_step() with synthetic input (no platform, no GL, no X)
// and prints how the window rect evolves under a given fire policy. Exists
// because "does pushing actually reclaim space?" is a question about sim
// math, and answering it by screenshotting a software-rendered X window is
// both slower and weaker evidence.
//
// Build/run:  make balance-probe && ./bin/balance-probe
#include <stdio.h>

#include "../src/platform.h"
#include "../src/sim.h"
#include "../src/snapshot.h"

// The sim links against plat_time_ns/plat_sleep_until_ns only through main.c,
// not through sim.c — so nothing to stub. sim.c is self-contained.

typedef enum { POLICY_IDLE, POLICY_PUSH_RIGHT, POLICY_SPLIT_4 } Policy;

// Some probes want to watch the window in isolation. Enemies would kill the
// stationary probe "player" long before the window reaches its floor. Parking
// them far outside the window (rather than deleting them) keeps enemy_count
// nonzero, so the wave never "clears" and the sim stays in PLAY — deleting
// them instead drops it into UPGRADE, which correctly freezes the window and
// hides the very curve we're trying to see.
static bool g_park_enemies = false;

static void set_input(Sim* sim, Policy p, uint64_t tick) {
    sim->input[0].keys = 0;
    sim->input[0].mouse_down = (p != POLICY_IDLE);

    switch (p) {
        case POLICY_IDLE:
            sim->input[0].mouse_x = sim->players[0].x;
            sim->input[0].mouse_y = sim->players[0].y - 100.0f;
            break;
        case POLICY_PUSH_RIGHT:  // aim straight out through the right edge
            sim->input[0].mouse_x = INTERNAL_W;
            sim->input[0].mouse_y = sim->players[0].y;
            break;
        case POLICY_SPLIT_4: {   // rotate aim across all four edges
            int quadrant = (int)((tick / 60) % 4);
            float tx[4] = { INTERNAL_W, 0.0f, INTERNAL_W * 0.5f, INTERNAL_W * 0.5f };
            float ty[4] = { INTERNAL_H * 0.5f, INTERNAL_H * 0.5f, 0.0f, INTERNAL_H };
            sim->input[0].mouse_x = tx[quadrant];
            sim->input[0].mouse_y = ty[quadrant];
            break;
        }
    }
}

static void run(const char* label, Policy p, int seconds) {
    Sim sim;
    static SnapshotBuffer sb;  // large; keep it off the stack
    sim_init(&sim, 12345);
    snapshot_buffer_init(&sb);

    // Jump straight into PLAY (bypass MODE_SELECT + COLOR_SELECT): a shoot
    // edge confirms SINGLE PLAYER (default cursor), landing in COLOR_SELECT;
    // a second shoot edge confirms the default color, landing in PLAY.
    sim.input[0].mouse_down = true;
    sim_step(&sim);
    sim.input[0].mouse_down = false;
    sim_step(&sim);
    sim.input[0].mouse_down = true;
    sim_step(&sim);
    sim.input[0].mouse_down = false;
    sim_step(&sim);

    printf("\n=== %s ===\n", label);
    printf(" t(s)   width   height   danger  lives  state  enemies\n");

    uint64_t total = (uint64_t)seconds * SIM_HZ;
    for (uint64_t i = 0; i < total; i++) {
        set_input(&sim, p, i);
        if (g_park_enemies) {
            if (sim.enemy_count == 0) sim.enemy_count = 1;  // keep the wave un-cleared
            for (uint16_t e = 0; e < sim.enemy_count; e++) {
                sim.enemies[e].x = -1000.0f;
                sim.enemies[e].y = -1000.0f;
                sim.enemies[e].hp = 999.0f;
            }
        }
        sim_step(&sim);

        if (i % SIM_HZ == 0) {
            // Read the published snapshot rather than recomputing anything —
            // a probe that duplicates sim math will happily report on its own
            // stale copy of a formula instead of the one the game runs.
            sim_publish(&sim, &sb);
            const SimSnapshot* s = snapshot_acquire_read(&sb);

            static const char* names[] = { "MODE", "WAIT", "CLR", "PLAY", "UPGR", "DEAD" };
            printf("%5llu  %6.1f  %6.1f   %5.2f  %5d  %5s  %5u\n",
                    (unsigned long long)(i / SIM_HZ),
                    s->playfield_w, s->playfield_h, s->danger,
                    s->lives, names[s->state < 6 ? s->state : 0], s->enemy_count);
        }
    }
}

int main(void) {
    printf("push_base=%.1f  fire_hz=%.1f  shrink=%.1f px/s/edge\n",
            (double)WINDOW_PUSH_BASE_DEFAULT, (double)PSHOT_FIRE_HZ_DEFAULT,
            (double)WINDOW_SHRINK_RATE_DEFAULT);
    printf("start %.0fx%.0f, floor %.0fx%.0f\n",
            (double)WINDOW_START_W, (double)WINDOW_START_H,
            (double)WINDOW_MIN_W, (double)WINDOW_MIN_H);

    g_park_enemies = true;
    run("never shoot (pure shrink to the floor)", POLICY_IDLE, 50);
    run("all fire -> right edge", POLICY_PUSH_RIGHT, 12);
    run("fire split across all 4 edges", POLICY_SPLIT_4, 12);

    g_park_enemies = false;
    run("never shoot, enemies live (how long a passive player lasts)", POLICY_IDLE, 14);
    return 0;
}
