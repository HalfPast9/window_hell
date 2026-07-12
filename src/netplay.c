// netplay.c — see netplay.h. Pure BSD sockets, modeled on handtrack.c
// (nonblocking UDP, builds unchanged on Linux/QNX; QNX links -lsocket).
#define _POSIX_C_SOURCE 200809L
#include "netplay.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define NET_MAGIC   0x31504D57u  // "WMP1" LE, same convention as HT_MAGIC/REPLAY_MAGIC
#define NET_VERSION 1u

enum { NET_PKT_JOIN = 1, NET_PKT_START = 2, NET_PKT_INPUT = 3 };

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t reserved;
} NetHdr;

typedef struct {
    NetHdr   hdr;
    uint64_t seed;
    uint32_t input_delay_ticks;
} NetPktStart;

typedef struct {
    uint32_t keys;
    uint8_t  flags;      // bit0 mouse_down, bit1 use_aim_q
    uint8_t  pad[3];
    uint16_t aim_q;
    uint16_t pad2;
    float    mouse_x, mouse_y;
} NetInputEntry;

typedef struct {
    NetHdr         hdr;
    uint32_t       base_tick;   // tick of entries[0]
    uint32_t       count;
    uint32_t       hash_tick;   // 0 = no checkpoint attached to this packet
    uint64_t       hash_value;
    NetInputEntry  entries[NET_REDUNDANCY];
} NetPktInput;

_Static_assert(sizeof(struct sockaddr_in) <= sizeof(((NetplayState*)0)->peer_addr),
               "peer_addr storage too small for sockaddr_in on this platform");

// No #pragma pack here (unlike some wire formats elsewhere in this codebase):
// every field is explicitly padded to its own natural alignment, so a plain
// memcpy-in/memcpy-out round-trips identically on x86_64 and aarch64le — the
// two ABIs this game ships on (both little-endian, both align uint64_t to 8).
// These asserts catch it at compile time if that ever stops being true.
_Static_assert(sizeof(NetHdr) == 8, "NetHdr layout drifted");
_Static_assert(sizeof(NetPktStart) == 24, "NetPktStart layout drifted");
_Static_assert(sizeof(NetInputEntry) == 20, "NetInputEntry layout drifted");
_Static_assert(sizeof(NetPktInput) == 192, "NetPktInput layout drifted");

static uint64_t now_ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void netplay_init(NetplayState* net, uint8_t role, const char* target_ip, uint16_t port) {
    memset(net, 0, sizeof(*net));
    net->fd = -1;
    net->role = role;
    net->port = port ? port : NET_PORT_DEFAULT;
    net->local_player_idx = -1;

    if (role == NET_ROLE_NONE) return;  // single-player: leave fd == -1, fully inert

    bool dbg = getenv("WH_NETDEBUG") != NULL;
    if (dbg) { fprintf(stderr, "netdebug: netplay_init entry, role=%d\n", role); fflush(stderr); }

    if (role == NET_ROLE_JOIN) {
        const char* ip = (target_ip && target_ip[0]) ? target_ip : NULL;
        if (!ip) ip = getenv("WH_MP_HOST_IP");
        if (!ip) ip = NET_HOST_IP_DEFAULT;
        snprintf(net->target_ip, sizeof(net->target_ip), "%s", ip);
    }

    if (dbg) { fprintf(stderr, "netdebug: calling socket()\n"); fflush(stderr); }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dbg) { fprintf(stderr, "netdebug: socket() returned fd=%d\n", fd); fflush(stderr); }
    if (fd < 0) {
        fprintf(stderr, "netplay: socket() failed: %s\n", strerror(errno));
        return;
    }
    if (dbg) { fprintf(stderr, "netdebug: calling fcntl(F_GETFL)\n"); fflush(stderr); }
    int flags = fcntl(fd, F_GETFL, 0);
    if (dbg) { fprintf(stderr, "netdebug: fcntl(F_GETFL) returned %d, calling fcntl(F_SETFL)\n", flags); fflush(stderr); }
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "netplay: fcntl(O_NONBLOCK) failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    if (dbg) { fprintf(stderr, "netdebug: fcntl(F_SETFL) OK\n"); fflush(stderr); }

    // HOST binds the well-known port so a joiner can find it by address
    // alone. JOIN binds an EPHEMERAL port (0) — it only ever talks to the one
    // host address/port it already knows, and this is what lets host+join
    // run as two processes on the same machine for the dev-box loopback
    // test (two sockets can't both bind the same fixed UDP port).
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = (role == NET_ROLE_HOST) ? htons(net->port) : 0;
    if (dbg) { fprintf(stderr, "netdebug: calling bind() on port %u\n", (unsigned)ntohs(addr.sin_port)); fflush(stderr); }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "netplay: bind() failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    if (dbg) { fprintf(stderr, "netdebug: bind() OK\n"); fflush(stderr); }

    if (role == NET_ROLE_JOIN) {
        struct sockaddr_in peer = {0};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(net->port);
        if (inet_pton(AF_INET, net->target_ip, &peer.sin_addr) != 1) {
            fprintf(stderr, "netplay: bad target IP '%s'\n", net->target_ip);
            close(fd);
            return;
        }
        memcpy(net->peer_addr, &peer, sizeof(peer));
        net->peer_addr_known = true;  // known target, not yet confirmed live (see JOIN retries)
    }

    net->fd = fd;
    printf("netplay: %s on UDP :%u%s%s\n",
            role == NET_ROLE_HOST ? "hosting" : "joining",
            (unsigned)net->port,
            role == NET_ROLE_JOIN ? " -> " : "",
            role == NET_ROLE_JOIN ? net->target_ip : "");
}

