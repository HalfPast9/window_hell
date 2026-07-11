// metrics.h — lock-free timing capture, sim thread -> render thread.
//
// SimMetrics is written exclusively by the sim thread and read exclusively by
// the render thread (via metrics_read), protected by a seqlock: no blocking
// on either side, sim never stalls waiting on render and vice versa.
// RenderMetrics is owned entirely by the render thread — no synchronization
// needed since nothing else touches it.
#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define JITTER_HIST_BUCKETS  16
#define JITTER_HIST_BUCKET_NS (50ull * 1000)  // 50us/bucket; last bucket is catch-all

typedef struct {
    uint64_t tick;
    uint64_t jitter_last_ns;
    double   jitter_mean_ns;                      // EWMA
    uint64_t jitter_max_ns;                        // since last reset
    uint64_t jitter_hist[JITTER_HIST_BUCKETS];
    uint64_t overrun_count;
    uint64_t exec_last_ns;                         // time sim_step() took
} SimMetrics;

typedef struct {
    _Atomic uint32_t seq;
    SimMetrics        data;
} SimMetricsBoard;

typedef struct {
    uint64_t last_frame_ns;
    double   fps_ewma;
    uint64_t worst_frame_ns;                        // latches until metrics_render_reset_worst
} RenderMetrics;

void metrics_init(SimMetricsBoard* mb);
void metrics_record_tick(SimMetricsBoard* mb, uint64_t tick, uint64_t jitter_ns, uint64_t exec_ns);
void metrics_record_overrun(SimMetricsBoard* mb);
void metrics_reset_worst(SimMetricsBoard* mb);
void metrics_read(const SimMetricsBoard* mb, SimMetrics* out);

void metrics_render_init(RenderMetrics* rm);
void metrics_render_frame(RenderMetrics* rm, uint64_t frame_ns);
void metrics_render_reset_worst(RenderMetrics* rm);

#endif // METRICS_H
