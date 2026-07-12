// netplay.h — deterministic-lockstep co-op over UDP (direct ethernet cable).
//
// The sim is already a pure function of (seed, per-tick dual input) —
// verified bit-identical across Linux/QNX-x86/QNX-aarch64 by replaycheck.
// Two identical binaries fed identical input therefore produce identical
// states, so multiplayer is "both machines run the full sim; exchange only
// each tick's tiny input" — a replay streamed live instead of read from disk.
//
// Modeled on handtrack.c's nonblocking-UDP style. Unlike handtrack (which
// only ever ADDS to input, best-effort, and is fine to drop packets), netplay
// is load-bearing for determinism: a tick may only be stepped once BOTH
// sides' input for it is known, on both machines, in the same slot order.
//
// Two phases:
//  1. Handshake (while sim->state == SIM_STATE_WAITING_ROOM): unreliable
//     retry of a tiny JOIN/START exchange to agree on (seed, input_delay,
//     who's player 0 vs 1). On success, netplay_service() itself forces
//     sim_init(seed) and jumps sim->state to SIM_STATE_COLOR_SELECT — both
//     sides do this at their own "tick 0", so lockstep ticks line up from
//     the very first tick without needing a shared clock.
//  2. Lockstep (COLOR_SELECT..DEAD): the caller's freshly-polled local input
//     is queued for delivery `input_delay_ticks` in the future (hides the
//     cable's <1ms RTT completely) and broadcast every tick with the last
//     few ticks repeated for loss tolerance (no ACKs needed). A tick is
//     stepped only once both the (delayed) local and the received remote
//     input for it are in hand; otherwise the caller sees a stall.
#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "sim.h"  // InputState, Sim, NET_ROLE_*

#define NET_PORT_DEFAULT 47900

// Direct point-to-point ethernet link (Phase 6 bring-up): the host's static
// address. Override with WH_MP_HOST_IP (dev-box loopback testing) or
// --mp-join <ip>.
#define NET_HOST_IP_DEFAULT "192.168.100.1"

#define NET_INPUT_DELAY_DEFAULT 4  // ticks (~16.7ms @ 240Hz) — hides the cable's <1ms RTT
#define NET_RING_CAP 64            // must be a power of two; indexed by tick & (CAP-1)
#define NET_REDUNDANCY 8           // ticks of input repeated per packet (loss tolerance, no ACKs)
#define NET_HASH_INTERVAL_TICKS 240  // one desync check per second of sim time
#define NET_PEER_TIMEOUT_NS (2000ull * 1000000ull)  // 2s silence -> PEER LOST
#define NET_RETRY_INTERVAL_NS (250ull * 1000000ull)  // handshake retry cadence

typedef struct {
    // --- configuration (set once by netplay_init) ---
    uint8_t  role;             // NET_ROLE_HOST or NET_ROLE_JOIN
    uint16_t port;
    char     target_ip[64];    // JOIN only

    // --- socket ---
    int  fd;                   // -1 = disabled/not yet bound

    // --- handshake state ---
    bool     peer_addr_known;
    uint8_t  peer_addr[16];    // opaque storage for struct sockaddr_in (avoids leaking socket headers here)
    uint64_t agreed_seed;
    int      input_delay_ticks;
    bool     established;      // handshake complete; sim_init+jump already done
    int      local_player_idx; // 0 (host) or 1 (join); valid once established
    uint64_t last_send_ns;
    uint32_t handshake_attempts;

    // --- lockstep buffers, indexed by tick & (NET_RING_CAP-1). Each slot
    // also carries the actual tick it represents, so a slot recycled from 64
    // ticks ago can never be mistaken for fresh data (`have` alone isn't
    // enough once the ring wraps). ---
    InputState local_ring[NET_RING_CAP];
    uint32_t   local_tick[NET_RING_CAP];
    bool       local_have[NET_RING_CAP];
    InputState remote_ring[NET_RING_CAP];
    uint32_t   remote_tick[NET_RING_CAP];
    bool       remote_have[NET_RING_CAP];
    uint64_t   next_local_push_tick;  // next tick a freshly-polled input will be tagged for

    // --- desync check: every NET_HASH_INTERVAL_TICKS, each side stamps its
    // sim_hash() into outbound packets; the peer compares it against its own
    // hash for that same tick once it gets there too. Single-slot (not a
    // ring) — packets repeat the latest checkpoint continuously, so by the
    // time either side reaches a given checkpoint tick the peer's value for
    // it has almost always already arrived. ---
    uint64_t own_hash_tick, own_hash_value;
    uint64_t pending_peer_hash_tick, pending_peer_hash_value;
    bool     pending_peer_hash_valid;

    // --- diagnostics (local-only; deliberately NOT part of deterministic Sim
    // state — these depend on real time / packet loss, so they ride a
    // separate seqlock-style board main.c's render thread polls for the
    // WAITING_ROOM screen and the NET HUD line, exactly like SimMetricsBoard) ---
    uint64_t stall_count;
    uint64_t last_recv_ns;
    bool     peer_lost;
    bool     desync;
} NetplayState;

// NetplayState lives on the sim thread. The render thread needs a handful of
// its fields (to draw WAITING_ROOM and the NET HUD line) without touching
// sockets or blocking either thread — same seqlock idiom as SimMetricsBoard
// (metrics.h), just for netplay diagnostics instead of tick timing.
typedef struct {
    uint8_t  role;
    bool     established;
    uint8_t  local_player_idx;  // 0/1; meaningless (0) until established
    bool     peer_lost;
    bool     desync;
    uint64_t stall_count;
    uint32_t handshake_attempts;
    int      input_delay_ticks;
} NetplayStatus;

typedef struct {
    _Atomic uint32_t seq;
    NetplayStatus     data;
} NetplayStatusBoard;

void netplay_status_init(NetplayStatusBoard* board);
// sim thread only; cheap, call once per netplay_service call.
void netplay_publish_status(const NetplayState* net, NetplayStatusBoard* board);
// render thread only.
void netplay_read_status(const NetplayStatusBoard* board, NetplayStatus* out);

// role = NET_ROLE_HOST or NET_ROLE_JOIN. target_ip is only read for JOIN
// (pass NULL/"" to fall back to WH_MP_HOST_IP or NET_HOST_IP_DEFAULT). Binds
// a nonblocking UDP socket; failure is non-fatal (net->fd stays -1 and
// netplay_service becomes a no-op that never establishes — the WAITING_ROOM
// screen just never advances, and R still works to back out).
void netplay_init(NetplayState* net, uint8_t role, const char* target_ip, uint16_t port);
void netplay_shutdown(NetplayState* net);

// Call once per sim-thread iteration, always (cheap no-op when net->fd < 0 or
// role is NONE). `local_in` is this tick's freshly-polled LOCAL input (raw,
// not yet delay-buffered — netplay_service owns that).
//
// While !established: drives the handshake; may call sim_init() and set
// sim->state = SIM_STATE_COLOR_SELECT as a side effect (both sides do this
// independently, at their own "tick 0" — see the header comment). Returns
// false (never steps the sim itself during the handshake).
//
// Once established: queues local_in for delivery, exchanges packets, and —
// if both the delayed-local and remote input for tick (sim->tick+1) are
// available — writes them into sim->input[0]/sim->input[1] and returns true
// (the caller should call sim_step() this iteration). Returns false on a
// stall (increments net->stall_count); the caller should skip sim_step and
// try again next iteration without falling further behind on wall-clock.
bool netplay_service(NetplayState* net, Sim* sim, InputState local_in, uint64_t now_ns);

#endif  // NETPLAY_H
