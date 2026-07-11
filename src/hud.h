// hud.h — telemetry HUD (PRD §7.4). To a player it's set dressing; to a QNX
// engineer it's the pitch. Draws via render_push_quad — caller wraps in
// render_begin()/render_flush(), same batch pipeline as everything else.
#ifndef HUD_H
#define HUD_H

#include <stdbool.h>

#include "metrics.h"
#include "snapshot.h"

void hud_draw(const SimMetrics* sim_m, const RenderMetrics* render_m,
              const SimSnapshot* snap, bool rec_active, bool play_active);

#endif // HUD_H
