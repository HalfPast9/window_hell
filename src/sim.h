// sim.h — the real-time sim core (PRD §6, §8). M4 scope: enemies (Triangle,
// Circle), waves, all collision loops that exist without Octagon/Spiker
// (pshots x enemies, enemies x player), lives/states/score, upgrades.
// Octagon/Spiker/enemy-bullets/beams are M5 — no enemy fires anything yet.
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <stdbool.h>

#include "input_ring.h"
#include "rng.h"
#include "snapshot.h"

#define SIM_HZ 240
#define SIM_DT (1.0f / SIM_HZ)

#define INTERNAL_W 1280.0f
#define INTERNAL_H 720.0f

enum { EDGE_LEFT = 0, EDGE_RIGHT = 1, EDGE_TOP = 2, EDGE_BOTTOM = 3 };

// window geometry: PRD §8.1
#define WINDOW_START_W 900.0f
#define WINDOW_START_H 560.0f
#define WINDOW_MIN_W   140.0f
#define WINDOW_MAX_W   1180.0f
#define WINDOW_MIN_H   140.0f
#define WINDOW_MAX_H   660.0f
#define WINDOW_RESET_SCALE 0.6f  // crush: rect resets to 60% of start size

#define WINDOW_SHRINK_RATE_DEFAULT 8.0f  // px/s, base (no wave/Spiker scaling — M5)

#define WINDOW_SPRING_K    180.0f
#define WINDOW_SPRING_ZETA 0.55f

// Each shot shoves hard (PRD's 6.0 was tuned for the old 12/s fire rate).
// Sized against the shrink: holding both window dimensions costs 32 px/s of
// push, and 1.9 volleys/s x (16 + 0.85*16) supplies ~56 px/s — so ~57% of
// your fire goes to wall upkeep and ~43% is left for enemies. Deliberately
// short of comfortable: the window always wants to close, but a focused
// burst reclaims real space. See DESIGN_CHANGES.md.
#define WINDOW_PUSH_BASE_DEFAULT    16.0f
#define WINDOW_PUSH_DECAY           0.85f
#define WINDOW_PUSH_FLOOR           2.0f
#define WINDOW_PUSH_DECAY_WINDOW_TICKS (SIM_HZ / 2)  // 0.5s

#define SHAKE_DECAY 0.9f  // unified decay for the single shake_x/shake_y accumulator (§8.6)

// Danger telegraph (PRD §8.1): ramps 0 -> 1 as the smaller window dimension
// falls from DANGER_ONSET_PX to the hard floor. The PRD writes this as
// "(180 - min(w,h)) / 120", but min(w,h) can never go below the 140 px floor,
// so that expression maxes out at 0.33 — the border would never finish going
// red and the pulse would never reach full rate, despite §10 listing the
// telegraph as never-cut. Divisor is the actual span (180 - 140) instead.
#define DANGER_ONSET_PX 180.0f

// player: PRD §8.2
#define PLAYER_SPEED_DEFAULT       260.0f
#define PLAYER_FOCUS_SPEED_DEFAULT 120.0f
#define PLAYER_HITBOX_R    3.0f
#define LIVES_START 3
#define LIVES_START_COOP 4  // shared pool: any hit on either ship costs one

// Co-op balance knobs (two guns ~= double push + DPS against a single shared
// window/wave). Starting points for a feel pass, same spirit as the SP
// PSHOT_FIRE_HZ_DEFAULT retune in DESIGN_CHANGES.md — not final.
#define COOP_SHRINK_MULT 1.6f
#define COOP_WAVE_BONUS  1   // extra Triangle/Circle/Octagon per wave in co-op
#define COOP_ENEMY_HP_MULT 1.5f  // two guns kill much faster; confirmed via real 2-Pi playtest