void netplay_shutdown(NetplayState* net) {
    if (net->fd >= 0) {
        close(net->fd);
        net->fd = -1;
    }
}

static void send_raw(NetplayState* net, const void* data, size_t len) {
    if (net->fd < 0 || !net->peer_addr_known) return;
    struct sockaddr_in addr;
    memcpy(&addr, net->peer_addr, sizeof(addr));
    sendto(net->fd, data, len, 0, (const struct sockaddr*)&addr, sizeof(addr));
}

// sim_hash() covers the raw struct, and net_role (HOST vs JOIN) is the one
// field each side legitimately sets to a different value on purpose — every
// other byte, including both players' full input history, is supposed to be
// bit-identical by construction. Excluding it here is what keeps this check
// honest instead of permanently latching a false DESYNC on the HUD.
static uint64_t gameplay_hash(const Sim* sim) {
    Sim tmp = *sim;
    tmp.net_role = 0;
    return sim_hash(&tmp);
}

static void maybe_record_own_hash(NetplayState* net, const Sim* sim) {
    if (sim->tick == 0 || sim->tick % NET_HASH_INTERVAL_TICKS != 0) return;
    uint64_t h = gameplay_hash(sim);
    net->own_hash_tick = sim->tick;
    net->own_hash_value = h;
    if (net->pending_peer_hash_valid && net->pending_peer_hash_tick == sim->tick) {
        if (net->pending_peer_hash_value != h) net->desync = true;
        net->pending_peer_hash_valid = false;
    }
}

static void apply_peer_hash(NetplayState* net, uint32_t tick, uint64_t hash) {
    if (tick == 0) return;
    if (tick == net->own_hash_tick) {
        if (hash != net->own_hash_value) net->desync = true;
    } else if (tick > net->own_hash_tick) {
        net->pending_peer_hash_tick = tick;
        net->pending_peer_hash_value = hash;
        net->pending_peer_hash_valid = true;
    }
    // tick < own_hash_tick: stale, we've already passed it — not chased.
}

// The very first local poll after establishing targets tick (1 + delay) —
// nothing ever pushes ticks 1..delay. Rather than exchange them over the
// wire, both sides independently pre-fill both rings for that bootstrap
// range with the SAME neutral default (matching sim_init's own idle-input
// defaults): a known protocol convention, not data, so it needs no packet.
static void seed_bootstrap_ticks(NetplayState* net) {
    InputState neutral = {0};
    neutral.mouse_x = INTERNAL_W * 0.5f;
    neutral.mouse_y = INTERNAL_H * 0.5f;
    for (uint64_t t = 1; t <= (uint64_t)net->input_delay_ticks; t++) {
        uint32_t slot = (uint32_t)(t & (NET_RING_CAP - 1));
        net->local_ring[slot] = neutral;  net->local_tick[slot] = (uint32_t)t;  net->local_have[slot] = true;
        net->remote_ring[slot] = neutral; net->remote_tick[slot] = (uint32_t)t; net->remote_have[slot] = true;
    }
}

