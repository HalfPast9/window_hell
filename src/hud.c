#include "hud.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "render.h"
#include "sim.h"

#define HUD_ORIGIN_X 8.0f
#define HUD_ORIGIN_Y 8.0f
#define HUD_LINE_H   18.0f
#define HUD_TEXT_SCALE 2.0f  // 8x8 glyphs -> 16x16 on screen; must read from 2m (§7.4)

#define HUD_HIST_BAR_W   8.0f
#define HUD_HIST_BAR_GAP 2.0f
#define HUD_HIST_MAX_H   28.0f

static const RGBA HUD_COLOR = { 124.0f / 255.0f, 255.0f / 255.0f, 176.0f / 255.0f, 1.0f };

static void hud_draw_histogram(float x, float y, const SimMetrics* sim_m) {
    uint64_t max_count = 0;
    for (int i = 0; i < JITTER_HIST_BUCKETS; i++) {
        if (sim_m->jitter_hist[i] > max_count) max_count = sim_m->jitter_hist[i];
    }

    for (int i = 0; i < JITTER_HIST_BUCKETS; i++) {
        float bar_h = 0.0f;
        if (max_count > 0 && sim_m->jitter_hist[i] > 0) {
            bar_h = ((float)sim_m->jitter_hist[i] / (float)max_count) * HUD_HIST_MAX_H;
            if (bar_h < 2.0f) bar_h = 2.0f; // stay visible even for a single sample
        }
        float bar_x = x + i * (HUD_HIST_BAR_W + HUD_HIST_BAR_GAP);
        float bar_y = y + (HUD_HIST_MAX_H - bar_h);
        render_push_quad(bar_x, bar_y, HUD_HIST_BAR_W, bar_h, ATLAS_WHITE, HUD_COLOR);
    }
}

void hud_draw(const SimMetrics* sim_m, const RenderMetrics* render_m,
              const SimSnapshot* snap, bool rec_active, bool play_active) {
    char line[128];
    float y = HUD_ORIGIN_Y;

    snprintf(line, sizeof(line), "FPS %.1f  FT %.1fms  WORST %.1fms",
              render_m->fps_ewma,
              (double)render_m->last_frame_ns / 1e6,
              (double)render_m->worst_frame_ns / 1e6);
    render_text(HUD_ORIGIN_X, y, HUD_TEXT_SCALE, line, HUD_COLOR);
    y += HUD_LINE_H;

    snprintf(line, sizeof(line), "SIM %dHz JIT avg %.0fus max %.0fus OVR %" PRIu64,
              SIM_HZ,
              sim_m->jitter_mean_ns / 1000.0,
              (double)sim_m->jitter_max_ns / 1000.0,
              sim_m->overrun_count);
    render_text(HUD_ORIGIN_X, y, HUD_TEXT_SCALE, line, HUD_COLOR);
    y += HUD_LINE_H;

    hud_draw_histogram(HUD_ORIGIN_X, y, sim_m);
    y += HUD_HIST_MAX_H + 6.0f;

    // font8x8 is plain ASCII (no bullet/triangle glyphs), so REC/PLAY badges
    // use ASCII markers rather than the mockup's Unicode ones (PRD §7.4).
    snprintf(line, sizeof(line), "TICK %" PRIu64 "   REC%s PLAY%s        SCORE %u",
              snap->tick,
              rec_active ? "*" : " ",
              play_active ? ">" : " ",
              snap->score);
    render_text(HUD_ORIGIN_X, y, HUD_TEXT_SCALE, line, HUD_COLOR);

    // Lives/wave aren't in the §7.4 mockup (which predates enemies/lives
    // landing in M4) but the player needs to see them somewhere — top-right,
    // out of the way of the telemetry block.
    if (snap->state == SIM_STATE_PLAY || snap->state == SIM_STATE_UPGRADE) {
        char status[32];
        snprintf(status, sizeof(status), "WAVE %u  LIVES %u", (unsigned)snap->wave, (unsigned)snap->lives);
        float w = (float)strlen(status) * 8.0f * HUD_TEXT_SCALE;
        render_text(INTERNAL_W - HUD_ORIGIN_X - w, HUD_ORIGIN_Y, HUD_TEXT_SCALE, status, HUD_COLOR);
    }
}