#define PSHOT_SPEED      700.0f
// Shots/sec while held. Deliberately slow: shooting is the managed resource
// (see DESIGN_CHANGES.md §2) — the cost is the ~526ms you can't fire again,
// not a depletable ammo pool. Starting point for the playtest pass, not final.
#define PSHOT_FIRE_HZ_DEFAULT 1.9f
#define PSHOT_OFFSET     4.0f        // spacing between parallel bullets in a volley
#define PSHOT_DESPAWN_MARGIN 60.0f
#define PSHOT_HIT_RADIUS 3.0f
#define PSHOT_DAMAGE     1.0f
#define PSHOT_MULTISHOT_DEFAULT 2

#define PLAYER_INVULN_TICKS (int)(2.0f * SIM_HZ)
#define PLAYER_HIT_SHAKE  8.0f
#define ENEMY_KILL_SHAKE  2.0f
#define HITSTOP_ENEMY_KILL_TICKS  3
#define HITSTOP_PLAYER_HIT_TICKS  6

// enemies: PRD §8.3
enum { ENEMY_TRIANGLE = 0, ENEMY_CIRCLE = 1, ENEMY_OCTAGON = 2, ENEMY_SPIKER = 3 };

#define TRIANGLE_HP 6.0f
#define TRIANGLE_SPEED 90.0f
#define TRIANGLE_SLOW_RADIUS 60.0f
#define TRIANGLE_MIN_SPEED_FRAC 0.15f
#define TRIANGLE_RADIUS 7.0f
#define TRIANGLE_SCORE 100u

#define CIRCLE_HP 4.0f
#define CIRCLE_RADIUS 7.0f
#define CIRCLE_APPROACH_SPEED 40.0f
#define CIRCLE_DASH_SPEED 480.0f
#define CIRCLE_WINDUP_TICKS (int)(0.30f * SIM_HZ)
#define CIRCLE_DASH_TICKS   (int)(0.35f * SIM_HZ)
#define CIRCLE_IDLE_TICKS   (int)(1.20f * SIM_HZ)
#define CIRCLE_SCORE 150u

enum { CIRCLE_IDLE = 0, CIRCLE_WINDUP = 1, CIRCLE_DASHING = 2 };

#define OCTAGON_HP 10.0f
#define OCTAGON_RADIUS 8.0f
#define OCTAGON_DRIFT_SPEED 70.0f
#define OCTAGON_LATCH_EPS 2.0f              // considered latched within this of its edge
#define OCTAGON_FIRE_INTERVAL_TICKS (int)(1.6f * SIM_HZ)
#define OCTAGON_SCORE 250u

#define ENEMY_BULLET_SPEED 210.0f
#define ENEMY_BULLET_RADIUS 4.0f            // fat: visibility is king (§0.1)

// Spiker boss: PRD §8.3. Immobile but teleports; every 5th wave.
#define SPIKER_WAVE_INTERVAL 5
#define SPIKER_HP_BASE 48.0f
#define SPIKER_HP_GROWTH 1.5f               // +50% per subsequent appearance
#define SPIKER_RADIUS 16.0f
#define SPIKER_SCORE 2000u
#define SPIKER_SHRINK_MULT 1.6f             // room closes faster while it lives

#define SPIKER_RADIAL_BULLETS 8
#define SPIKER_RADIAL_INTERVAL_TICKS (int)(2.0f * SIM_HZ)
#define SPIKER_RADIAL_MIN_TICKS      (int)(0.7f * SIM_HZ)
#define SPIKER_RADIAL_SPEEDUP 0.85f         // interval shrinks as its wave counter climbs

// The boss beam locks onto the player, telegraphs, then fires straight — it
// does NOT sweep. The telegraph is the player's window to step off the line.
#define SPIKER_LASER_INTERVAL_TICKS  (int)(6.0f * SIM_HZ)
#define SPIKER_LASER_TELEGRAPH_TICKS (int)(0.8f * SIM_HZ)
#define SPIKER_LASER_FIRE_TICKS      (int)(0.35f * SIM_HZ)
#define SPIKER_LASER_WIDTH 12.0f
#define SPIKER_LASER_LEN 2400.0f            // long enough to always cross the screen