static void handle_join(NetplayState* net, Sim* sim, const struct sockaddr_in* from) {
    if (net->role != NET_ROLE_HOST) return;

    if (!net->peer_addr_known) {
        memcpy(net->peer_addr, from, sizeof(*from));
        net->peer_addr_known = true;
        // Seed picked once, from wall-clock jitter — legitimately
        // non-deterministic (host-authoritative), unrelated to the sim's own
        // seeded PRNG.
        net->agreed_seed = now_ns_monotonic() ^ ((uint64_t)getpid() << 32);
    }

    NetPktStart resp = {
        .hdr = { .magic = NET_MAGIC, .version = NET_VERSION, .type = NET_PKT_START, .reserved = 0 },
        .seed = net->agreed_seed,
        .input_delay_ticks = (uint32_t)NET_INPUT_DELAY_DEFAULT,
    };
    send_raw(net, &resp, sizeof(resp));

    if (!net->established) {
        net->established = true;
        net->local_player_idx = 0;
        net->input_delay_ticks = NET_INPUT_DELAY_DEFAULT;
        net->next_local_push_tick = 1 + (uint64_t)net->input_delay_ticks;
        seed_bootstrap_ticks(net);
        sim_begin_multiplayer_run(sim, net->agreed_seed, NET_ROLE_HOST);
    }
}

static void handle_start(NetplayState* net, Sim* sim, const NetPktStart* pkt) {
    if (net->role != NET_ROLE_JOIN || net->established) return;  // idempotent: ignore duplicates/replays

    net->agreed_seed = pkt->seed;
    net->input_delay_ticks = (int)pkt->input_delay_ticks;
    net->established = true;
    net->local_player_idx = 1;
    net->next_local_push_tick = 1 + (uint64_t)net->input_delay_ticks;
    seed_bootstrap_ticks(net);
    sim_begin_multiplayer_run(sim, net->agreed_seed, NET_ROLE_JOIN);
}

static void handle_input(NetplayState* net, const NetPktInput* pkt) {
    if (!net->established) return;

    uint32_t count = pkt->count > NET_REDUNDANCY ? NET_REDUNDANCY : pkt->count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t t = pkt->base_tick + i;
        uint32_t slot = t & (NET_RING_CAP - 1);
        const NetInputEntry* e = &pkt->entries[i];
        InputState* dst = &net->remote_ring[slot];
        dst->keys = e->keys;
        dst->mouse_x = e->mouse_x;
        dst->mouse_y = e->mouse_y;
        dst->mouse_down = (e->flags & 1u) != 0;
        dst->aim_q = e->aim_q;
        dst->use_aim_q = (e->flags & 2u) != 0;
        net->remote_tick[slot] = t;
        net->remote_have[slot] = true;
    }

    apply_peer_hash(net, pkt->hash_tick, pkt->hash_value);
}

static void drain_recv(NetplayState* net, Sim* sim, uint64_t now_ns) {
    if (net->fd < 0) return;

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(net->fd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "netplay: recvfrom() failed: %s\n", strerror(errno));
            }
            break;
        }
        if ((size_t)n < sizeof(NetHdr)) continue;

        NetHdr hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != NET_MAGIC || hdr.version != NET_VERSION) continue;

        net->last_recv_ns = now_ns;
        net->peer_lost = false;

        switch (hdr.type) {
            case NET_PKT_JOIN:
                handle_join(net, sim, &from);
                break;
            case NET_PKT_START:
                if ((size_t)n == sizeof(NetPktStart)) {
                    NetPktStart pkt; memcpy(&pkt, buf, sizeof(pkt));
                    handle_start(net, sim, &pkt);
                }
                break;
            case NET_PKT_INPUT:
                if ((size_t)n == sizeof(NetPktInput)) {
                    NetPktInput pkt; memcpy(&pkt, buf, sizeof(pkt));
                    handle_input(net, &pkt);
                }
                break;
            default: break;
        }
    }

    if (net->established && net->last_recv_ns != 0 &&
        (now_ns - net->last_recv_ns) > NET_PEER_TIMEOUT_NS) {
        net->peer_lost = true;
    }
}

static void drive_handshake(NetplayState* net, uint64_t now_ns) {
    if (net->fd < 0 || net->established) return;
    if (now_ns - net->last_send_ns < NET_RETRY_INTERVAL_NS) return;
    net->last_send_ns = now_ns;
    net->handshake_attempts++;

    if (net->role == NET_ROLE_JOIN) {
        NetHdr pkt = { .magic = NET_MAGIC, .version = NET_VERSION, .type = NET_PKT_JOIN, .reserved = 0 };
        send_raw(net, &pkt, sizeof(pkt));
    }
    // HOST has nothing to proactively send — it replies to JOIN packets
    // (handle_join), including re-sending START to a peer that already
    // retried once (covers a lost first START).
}

