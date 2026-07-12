// main.c — thread startup, main loop wiring (PRD §6.1/§6.2).
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <GLES2/gl2.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "handtrack.h"
#include "hud.h"
#include "input_ring.h"
#include "metrics.h"
#include "netplay.h"
#include "palette.h"
#include "render.h"
#include "replay.h"
#include "sim.h"
#include "snapshot.h"

static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_sigint(int signum) {
    (void)signum;
    g_shutdown_requested = 1;
}

typedef struct {
    Sim*                 sim;
    InputRing*           input_ring;
    SnapshotBuffer*       snapshot_buf;
    SimMetricsBoard*      metrics_board;
    ReplayRecorder*       recorder;
    ReplayPlayer*         player;
    _Atomic bool*         playback_requested;  // render thread -> sim thread
    NetplayState*         net;
    NetplayStatusBoard*   net_status;
    const char*           mp_target_ip;  // NULL = use netplay.h's default/WH_MP_HOST_IP
    uint16_t              mp_port;
} SimThreadArgs;

// Packs the sim's trigger state into the replay log's single uint16.
// Single-player only — replay recording is disabled in multiplayer (§ main()).
static uint16_t pack_replay_keys(const Sim* sim) {
    uint16_t k = (uint16_t)(sim->input[0].keys & REPLAY_KEY_MASK);
    if (sim->input[0].mouse_down) k |= REPLAY_KEY_MOUSE_FIRE;
    return k;
}

// Drains `ring`, folding every pending frame into a scratch InputState
// (starting from `fallback` so an empty drain is a no-op) rather than writing
// straight into a Sim field — used once established, when the local player's
// input must go through netplay's input-delay buffer instead of landing in
// sim->input[] immediately (see netplay.h).
static InputState drain_ring_into(InputRing* ring, InputState fallback) {
    InputFrame f;
    InputState latest = fallback;
    while (input_ring_pop(ring, &f)) {
        latest.keys = f.keys;
        latest.mouse_x = f.mouse_x;
        latest.mouse_y = f.mouse_y;
        latest.mouse_down = f.mouse_down;
        latest.aim_q = f.aim_q;
        latest.use_aim_q = f.use_aim_q;
    }
    return latest;
}

static void* sim_thread_main(void* arg_ptr) {
    SimThreadArgs* args = (SimThreadArgs*)arg_ptr;
    Sim* sim = args->sim;
    NetplayState* net = args->net;

#ifdef __QNX__
    struct sched_param sp = { .sched_priority = 60 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "sim_thread: pthread_setschedparam(SCHED_FIFO) failed "
                         "(permissions?) — continuing best-effort\n");
    }