// Gaster-blasters: free-floating emitters that spawn away from the boss, lock
// aim at the player, hover, then fire. Lasers cross the void, so these are the
// boss's only reach before the windows merge.
#define MAX_BLASTERS 3
#define BLASTER_VOLLEY 2
#define BLASTER_INTERVAL_TICKS  (int)(3.5f * SIM_HZ)
#define BLASTER_TELEGRAPH_TICKS (int)(0.55f * SIM_HZ)
#define BLASTER_FIRE_TICKS      (int)(0.30f * SIM_HZ)
#define BLASTER_WIDTH 16.0f
#define BLASTER_LEN 2400.0f
#define BLASTER_SPAWN_MIN_DIST 180.0f       // never materializes on top of the player

// Teleports once it has taken this much cumulative damage since the last blink.
// The blink is TELEGRAPHED: the destination is picked immediately and shown as
// a ghost outline for the whole telegraph, then the boss (and its window)
// jumps. That warning is what makes the merge mechanic fair — a player
// standing in the boss's room has time to get out before the floor vanishes
// from under them (staying anyway is a crush, same as any void exposure).
#define SPIKER_TELEPORT_DAMAGE 12.0f
#define SPIKER_TELEPORT_SHAKE 5.0f
#define SPIKER_TP_TELEGRAPH_TICKS (int)(2.0f * SIM_HZ)

enum { SPIKER_TP_NONE = 0, SPIKER_TP_TELEGRAPH = 1 };

// The boss carries its own window. It teleports anywhere on screen; you reach
// it by pushing YOUR walls out until the two rects overlap. When they overlap
// the two windows MERGE into one playable space: the border opens where they
// cross, the player can walk through into the boss's room, and the boss is
// vulnerable. Fixed size, never shrinks, can't be pushed.
#define BOSS_WINDOW_W 300.0f
#define BOSS_WINDOW_H 220.0f
#define BOSS_WINDOW_SCREEN_MARGIN 12.0f     // its rect always stays fully on screen

#define ENEMY_HIT_FLASH_TICKS 1
#define ENEMY_SPAWN_MARGIN 24.0f  // spawned just outside the current window rect

#define SPAWN_QUEUE_CAP 64
#define SPAWN_INTERVAL_TICKS        (int)(0.5f * SIM_HZ)
#define SPAWN_INTERVAL_JITTER_TICKS (int)(0.3f * SIM_HZ)

// upgrades: PRD §8.3a
#define UPGRADE_COUNT 6
#define UPGRADE_STATE_TICKS (int)(3.0f * SIM_HZ)
// PRD says Wall Punch is "+2 push_per_shot", which was +33% against the old
// 6 px base. Scaled to hold that relative weight against the new 16 px base
// (+2 would be a 12.5% dud). A divergence from the PRD's literal number,
// kept faithful to its intent.
#define UPGRADE_WALL_PUNCH_ADD 5.0f

// Aim angle is quantized to 1/65536 of a turn (~0.0055 deg — imperceptible).
// This happens INSIDE the sim, not at the replay file boundary: aim_dx/aim_dy
// are derived only from the quantized value, so a live run and its replay
// compute bit-identical floats by construction. Quantizing only on write
// would make playback diverge from the run it recorded.
#define AIM_Q_STEPS 65536.0f
#define SIM_TAU 6.28318530717958647692f

typedef struct {
    uint32_t keys;              // current folded key bitmask, latest-wins within a tick
    float    mouse_x, mouse_y;  // internal 1280x720 space (live input)
    bool     mouse_down;
    uint16_t aim_q;             // replay playback: aim angle, used iff use_aim_q
    bool     use_aim_q;
} InputState;

// Per-ship state (PRD §8.2, now x2 for co-op). Index 0 is always the host/
// single-player ship; index 1 exists only when player_count == 2.
typedef struct {
    float    x, y;
    uint16_t aim_q;
    float    aim_dx, aim_dy;
    float    move_dx, move_dy;  // last nonzero movement direction; keyboard-shoot fallback
    int      shoot_cooldown_ticks;
    int      invuln_ticks;
    float    hit_flash;         // this ship's own hit-white tint, decays for HUD/juice
} Player;