static void push_local_input(NetplayState* net, InputState local_in, uint64_t sim_tick) {
    // Guard against overrunning the ring during a long stall: stop advancing
    // the push cursor once it's run far enough ahead of the last steppable
    // tick that continuing would overwrite not-yet-consumed slots. Harmless
    // — NET_PEER_TIMEOUT_NS will flag PEER LOST well before this matters in
    // practice; this just keeps the ring itself always self-consistent.
    if (net->next_local_push_tick >= sim_tick + 1 + (uint64_t)(NET_RING_CAP - 8)) return;

    uint32_t slot = (uint32_t)(net->next_local_push_tick & (NET_RING_CAP - 1));
    net->local_ring[slot] = local_in;
    net->local_tick[slot] = (uint32_t)net->next_local_push_tick;
    net->local_have[slot] = true;
    net->next_local_push_tick++;
}

static void send_local_input(NetplayState* net) {
    uint64_t end = net->next_local_push_tick;  // one past the last pushed tick
    uint32_t count = end >= NET_REDUNDANCY ? NET_REDUNDANCY : (uint32_t)end;
    if (count == 0) return;
    uint64_t base = end - count;

    NetPktInput pkt = {
        .hdr = { .magic = NET_MAGIC, .version = NET_VERSION, .type = NET_PKT_INPUT, .reserved = 0 },
        .base_tick = (uint32_t)base,
        .count = count,
        .hash_tick = (uint32_t)net->own_hash_tick,
        .hash_value = net->own_hash_value,
    };
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (uint32_t)((base + i) & (NET_RING_CAP - 1));
        const InputState* s = &net->local_ring[slot];
        NetInputEntry* e = &pkt.entries[i];
        e->keys = s->keys;
        e->flags = (uint8_t)((s->mouse_down ? 1u : 0u) | (s->use_aim_q ? 2u : 0u));
        e->pad[0] = e->pad[1] = e->pad[2] = 0;
        e->aim_q = s->aim_q;
        e->pad2 = 0;
        e->mouse_x = s->mouse_x;
        e->mouse_y = s->mouse_y;
    }
    send_raw(net, &pkt, sizeof(pkt));
}

bool netplay_service(NetplayState* net, Sim* sim, InputState local_in, uint64_t now_ns) {
    if (net->role == NET_ROLE_NONE || net->fd < 0) return false;

    drain_recv(net, sim, now_ns);

    if (!net->established) {
        drive_handshake(net, now_ns);
        return false;  // never steps the sim during the handshake
    }

    push_local_input(net, local_in, sim->tick);
    send_local_input(net);
    maybe_record_own_hash(net, sim);

    uint64_t want = sim->tick + 1;
    uint32_t local_slot = (uint32_t)(want & (NET_RING_CAP - 1));
    uint32_t remote_slot = local_slot;

    bool local_ready = net->local_have[local_slot] && net->local_tick[local_slot] == (uint32_t)want;
    bool remote_ready = net->remote_have[remote_slot] && net->remote_tick[remote_slot] == (uint32_t)want;

    if (!local_ready || !remote_ready) {
        net->stall_count++;
        return false;
    }

    int local_idx = net->local_player_idx;
    int remote_idx = 1 - local_idx;
    sim->input[local_idx] = net->local_ring[local_slot];
    sim->input[remote_idx] = net->remote_ring[remote_slot];
    return true;
}

// --- status board: sim thread -> render thread, same seqlock idiom as
// metrics.c's SimMetricsBoard, just for netplay diagnostics. ---

void netplay_status_init(NetplayStatusBoard* board) {
    atomic_store_explicit(&board->seq, 0, memory_order_relaxed);
    memset(&board->data, 0, sizeof(board->data));
}

void netplay_publish_status(const NetplayState* net, NetplayStatusBoard* board) {
    NetplayStatus s = {
        .role = net->role,
        .established = net->established,
        .local_player_idx = net->established ? (uint8_t)net->local_player_idx : 0,
        .peer_lost = net->peer_lost,
        .desync = net->desync,
        .stall_count = net->stall_count,
        .handshake_attempts = net->handshake_attempts,
        .input_delay_ticks = net->input_delay_ticks,
    };
    uint32_t seq = atomic_load_explicit(&board->seq, memory_order_relaxed);
    atomic_store_explicit(&board->seq, seq + 1, memory_order_release);  // odd: writer in progress
    board->data = s;
    atomic_store_explicit(&board->seq, seq + 2, memory_order_release);  // even: write visible
}

void netplay_read_status(const NetplayStatusBoard* board, NetplayStatus* out) {
    for (;;) {
        uint32_t seq1 = atomic_load_explicit(&board->seq, memory_order_acquire);
        if (seq1 & 1u) continue;  // writer in progress, retry
        *out = board->data;
        uint32_t seq2 = atomic_load_explicit(&board->seq, memory_order_acquire);
        if (seq1 == seq2) return;  // consistent snapshot
    }
}