#endif

    const uint64_t TICK_NS = 1000000000ull / SIM_HZ;
    uint64_t next = plat_time_ns() + TICK_NS;
    uint64_t scheduled_time = next;
    uint32_t prev_keys = 0;
    bool playing = false;

    while (!g_shutdown_requested) {
        // Render thread asks for playback; the sim thread owns the transition
        // so the seed reset and the first replayed tick land together.
        if (atomic_exchange_explicit(args->playback_requested, false, memory_order_acq_rel)) {
            sim_init(sim, args->player->seed);
            playing = args->player->playing;
        }

        // (Re)create the netplay socket the instant the front end asks for a
        // new role (MODE_SELECT -> WAITING_ROOM), and tear it down the
        // instant restart takes us back to MODE_SELECT — this is what lets
        // HOST/JOIN be picked, cancelled, and picked again without
        // restarting the process. See netplay.h.
        if (sim->state == SIM_STATE_WAITING_ROOM && net->role != sim->net_role) {
            if (getenv("WH_NETDEBUG")) { fprintf(stderr, "netdebug: main.c calling netplay_shutdown+init\n"); fflush(stderr); }
            netplay_shutdown(net);
            netplay_init(net, sim->net_role, args->mp_target_ip, args->mp_port);
            if (getenv("WH_NETDEBUG")) { fprintf(stderr, "netdebug: main.c netplay_init returned, fd=%d\n", net->fd); fflush(stderr); }
        } else if (sim->state == SIM_STATE_MODE_SELECT && net->role != NET_ROLE_NONE) {
            netplay_shutdown(net);
            netplay_init(net, NET_ROLE_NONE, NULL, 0);
        }

        bool net_active = (net->role != NET_ROLE_NONE);
        bool was_established = net_active && net->established;

        if (net_active && !was_established) {
            // Drive the handshake. Deliberately NOT gating sim_step on this —
            // only actual dual-player gameplay (COLOR_SELECT onward) needs
            // both sides' input; WAITING_ROOM must keep ticking at the normal
            // rate on local input alone so R-to-cancel and the connecting
            // animation both keep working.
            netplay_service(net, sim, sim->input[0], plat_time_ns());
            netplay_publish_status(net, args->net_status);
        }

        bool now_lockstep = net_active && net->established;
        bool stepped = false;
        uint64_t t0 = 0, t1 = 0;

        if (!now_lockstep) {
            // Single-player, OR multiplayer still in the handshake (not
            // established yet, including the case where it just became
            // established above THIS iteration — skip stepping once here so
            // the very first lockstep tick is cleanly gated next iteration
            // instead of running on stale single-player-style local input).
            if (!(net_active && was_established != net->established)) {
                sim_consume_input(sim, args->input_ring, 0);

                if (!net_active && playing) {
                    uint16_t keys16, aim_q;
                    if (replay_input_for_tick(args->player, sim->tick + 1, &keys16, &aim_q)) {
                        sim->input[0].keys = (uint32_t)(keys16 & REPLAY_KEY_MASK);
                        sim->input[0].mouse_down = (keys16 & REPLAY_KEY_MOUSE_FIRE) != 0;
                        sim->input[0].aim_q = aim_q;
                        sim->input[0].use_aim_q = true;  // aim from the log, not a cursor
                    } else {
                        playing = false;
                        sim->input[0].use_aim_q = false;
                    }
                }

                uint32_t keys = sim->input[0].keys;
                if ((keys & KEY_RESTART) && !(prev_keys & KEY_RESTART)) {
                    metrics_reset_worst(args->metrics_board);
                }
                prev_keys = keys;

                t0 = plat_time_ns();
                sim_step(sim);
                t1 = plat_time_ns();
                stepped = true;

                // Record AFTER the step: aim_q is resolved inside sim_step
                // (step_aim), so this captures the value the tick actually
                // ran with. Single-player only (§ main(): F2/F3 disabled in MP).
                if (!net_active && !playing) {
                    replay_record_tick(args->recorder, sim->tick,
                                        pack_replay_keys(sim), sim->players[0].aim_q);
                }
            }
        } else {
            // Steady-state lockstep: local input goes through the delay
            // buffer, not straight into sim->input[] (see netplay.h).
            InputState local_in = drain_ring_into(args->input_ring, sim->input[net->local_player_idx]);
            uint64_t now = plat_time_ns();
            t0 = now;
            bool can_step = netplay_service(net, sim, local_in, now);
            netplay_publish_status(net, args->net_status);
            if (can_step) {
                sim_step(sim);
                t1 = plat_time_ns();
                stepped = true;
            }
        }

        if (stepped) {
            uint64_t jitter_ns = (t0 > scheduled_time) ? (t0 - scheduled_time) : 0;
            metrics_record_tick(args->metrics_board, sim->tick, jitter_ns, t1 - t0);
            sim_publish(sim, args->snapshot_buf);
            if (t1 > next) {
                metrics_record_overrun(args->metrics_board);
                next = t1;  // late: run the next tick immediately, never skip sim steps silently
            }
        }

        plat_sleep_until_ns(next);
        scheduled_time = next;
        next += TICK_NS;
    }
    return NULL;
}

