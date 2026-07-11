#include "metrics.h"

#include <string.h>

#define JITTER_EWMA_ALPHA 0.05  // ~20-tick time constant at 240Hz (~83ms)

void metrics_init(SimMetricsBoard* mb) {
    atomic_store_explicit(&mb->seq, 0, memory_order_relaxed);
    memset(&mb->data, 0, sizeof(mb->data));
}

static void seqlock_write_begin(SimMetricsBoard* mb) {
    uint32_t seq = atomic_load_explicit(&mb->seq, memory_order_relaxed);
    atomic_store_explicit(&mb->seq, seq + 1, memory_order_release); // now odd: writer in progress
}

static void seqlock_write_end(SimMetricsBoard* mb) {
    uint32_t seq = atomic_load_explicit(&mb->seq, memory_order_relaxed);
    atomic_store_explicit(&mb->seq, seq + 1, memory_order_release); // back to even: write visible
}

void metrics_record_tick(SimMetricsBoard* mb, uint64_t tick, uint64_t jitter_ns, uint64_t exec_ns) {
    SimMetrics* d = &mb->data;

    uint32_t bucket = (uint32_t)(jitter_ns / JITTER_HIST_BUCKET_NS);
    if (bucket >= JITTER_HIST_BUCKETS) bucket = JITTER_HIST_BUCKETS - 1;

    seqlock_write_begin(mb);
    d->tick = tick;
    d->jitter_last_ns = jitter_ns;
    d->jitter_mean_ns = d->jitter_mean_ns == 0.0
        ? (double)jitter_ns
        : d->jitter_mean_ns + JITTER_EWMA_ALPHA * ((double)jitter_ns - d->jitter_mean_ns);
    if (jitter_ns > d->jitter_max_ns) d->jitter_max_ns = jitter_ns;
    d->jitter_hist[bucket]++;
    d->exec_last_ns = exec_ns;
    seqlock_write_end(mb);
}

void metrics_record_overrun(SimMetricsBoard* mb) {
    seqlock_write_begin(mb);
    mb->data.overrun_count++;
    seqlock_write_end(mb);
}

void metrics_reset_worst(SimMetricsBoard* mb) {
    seqlock_write_begin(mb);
    mb->data.jitter_max_ns = 0;
    memset(mb->data.jitter_hist, 0, sizeof(mb->data.jitter_hist));
    mb->data.overrun_count = 0;
    seqlock_write_end(mb);
}

void metrics_read(const SimMetricsBoard* mb, SimMetrics* out) {
    for (;;) {
        uint32_t seq1 = atomic_load_explicit(&mb->seq, memory_order_acquire);
        if (seq1 & 1u) continue; // writer in progress, retry
        *out = mb->data;
        uint32_t seq2 = atomic_load_explicit(&mb->seq, memory_order_acquire);
        if (seq1 == seq2) return; // consistent snapshot
    }
}

void metrics_render_init(RenderMetrics* rm) {
    memset(rm, 0, sizeof(*rm));
}

void metrics_render_frame(RenderMetrics* rm, uint64_t frame_ns) {
    rm->last_frame_ns = frame_ns;
    double fps = frame_ns > 0 ? 1e9 / (double)frame_ns : 0.0;
    rm->fps_ewma = rm->fps_ewma == 0.0 ? fps : rm->fps_ewma + 0.1 * (fps - rm->fps_ewma);
    if (frame_ns > rm->worst_frame_ns) rm->worst_frame_ns = frame_ns;
}

void metrics_render_reset_worst(RenderMetrics* rm) {
    rm->worst_frame_ns = 0;
}