typedef struct {
    float    pos, vel, target;   // 1-D spring state
    float    push_amount;        // last-applied push magnitude (diminishing returns)
    uint64_t last_push_tick;     // 0 = never pushed
} WindowEdge;

typedef struct {
    float x, y, vx, vy;
    bool  exited;  // already triggered its one-time edge push for this shot
} PShot;

enum { SPIKER_LASER_IDLE = 0, SPIKER_LASER_TELEGRAPH = 1, SPIKER_LASER_FIRING = 2 };

// A blaster locks its angle at spawn and never re-aims — dodging means walking
// off the line it committed to.
typedef struct {
    float   x, y, angle;
    uint8_t state;        // SPIKER_LASER_TELEGRAPH / _FIRING
    int     timer_ticks;
    bool    active;
} Blaster;

typedef struct {
    float   x, y;
    float   hp;
    uint8_t type;             // ENEMY_*
    uint8_t dash_state;       // Circle only
    int     dash_timer_ticks; // Circle only
    float   dash_vx, dash_vy; // Circle only
    uint8_t latch_edge;       // Octagon only: EDGE_* it has latched onto
    bool    latched;          // Octagon only
    int     fire_timer_ticks; // Octagon aimed shot / Spiker radial wave
    // Spiker only
    uint8_t laser_state;          // SPIKER_LASER_*
    int     laser_timer_ticks;    // counts down within the current laser phase
    float   laser_angle;          // beam angle, locked at telegraph start
    int     spiker_wave_counter;  // attacks speed up as this climbs
    float   damage_since_blink;   // telegraphs a teleport once this crosses the threshold
    uint8_t tp_state;             // SPIKER_TP_*
    int     tp_timer_ticks;       // counts down through the teleport telegraph
    float   tp_dest_x, tp_dest_y; // destination, picked when the telegraph starts
    int     hit_flash_ticks;
} Enemy;

// Enemy bullets live ONLY inside the window: they despawn the instant they
// cross an edge into the void (PRD §8.1).
typedef struct {
    float   x, y, vx, vy;
    uint8_t age_ticks;  // saturates; drives the spawn-pop scale-in (§8.6)
} Bullet;

typedef struct { uint8_t type; } SpawnQueueItem;

