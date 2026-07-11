// render.h — batched-quad GLES2 renderer (PRD §7.1). One shader program, one
// dynamic VBO, orphan-pattern updates, ortho MVP fixed to the internal
// 1280x720 space. render_flush() draws whatever's been pushed since the last
// flush/begin and resets the batch — call it once per distinct scissor
// state, so a frame is a handful of flushes (PRD §7.1: "≤3 draw calls").
#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>

#include "atlas.h"
#include "snapshot.h"

typedef struct { float r, g, b, a; } RGBA;

bool render_init(void);
void render_shutdown(void);
void render_begin(void);
void render_push_quad(float x, float y, float w, float h, AtlasUV uv, RGBA color);
void render_set_scissor(float x, float y, float w, float h);
void render_clear_scissor(void);
void render_flush(void);

// Draws an 8x8-font string (ASCII only) at the given scale; returns the x
// just past the last glyph, so callers can chain multiple draws on a line.
// Shared by hud.c and the menu/dead/upgrade screen text in render.c itself.
float render_text(float x, float y, float scale, const char* s, RGBA color);

// Draws the playfield frame (fill/glow/border/corners/edge-flash, PRD §7.6),
// the scissor-clipped world entities (player, player shots, enemies), and —
// outside PLAY — the menu/dead/upgrade screen text. Issues its own draw
// calls internally and leaves scissor disabled on return so the caller's
// next batch (HUD) isn't clipped.
void render_draw_world(const SimSnapshot* snap);

#endif // RENDER_H
