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

#include "hud.h"
#include "input_ring.h"
#include "metrics.h"
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
    Sim*             sim;
    InputRing*       input_ring;
    SnapshotBuffer*  snapshot_buf;
    SimMetricsBoard* metrics_board;
    ReplayRecorder*  recorder;
    ReplayPlayer*    player;
    _Atomic bool*    playback_requested;  // render thread -> sim thread
} SimThreadArgs;

// Packs the sim's trigger state into the replay log's single uint16.
static uint16_t pack_replay_keys(const Sim* sim) {
    uint16_t k = (uint16_t)(sim->input.keys & REPLAY_KEY_MASK);
    if (sim->input.mouse_down) k |= REPLAY_KEY_MOUSE_FIRE;
    return k;
}

static void* sim_thread_main(void* arg_ptr) {
    SimThreadArgs* args = (SimThreadArgs*)arg_ptr;

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
            sim_init(args->sim, args->player->seed);
            playing = args->player->playing;
        }

        // Always drain the ring so it can't back up, then let the replay log
        // override the input if we're playing one back.
        sim_consume_input(args->sim, args->input_ring);
        args->sim->input.use_aim_q = false;

        if (playing) {
            uint16_t keys16, aim_q;
            if (replay_input_for_tick(args->player, args->sim->tick + 1, &keys16, &aim_q)) {
                args->sim->input.keys = (uint32_t)(keys16 & REPLAY_KEY_MASK);
                args->sim->input.mouse_down = (keys16 & REPLAY_KEY_MOUSE_FIRE) != 0;
                args->sim->input.aim_q = aim_q;
                args->sim->input.use_aim_q = true;  // aim from the log, not a cursor
            } else {
                playing = false;  // log exhausted; hand control back to the player
            }
        }

        uint32_t keys = args->sim->input.keys;
        if ((keys & KEY_RESTART) && !(prev_keys & KEY_RESTART)) {
            metrics_reset_worst(args->metrics_board);
        }
        prev_keys = keys;

        uint64_t t0 = plat_time_ns();
        sim_step(args->sim);
        uint64_t t1 = plat_time_ns();

        // Record AFTER the step: aim_q is resolved inside sim_step (step_aim),
        // so this captures the exact value the tick actually ran with.
        if (!playing) {
            replay_record_tick(args->recorder, args->sim->tick,
                                pack_replay_keys(args->sim), args->sim->aim_q);
        }

        uint64_t jitter_ns = (t0 > scheduled_time) ? (t0 - scheduled_time) : 0;
        metrics_record_tick(args->metrics_board, args->sim->tick, jitter_ns, t1 - t0);

        sim_publish(args->sim, args->snapshot_buf);

        if (t1 > next) {
            metrics_record_overrun(args->metrics_board);
            next = t1;  // late: run the next tick immediately, never skip sim steps silently
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
    int start_wave = 0;  // 0 = normal (start at the menu)
    int stress_n = 0;    // 0 = disabled; see PRD §10 M3 acceptance test

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--wave") == 0 && i + 1 < argc) {
            start_wave = atoi(argv[++i]);  // dev/demo: jump to a wave (5, 10, ... = boss)
        } else if (strcmp(argv[i], "--stress") == 0 && i + 1 < argc) {
            stress_n = atoi(argv[++i]);  // dev/demo: N bouncing bullets, perf stress test
        }
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

    Sim sim;
    sim_init(&sim, seed);
    if (start_wave > 0) {
        sim_start_at_wave(&sim, start_wave);
        printf("main: starting at wave %d%s\n", start_wave,
                (start_wave % 5 == 0) ? " (Spiker)" : "");
    } else if (stress_n > 0) {
        sim_start_stress(&sim, stress_n);
        printf("main: stress mode, %d bouncing bullets\n", stress_n);
    }

    InputRing input_ring;
    input_ring_init(&input_ring);

    SnapshotBuffer snapshot_buf;
    snapshot_buffer_init(&snapshot_buf);

    SimMetricsBoard metrics_board;
    metrics_init(&metrics_board);

    RenderMetrics render_metrics;
    metrics_render_init(&render_metrics);

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
    while (!input.quit_requested && !g_shutdown_requested) {
        uint64_t frame_start = plat_time_ns();

        plat_poll(&win, &input);
        input_ring_push(&input_ring, (InputFrame){
            .keys = input.keys,
            .mouse_x = input.mouse_x,
            .mouse_y = input.mouse_y,
            .mouse_down = input.mouse_down,
            .use_aim_q = false,  // live input: sim derives aim from the cursor
            .tick = 0,
        });

        if ((input.keys & KEY_HUD) && !(prev_keys & KEY_HUD)) {
            hud_visible = !hud_visible;
        }
        if ((input.keys & KEY_RESTART) && !(prev_keys & KEY_RESTART)) {
            metrics_render_reset_worst(&render_metrics);
        }

        // F2: start/stop recording. The file write happens here, on the render
        // thread — never on the sim thread (§6.4).
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
        prev_keys = input.keys;

        SimMetrics sim_metrics;
        metrics_read(&metrics_board, &sim_metrics);

        const SimSnapshot* snap = snapshot_acquire_read(&snapshot_buf);

        glClear(GL_COLOR_BUFFER_BIT);
        render_begin();
        render_draw_world(snap);
        if (hud_visible) {
            hud_draw(&sim_metrics, &render_metrics, snap,
                      replay_record_active(recorder), player->playing);
        }
        render_flush();
        plat_swap(&win);

        uint64_t frame_end = plat_time_ns();
        metrics_render_frame(&render_metrics, frame_end - frame_start);
    }

    g_shutdown_requested = 1;
    pthread_join(sim_thread, NULL);

    // Don't lose a recording just because the window was closed mid-take.
    if (replay_record_active(recorder)) {
        atomic_store_explicit(&recorder->active, false, memory_order_release);
        size_t bytes = replay_record_save(recorder, REPLAY_PATH);
        if (bytes) printf("replay: saved in-progress recording to %s (%zu bytes)\n", REPLAY_PATH, bytes);
    }

    free(recorder);
    free(player);
    render_shutdown();
    plat_shutdown(&win);
    return 0;
}
