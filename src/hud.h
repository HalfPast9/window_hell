// hud.h — telemetry HUD (PRD §7.4). To a player it's set dressing; to a QNX
// engineer it's the pitch. Draws via render_push_quad — caller wraps in
// render_begin()/render_flush(), same batch pipeline as everything else.
#ifndef HUD_H
#define HUD_H

#include <stdbool.h>

#include "metrics.h"
#include "netplay.h"
#include "snapshot.h"

// net_status may be NULL (single-player builds/tools that never touch
// netplay); the NET line is simply omitted then.
void hud_draw(const SimMetrics* sim_m, const RenderMetrics* render_m,
              const SimSnapshot* snap, bool rec_active, bool play_active,
              const NetplayStatus* net_status);

#endif // HUD_H