int main(int argc, char** argv) {
    uint64_t seed = 0;
    const char* replay_path = NULL;
    int start_wave = 0;  // 0 = normal (start at the front end)
    int stress_n = 0;    // 0 = disabled; see PRD §10 M3 acceptance test
    int handtrack_port = 0;  // 0 = disabled
    uint8_t cli_mp_role = NET_ROLE_NONE;
    const char* cli_mp_target_ip = NULL;
    uint16_t cli_mp_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--wave") == 0 && i + 1 < argc) {
            start_wave = atoi(argv[++i]);  // dev/demo: jump to a wave (5, 10, ... = boss)
        } else if (strcmp(argv[i], "--stress") == 0 && i + 1 < argc) {
            stress_n = atoi(argv[++i]);  // dev/demo: N bouncing bullets, perf stress test
        } else if (strcmp(argv[i], "--handtrack") == 0) {
            handtrack_port = HT_PORT_DEFAULT;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) handtrack_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mp-host") == 0) {
            cli_mp_role = NET_ROLE_HOST;  // scripted-launch convenience (deploy.sh); the in-game
        } else if (strcmp(argv[i], "--mp-join") == 0 && i + 1 < argc) {
            cli_mp_role = NET_ROLE_JOIN;  // MODE_SELECT screen works standalone without any of this.
            cli_mp_target_ip = argv[++i];
        } else if (strcmp(argv[i], "--mp-port") == 0 && i + 1 < argc) {
            cli_mp_port = (uint16_t)atoi(argv[++i]);
        }
    }
    if (handtrack_port == 0 && getenv("WH_HANDTRACK")) {
        handtrack_port = HT_PORT_DEFAULT;  // deploy.sh passes no args; env enables it there
    }
    signal(SIGINT, handle_sigint);

    PlatformWindow win = {0};
    if (!plat_init(&win, (int)INTERNAL_W, (int)INTERNAL_H)) {
        fprintf(stderr, "main: plat_init failed\n");
        return 1;
    }
    if (!render_init()) {
        fprintf(stderr, "main: render_init failed\n");
        plat_shutdown(&win);
        return 1;
    }

    glViewport(0, 0, win.width, win.height);
    glClearColor(PALETTE_VOID.r, PALETTE_VOID.g, PALETTE_VOID.b, PALETTE_VOID.a);

    HtState ht = {0};
    ht.fd = -1;
    if (handtrack_port) {
        if (!ht_init(&ht, (uint16_t)handtrack_port)) {
            fprintf(stderr, "main: handtrack disabled (bind failed); continuing keyboard-only\n");
        }
    }

    Sim sim;
    sim_init(&sim, seed);
    if (start_wave > 0) {
        sim_start_at_wave(&sim, start_wave);
        printf("main: starting at wave %d%s\n", start_wave,
                (start_wave % 5 == 0) ? " (Spiker)" : "");
    } else if (stress_n > 0) {
        sim_start_stress(&sim, stress_n);
        printf("main: stress mode, %d bouncing bullets\n", stress_n);
    } else if (cli_mp_role != NET_ROLE_NONE) {
        // Auto-start straight into WAITING_ROOM (skips MODE_SELECT) — for
        // deploy.sh / scripted Pi launches. Interactive play never needs
        // these flags; MODE_SELECT does the same thing from the keyboard.
        sim.player_count = 2;
        sim.net_role = cli_mp_role;
        sim.state = SIM_STATE_WAITING_ROOM;
        printf("main: multiplayer auto-start (%s)\n", cli_mp_role == NET_ROLE_HOST ? "host" : "join");
    }

    InputRing input_ring;
    input_ring_init(&input_ring);

    SnapshotBuffer snapshot_buf;
    snapshot_buffer_init(&snapshot_buf);

    SimMetricsBoard metrics_board;
    metrics_init(&metrics_board);

    RenderMetrics render_metrics;
    metrics_render_init(&render_metrics);

    NetplayState net;
    netplay_init(&net, NET_ROLE_NONE, NULL, 0);  // inert until MODE_SELECT/CLI picks a role
    NetplayStatusBoard net_status;
    netplay_status_init(&net_status);

    // Large (a few hundred KB each); heap-allocate rather than blow the stack.
    ReplayRecorder* recorder = malloc(sizeof(ReplayRecorder));
    ReplayPlayer* player = malloc(sizeof(ReplayPlayer));
    if (!recorder || !player) {
        fprintf(stderr, "main: out of memory allocating replay buffers\n");
        free(recorder);
        free(player);
        render_shutdown();
        plat_shutdown(&win);
        return 1;
    }
    replay_record_init(recorder);
    replay_player_init(player);

    _Atomic bool playback_requested = false;

    if (replay_path) {
        if (replay_load(player, replay_path)) {
            seed = player->seed;
            sim_init(&sim, seed);
            atomic_store(&playback_requested, true);
            printf("main: playing back '%s' (seed %llu, %u records)\n",
                    replay_path, (unsigned long long)player->seed, player->count);
        } else {
            fprintf(stderr, "main: --replay '%s' failed to load; running live\n", replay_path);
        }
    }

    SimThreadArgs sim_args = {
        .sim = &sim,
        .input_ring = &input_ring,
        .snapshot_buf = &snapshot_buf,
        .metrics_board = &metrics_board,
        .recorder = recorder,
        .player = player,
        .playback_requested = &playback_requested,
        .net = &net,
        .net_status = &net_status,
        .mp_target_ip = cli_mp_target_ip,
        .mp_port = cli_mp_port,
    };
    pthread_t sim_thread;
    if (pthread_create(&sim_thread, NULL, sim_thread_main, &sim_args) != 0) {
        fprintf(stderr, "main: pthread_create(sim_thread) failed\n");
        free(recorder);
        free(player);
        render_shutdown();
        plat_shutdown(&win);
        return 1;
    }

    bool hud_visible = true;
    uint32_t prev_keys = 0;

    PlatformInput input = {0};
    bool netdebug = getenv("WH_NETDEBUG") != NULL;
    uint32_t netdebug_frame = 0;
    while (!input.quit_requested && !g_shutdown_requested) {
        uint64_t frame_start = plat_time_ns();
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u top\n", netdebug_frame); fflush(stderr); }

        plat_poll(&win, &input);
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u after plat_poll\n", netdebug_frame); fflush(stderr); }
        InputFrame frame = {
            .keys = input.keys,
            .mouse_x = input.mouse_x,
            .mouse_y = input.mouse_y,
            .mouse_down = input.mouse_down,
            .use_aim_q = false,  // live input: sim derives aim from the cursor,
                                  // unless ht_merge below supplies a hand aim_q
            .tick = 0,
        };
        if (ht.fd >= 0) ht_merge(&ht, &frame, frame_start);
        input_ring_push(&input_ring, frame);

        if ((input.keys & KEY_HUD) && !(prev_keys & KEY_HUD)) {
            hud_visible = !hud_visible;
        }
        if ((input.keys & KEY_RESTART) && !(prev_keys & KEY_RESTART)) {
            metrics_render_reset_worst(&render_metrics);
        }

        const SimSnapshot* snap = snapshot_acquire_read(&snapshot_buf);

        // F2/F3 (record/replay) are single-player only: the recorder stores
        // one input stream, and multiplayer's input already IS a live
        // streamed replay (see netplay.h) — extending the on-disk format to
        // two players is a later, cheap follow-up, not done here.
        if (snap->net_role == NET_ROLE_NONE) {
            // F2: start/stop recording. The file write happens here, on the
            // render thread — never on the sim thread (§6.4).
            if ((input.keys & KEY_REC) && !(prev_keys & KEY_REC)) {
                if (replay_record_active(recorder)) {
                    atomic_store_explicit(&recorder->active, false, memory_order_release);
                    size_t bytes = replay_record_save(recorder, REPLAY_PATH);
                    if (bytes) {
                        printf("replay: wrote %s (%zu bytes, %u records)\n",
                                REPLAY_PATH, bytes, atomic_load(&recorder->count));
                    }
                } else {
                    replay_record_start(recorder, sim.seed);
                    printf("replay: recording (seed %llu)\n", (unsigned long long)sim.seed);
                }
            }

            // F3: play back the last recording.
            if ((input.keys & KEY_PLAY) && !(prev_keys & KEY_PLAY)) {
                if (replay_record_active(recorder)) {
                    atomic_store_explicit(&recorder->active, false, memory_order_release);
                    replay_record_save(recorder, REPLAY_PATH);
                }
                if (replay_load(player, REPLAY_PATH)) {
                    atomic_store_explicit(&playback_requested, true, memory_order_release);
                    printf("replay: playing %s (%u records)\n", REPLAY_PATH, player->count);
                }
            }
        }
        prev_keys = input.keys;

        SimMetrics sim_metrics;
        metrics_read(&metrics_board, &sim_metrics);
        NetplayStatus net_status_snapshot;
        netplay_read_status(&net_status, &net_status_snapshot);

        // WH_METRICS_STDOUT=1: ~1x/sec headless dump of the HUD's real-time
        // numbers, for verification when the physical display isn't
        // reachable (e.g. checking sim jitter/overruns over ssh while a
        // concurrent CPU load like the hand tracker is running).
        static bool metrics_stdout_checked = false, metrics_stdout_on = false;
        static uint64_t metrics_stdout_last_ns = 0;
        if (!metrics_stdout_checked) {
            metrics_stdout_on = getenv("WH_METRICS_STDOUT") != NULL;
            metrics_stdout_checked = true;
        }
        if (metrics_stdout_on && (frame_start - metrics_stdout_last_ns) >= 1000000000ull) {
            printf("metrics: tick=%llu jit_last=%lluus jit_mean=%.0fus jit_max=%lluus ovr=%llu exec=%lluus\n",
                    (unsigned long long)sim_metrics.tick,
                    (unsigned long long)(sim_metrics.jitter_last_ns / 1000),
                    sim_metrics.jitter_mean_ns / 1000.0,
                    (unsigned long long)(sim_metrics.jitter_max_ns / 1000),
                    (unsigned long long)sim_metrics.overrun_count,
                    (unsigned long long)(sim_metrics.exec_last_ns / 1000));
            fflush(stdout);
            metrics_stdout_last_ns = frame_start;
        }

        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u before glClear\n", netdebug_frame); fflush(stderr); }
        glClear(GL_COLOR_BUFFER_BIT);
        render_begin();
        render_draw_world(snap);
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u after render_draw_world\n", netdebug_frame); fflush(stderr); }
        if (hud_visible) {
            hud_draw(&sim_metrics, &render_metrics, snap,
                      replay_record_active(recorder), player->playing,
                      &net_status_snapshot);
        }
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u after hud_draw\n", netdebug_frame); fflush(stderr); }
        render_flush();
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u after render_flush\n", netdebug_frame); fflush(stderr); }
        plat_swap(&win);
        if (netdebug && netdebug_frame < 30) { fprintf(stderr, "netdebug: frame %u after plat_swap\n", netdebug_frame); fflush(stderr); }

        uint64_t frame_end = plat_time_ns();
        metrics_render_frame(&render_metrics, frame_end - frame_start);
        netdebug_frame++;
    }

    g_shutdown_requested = 1;
    pthread_join(sim_thread, NULL);

    // Don't lose a recording just because the window was closed mid-take.
    if (replay_record_active(recorder)) {
        atomic_store_explicit(&recorder->active, false, memory_order_release);
        size_t bytes = replay_record_save(recorder, REPLAY_PATH);
        if (bytes) printf("replay: saved in-progress recording to %s (%zu bytes)\n", REPLAY_PATH, bytes);
    }

    if (ht.fd >= 0) ht_shutdown(&ht);
    netplay_shutdown(&net);

    free(recorder);
    free(player);
    render_shutdown();
    plat_shutdown(&win);
    return 0;
}