typedef struct {
    uint64_t   tick;
    uint64_t   seed;
    Rng        rng;
    InputState input[2];       // index 0 = host/SP, index 1 = joiner (co-op only)
    uint32_t   prev_keys[2];   // input[i].keys as of the previous sim_step, for edge detection
    bool       prev_mouse_down[2];  // ditto, for click edge detection

    uint8_t state;  // SIM_STATE_*

    // --- front end: MODE_SELECT / WAITING_ROOM / COLOR_SELECT ---
    // Deterministic (derived from input alone), so it rides in Sim/SimSnapshot
    // like everything else the front end draws, and replays reproduce it.
    uint8_t player_count;       // 1 = single-player, 2 = co-op; decided at MODE_SELECT confirm
    uint8_t mode_sel_cursor;    // 0=SINGLE PLAYER 1=HOST GAME 2=JOIN GAME
    uint8_t net_role;           // NET_ROLE_* — confirmed choice; drives WAITING_ROOM and netplay_service
    uint8_t color_sel[2];       // each present player's picker cursor
    uint8_t color_confirmed[2]; // COLOR_UNCONFIRMED, else the confirmed palette index
    uint8_t player_color[2];    // confirmed ship colors — set once, survive into PLAY
    // M3 acceptance-test aid (PRD §10, "60 fps with 2048 test bullets
    // bouncing"): when set, step_bullets() bounces off the fixed screen
    // bounds instead of despawning outside the window, giving a sustained,
    // gameplay-independent entity-count load. Not reachable from normal
    // input; see sim_start_stress().
    bool stress_mode;

    WindowEdge edges[4];       // indexed by EDGE_*
    float      edge_flash[4];  // decays ~2 ticks after a push
    float      shake_x, shake_y;

    Player players[2];         // players[1] valid iff player_count == 2
    int   hitstop_ticks;       // shared: a hitstop freezes the whole arena, not one ship

    // upgrade-mutable balance (defaults from the *_DEFAULT constants above)
    float player_speed;
    float player_focus_speed;
    int   fire_cooldown_ticks_base;
    int   multishot_count;
    int   lives_max;
    float push_base;
    float shrink_rate;

    int      lives;
    uint32_t score;
    int      wave;

    PShot    pshots[MAX_PSHOTS];
    uint16_t pshot_count;

    Enemy    enemies[MAX_ENEMIES];
    uint16_t enemy_count;

    Bullet   bullets[MAX_BULLETS];
    uint16_t bullet_count;

    // Rebuilt from live Spikers/blasters each tick; mirrors into the snapshot.
    SnapBeam beams[MAX_BEAMS];
    uint16_t beam_count;

    Blaster blasters[MAX_BLASTERS];
    int     blaster_timer_ticks;

    int   spiker_appearances;   // drives HP scaling across boss fights
    bool  spiker_alive;         // shrink_rate x1.6 while true

    // Boss window: derived from the live Spiker's position each tick.
    float boss_win_x, boss_win_y;   // top-left
    bool  boss_win_active;
    bool  boss_vulnerable;          // true iff boss rect overlaps the player rect (windows merged)

    // Teleport-telegraph ghost, derived from the live Spiker each tick.
    bool  boss_tp_active;
    float boss_tp_x, boss_tp_y;     // destination (boss center)
    float boss_tp_progress;         // 0..1 through the telegraph

    SpawnQueueItem spawn_queue[SPAWN_QUEUE_CAP];
    int spawn_queue_count;
    int spawn_queue_index;
    int spawn_timer_ticks;

    uint8_t upgrade_a, upgrade_b;
    int     upgrade_selected;  // 0 or 1
    int     upgrade_timer_ticks;
} Sim;

void sim_init(Sim* sim, uint64_t seed);
// Dev/demo aid: jump straight into PLAY at a given wave. Wave 5, 10, ... are
// Spiker fights, so this is how you rehearse the boss without clearing four
// waves first. Not reachable from normal input.
void sim_start_at_wave(Sim* sim, int wave);
// Dev/demo aid (M3 acceptance test, PRD §10): starts a normal run, then
// overrides the bullet pool with n bullets scattered across the internal
// resolution with random velocities, bouncing off the screen bounds forever
// (see stress_mode). n is clamped to MAX_BULLETS. Not reachable from normal
// input.
void sim_start_stress(Sim* sim, int n);
// Drains `ring` into sim->input[local_idx] (the LOCAL machine's ship — 0 for
// single-player/host, 1 for a joiner once past WAITING_ROOM; see main.c).
void sim_consume_input(Sim* sim, InputRing* ring, int local_idx);

// netplay.c's one deliberate reach into sim.c's state machine: called once,
// on both machines independently, the instant the handshake completes (see
// netplay.h). Equivalent to sim_init(seed) followed by landing exactly where
// MODE_SELECT's SINGLE PLAYER branch would — at COLOR_SELECT with a fresh,
// unconfirmed picker — except player_count/net_role are set for co-op. Both
// sides call this at their own "tick 0", which is what keeps lockstep tick
// numbering aligned without a shared clock.
void sim_begin_multiplayer_run(Sim* sim, uint64_t seed, uint8_t net_role);

void sim_step(Sim* sim);
void sim_publish(const Sim* sim, SnapshotBuffer* sb);

// FNV-1a over the whole Sim struct — the determinism check (§9.1 replaycheck).
uint64_t sim_hash(const Sim* sim);

#endif // SIM_H
