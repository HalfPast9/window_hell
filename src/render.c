#include "render.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "atlas_data.h"
#include "palette.h"
#include "shaders.h"

#define INTERNAL_W 1280.0f
#define INTERNAL_H 720.0f

#define MAX_QUADS       4096  // comfortably covers HUD + world/frame/screen-text quads
#define VERTS_PER_QUAD  6

#define BORDER_THICKNESS   3.0f
#define BORDER_GLOW_WIDTH  8.0f
#define BORDER_GLOW_ALPHA  0.25f
#define CORNER_SIZE        6.0f
#define BORDER_PULSE_MAX_HZ 3.0f

#define PLAYER_SPRITE_SIZE 16.0f
#define PSHOT_SPRITE_W 8.0f
#define PSHOT_SPRITE_H 16.0f
#define ENEMY_SPRITE_SIZE 16.0f
// Drawn larger than its 8 px atlas cell and its 4 px hitbox: fat bullets are a
// readability decision, and the sprite is deliberately bigger than what kills
// you (danmaku convention — trust the dot, §0.1).
#define BULLET_SPRITE_SIZE 12.0f
#define BULLET_POP_START_SCALE 0.35f  // spawn-pop scales from this up to 1.0
#define SPIKER_SPRITE_SIZE 32.0f

#define BEAM_FIRE_SEG_LEN 8.0f   // step between marks along a firing beam
#define BEAM_DOT_LEN      4.0f   // telegraph dot size
#define BEAM_DOT_SPACING 14.0f   // gap between telegraph dots
#define BEAM_DOT_WIDTH    4.0f

#define PLAYER_BLINK_HALF_PERIOD_TICKS 30  // 4Hz invuln blink (§8.2)

static const RGBA WHITE = { 1.0f, 1.0f, 1.0f, 1.0f };

// sprite index values (SnapEntity.sprite), per PRD §6.3: 1=triangle 2=circle
static const char* UPGRADE_NAMES[6] = {
    "SPEED", "FIRE RATE", "MULTI SHOT", "MAX HEALTH", "WALL PUNCH", "BULWARK",
};

typedef struct { float x, y, u, v, r, g, b, a; } Vertex;

static GLuint s_program;
static GLuint s_vbo;
static GLuint s_texture;
static GLint  s_loc_mvp;
static GLint  s_loc_tex;

static Vertex s_verts[MAX_QUADS * VERTS_PER_QUAD];
static int    s_vert_count;

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "render: shader compile failed: %s\n", log);
    }
    return sh;
}

static GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glBindAttribLocation(prog, 2, "a_color");
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "render: program link failed: %s\n", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

bool render_init(void) {
    s_program = link_program(SHADER_VS_SRC, SHADER_FS_SRC);
    if (!s_program) return false;

    s_loc_mvp = glGetUniformLocation(s_program, "u_mvp");
    s_loc_tex = glGetUniformLocation(s_program, "u_tex");

    glGenBuffers(1, &s_vbo);

    glGenTextures(1, &s_texture);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, ATLAS_DATA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    s_vert_count = 0;
    return true;
}

void render_shutdown(void) {
    glDeleteTextures(1, &s_texture);
    glDeleteBuffers(1, &s_vbo);
    glDeleteProgram(s_program);
}

void render_begin(void) {
    s_vert_count = 0;
}

void render_push_quad(float x, float y, float w, float h, AtlasUV uv, RGBA color) {
    if (s_vert_count + VERTS_PER_QUAD > MAX_QUADS * VERTS_PER_QUAD) return; // over budget: drop silently

    Vertex tl = { x,     y,     uv.u0, uv.v0, color.r, color.g, color.b, color.a };
    Vertex tr = { x + w, y,     uv.u1, uv.v0, color.r, color.g, color.b, color.a };
    Vertex bl = { x,     y + h, uv.u0, uv.v1, color.r, color.g, color.b, color.a };
    Vertex br = { x + w, y + h, uv.u1, uv.v1, color.r, color.g, color.b, color.a };

    Vertex* out = &s_verts[s_vert_count];
    out[0] = tl; out[1] = tr; out[2] = bl;
    out[3] = tr; out[4] = br; out[5] = bl;
    s_vert_count += VERTS_PER_QUAD;
}

void render_flush(void) {
    if (s_vert_count == 0) return;

    // fixed ortho(0,1280,720,0): x:[0,1280]->[-1,1], y:[0,720]->[1,-1] (y-down screen space)
    const float mvp[16] = {
        2.0f / INTERNAL_W, 0.0f,                0.0f, 0.0f,
        0.0f,               -2.0f / INTERNAL_H, 0.0f, 0.0f,
        0.0f,               0.0f,               1.0f, 0.0f,
        -1.0f,              1.0f,                0.0f, 1.0f,
    };

    glUseProgram(s_program);
    glUniformMatrix4fv(s_loc_mvp, 1, GL_FALSE, mvp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glUniform1i(s_loc_tex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_verts), NULL, GL_DYNAMIC_DRAW); // orphan
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * (size_t)s_vert_count, s_verts);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, r));

    glDrawArrays(GL_TRIANGLES, 0, s_vert_count);
    s_vert_count = 0;  // batch consumed; caller may push+flush again this frame
}

void render_set_scissor(float x, float y, float w, float h) {
    glEnable(GL_SCISSOR_TEST);
    int gl_x = (int)x;
    int gl_y = (int)(INTERNAL_H - (y + h));  // GL scissor origin is bottom-left
    int gl_w = (int)(w < 0.0f ? 0.0f : w);
    int gl_h = (int)(h < 0.0f ? 0.0f : h);
    glScissor(gl_x, gl_y, gl_w, gl_h);
}

void render_clear_scissor(void) {
    glDisable(GL_SCISSOR_TEST);
}

float render_text(float x, float y, float scale, const char* s, RGBA color) {
    for (const char* c = s; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch < 128 && ch != ' ') {
            render_push_quad(x, y, 8.0f * scale, 8.0f * scale, ATLAS_FONT[ch], color);
        }
        x += 8.0f * scale;
    }
    return x;
}

static void draw_centered_text(float cx, float y, float scale, const char* s, RGBA color) {
    float w = (float)strlen(s) * 8.0f * scale;
    render_text(cx - w * 0.5f, y, scale, s, color);
}

static RGBA lerp_rgba(RGBA a, RGBA b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    RGBA out = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    return out;
}

static RGBA scale_rgb(RGBA c, float m) {
    RGBA out = { c.r * m, c.g * m, c.b * m, c.a };
    return out;
}

static RGBA desaturate(RGBA c) {
    float gray = (c.r + c.g + c.b) / 3.0f;
    RGBA out = { gray, gray, gray, c.a };
    return out;
}

// Border pulse (PRD §7.6): "sim advances phase faster as danger->1". Kept as
// a pure function of already-snapshot fields (tick, danger) instead of
// renderer-local mutable state, so it stays deterministic/replay-safe (§6.4)
// without needing a new field in the LOCKED SimSnapshot struct.
#define RENDER_SIM_DT (1.0f / 240.0f)

// Beams are drawn as a chain of small quads along the ray rather than one
// rotated quad — the batch pipeline only emits axis-aligned quads (§7.1), and
// the dotted telegraph wants discrete segments anyway.
static void draw_beam(const SnapBeam* b, float sx, float sy) {
    bool firing = (b->state == 1u);
    float ex = cosf(b->angle), ey = sinf(b->angle);

    float seg = firing ? BEAM_FIRE_SEG_LEN : BEAM_DOT_SPACING;
    float dot = firing ? BEAM_FIRE_SEG_LEN : BEAM_DOT_LEN;
    float width = firing ? b->width : BEAM_DOT_WIDTH;

    RGBA color = firing ? (RGBA){ 1.0f, 1.0f, 1.0f, 0.95f }
                         : (RGBA){ PALETTE_ENEMY_BULLET.r, PALETTE_ENEMY_BULLET.g,
                                    PALETTE_ENEMY_BULLET.b, 0.55f };

    for (float t = 0.0f; t < b->len; t += seg) {
        float cx = b->x + ex * (t + dot * 0.5f) + sx;
        float cy = b->y + ey * (t + dot * 0.5f) + sy;
        // Square marks: an axis-aligned quad can't be rotated, so sizing each
        // mark by the beam width keeps a diagonal beam from reading as a stair-step.
        float s = firing ? width : dot;
        render_push_quad(cx - s * 0.5f, cy - s * 0.5f, s, s, ATLAS_WHITE, color);
    }
}

// The boss's own window. Pink to read as hostile territory, and it brightens
// the moment it overlaps the player's rect — that flash IS the "you can hurt
// it now" tell, so it has to be unmissable.
static void draw_boss_window(const SimSnapshot* snap, float sx, float sy) {
    float x = snap->boss_win_x + sx, y = snap->boss_win_y + sy;
    float w = snap->boss_win_w, h = snap->boss_win_h;
    bool merged = snap->boss_vulnerable != 0;

    RGBA fill = { 0.09f, 0.05f, 0.09f, 1.0f };  // faintly warmer than the void
    render_push_quad(x, y, w, h, ATLAS_WHITE, fill);

    RGBA border = PALETTE_OCTAGON;
    if (merged) {
        float pulse = 0.75f + 0.25f * sinf((float)snap->tick * 0.12f);
        border = scale_rgb(WHITE, pulse);
    }
    RGBA glow = border;
    glow.a = merged ? 0.35f : 0.18f;

    render_push_quad(x - BORDER_GLOW_WIDTH, y - BORDER_GLOW_WIDTH,
                      BORDER_GLOW_WIDTH, h + 2.0f * BORDER_GLOW_WIDTH, ATLAS_WHITE, glow);
    render_push_quad(x + w, y - BORDER_GLOW_WIDTH,
                      BORDER_GLOW_WIDTH, h + 2.0f * BORDER_GLOW_WIDTH, ATLAS_WHITE, glow);
    render_push_quad(x - BORDER_GLOW_WIDTH, y - BORDER_GLOW_WIDTH,
                      w + 2.0f * BORDER_GLOW_WIDTH, BORDER_GLOW_WIDTH, ATLAS_WHITE, glow);
    render_push_quad(x - BORDER_GLOW_WIDTH, y + h,
                      w + 2.0f * BORDER_GLOW_WIDTH, BORDER_GLOW_WIDTH, ATLAS_WHITE, glow);

    render_push_quad(x - BORDER_THICKNESS, y, BORDER_THICKNESS, h, ATLAS_WHITE, border);
    render_push_quad(x + w, y, BORDER_THICKNESS, h, ATLAS_WHITE, border);
    render_push_quad(x, y - BORDER_THICKNESS, w, BORDER_THICKNESS, ATLAS_WHITE, border);
    render_push_quad(x, y + h, w, BORDER_THICKNESS, ATLAS_WHITE, border);
}

// `boss_only` splits the enemy list between the two scissored passes: the boss
// belongs to its own room, everyone else to the player's.
static void draw_enemies(const SimSnapshot* snap, float sx, float sy, bool boss_only) {
    for (uint16_t i = 0; i < snap->enemy_count; i++) {
        const SnapEntity* e = &snap->enemies[i];
        bool is_boss = (e->sprite == 4u);
        if (is_boss != boss_only) continue;

        AtlasUV uv;
        RGBA color;
        float size = ENEMY_SPRITE_SIZE;
        switch (e->sprite) {
            case 1u: uv = ATLAS_TRIANGLE; color = PALETTE_TRIANGLE; break;
            case 2u: uv = ATLAS_CIRCLE;   color = PALETTE_CIRCLE;   break;
            case 3u: uv = ATLAS_OCTAGON;  color = PALETTE_OCTAGON;  break;
            default: uv = ATLAS_SPIKER;   color = PALETTE_SPIKER_CORE;
                      size = SPIKER_SPRITE_SIZE; break;
        }
        // For the boss, flag bit 1 means "windows not merged", not "in the void".
        if (e->flags & 1u) color = scale_rgb(desaturate(color), PALETTE_OUTSIDE_TINT);
        if (e->flags & 8u) color = lerp_rgba(color, WHITE, 0.6f);  // hit / telegraph
        render_push_quad(e->x + sx - size * 0.5f, e->y + sy - size * 0.5f,
                          size, size, uv, color);
    }
}

static void draw_pshots(const SimSnapshot* snap, float sx, float sy) {
    for (uint16_t i = 0; i < snap->pshot_count; i++) {
        const SnapEntity* s = &snap->pshots[i];
        render_push_quad(s->x + sx - PSHOT_SPRITE_W * 0.5f,
                          s->y + sy - PSHOT_SPRITE_H * 0.5f,
                          PSHOT_SPRITE_W, PSHOT_SPRITE_H, ATLAS_PSHOT, PALETTE_PLAYER_SHOT);
    }
}

static void draw_player(const SimSnapshot* snap, float sx, float sy) {
    bool blink_hidden = (snap->player.flags & 2u) &&
                         ((snap->tick / PLAYER_BLINK_HALF_PERIOD_TICKS) % 2 == 1);
    if (blink_hidden) return;

    RGBA color = PALETTE_PLAYER;
    if (snap->hit_flash > 0.0f && snap->state == SIM_STATE_PLAY) {
        color = lerp_rgba(color, WHITE, snap->hit_flash);
    }
    render_push_quad(snap->player.x + sx - PLAYER_SPRITE_SIZE * 0.5f,
                      snap->player.y + sy - PLAYER_SPRITE_SIZE * 0.5f,
                      PLAYER_SPRITE_SIZE, PLAYER_SPRITE_SIZE, ATLAS_PLAYER, color);
}

static void draw_bullets(const SimSnapshot* snap, float sx, float sy) {
    for (uint16_t i = 0; i < snap->bullet_count; i++) {
        const SnapEntity* b = &snap->bullets[i];
        float size = BULLET_SPRITE_SIZE;
        if (b->flags & 4u) {  // spawn pop (§8.6): scale in over its first ticks
            float t = (float)b->age / (float)BULLET_POP_TICKS;
            size *= BULLET_POP_START_SCALE + (1.0f - BULLET_POP_START_SCALE) * t;
        }
        render_push_quad(b->x + sx - size * 0.5f, b->y + sy - size * 0.5f,
                          size, size, ATLAS_BULLET, PALETTE_ENEMY_BULLET);
    }
}

static float border_pulse_mult(uint64_t tick, float danger) {
    if (danger <= 0.0f) return 1.0f;
    float phase = (float)tick * RENDER_SIM_DT * BORDER_PULSE_MAX_HZ * 6.2831853f;
    float pulse = 0.6f + 0.4f * sinf(phase);
    return 1.0f - danger * (1.0f - pulse);
}

static void draw_menu_screen(void) {
    float cx = INTERNAL_W * 0.5f;
    draw_centered_text(cx, INTERNAL_H * 0.5f - 80.0f, 4.0f, "WINDOWED HELL", PALETTE_BORDER_NORMAL);
    draw_centered_text(cx, INTERNAL_H * 0.5f - 10.0f, 2.0f, "PRESS SHOOT TO START", PALETTE_PLAYER);
    draw_centered_text(cx, INTERNAL_H * 0.5f + 30.0f, 1.0f,
                        "WINDOW MECHANIC INSPIRED BY WINDOWKILL (TORCADO)", PALETTE_HUD_TEXT);
}

static void draw_dead_screen(const SimSnapshot* snap) {
    float cx = INTERNAL_W * 0.5f;
    char line[64];
    draw_centered_text(cx, INTERNAL_H * 0.5f - 40.0f, 3.0f, "GAME OVER", PALETTE_BORDER_DANGER);
    snprintf(line, sizeof(line), "SCORE %u   WAVE %u", (unsigned)snap->score, (unsigned)snap->wave);
    draw_centered_text(cx, INTERNAL_H * 0.5f + 10.0f, 2.0f, line, PALETTE_PLAYER);
    draw_centered_text(cx, INTERNAL_H * 0.5f + 40.0f, 1.0f, "PRESS R TO RESTART", PALETTE_HUD_TEXT);
}

static void draw_upgrade_screen(const SimSnapshot* snap) {
    float cx = INTERNAL_W * 0.5f;
    int selected = snap->upgrade_selected ? 1 : 0;

    draw_centered_text(cx, INTERNAL_H * 0.5f - 60.0f, 2.0f, "WAVE CLEAR - CHOOSE AN UPGRADE", PALETTE_PLAYER);

    char buf_a[32], buf_b[32];
    snprintf(buf_a, sizeof(buf_a), "%s%s", selected == 0 ? "-> " : "   ", UPGRADE_NAMES[snap->upgrade_a % 6]);
    snprintf(buf_b, sizeof(buf_b), "%s%s", selected == 1 ? "-> " : "   ", UPGRADE_NAMES[snap->upgrade_b % 6]);

    RGBA dim = { 0.5f, 0.5f, 0.5f, 1.0f };
    render_text(cx - 220.0f, INTERNAL_H * 0.5f, 2.0f, buf_a, selected == 0 ? WHITE : dim);
    render_text(cx + 40.0f, INTERNAL_H * 0.5f, 2.0f, buf_b, selected == 1 ? WHITE : dim);

    draw_centered_text(cx, INTERNAL_H * 0.5f + 40.0f, 1.0f,
                        "LEFT/RIGHT SELECT   SHOOT (OR WAIT) TO CONFIRM", PALETTE_HUD_TEXT);
}

void render_draw_world(const SimSnapshot* snap) {
    float sx = snap->shake_x, sy = snap->shake_y;
    float left = snap->playfield_x + sx;
    float top = snap->playfield_y + sy;
    float w = snap->playfield_w;
    float h = snap->playfield_h;
    float right = left + w;
    float bottom = top + h;

    RGBA border_base = lerp_rgba(PALETTE_BORDER_NORMAL, PALETTE_BORDER_DANGER, snap->danger);
    RGBA border_color = scale_rgb(border_base, border_pulse_mult(snap->tick, snap->danger));
    RGBA glow_color = border_color;
    glow_color.a = BORDER_GLOW_ALPHA;
    RGBA corner_color = { 1.0f, 1.0f, 1.0f, 1.0f };  // bright accent, full white

    // --- frame batch (unscissored): fill, glow, border, corners, edge flash ---
    render_push_quad(left, top, w, h, ATLAS_WHITE, PALETTE_PLAYFIELD);

    render_push_quad(left - BORDER_GLOW_WIDTH, top - BORDER_GLOW_WIDTH,
                      BORDER_GLOW_WIDTH, h + 2.0f * BORDER_GLOW_WIDTH, ATLAS_WHITE, glow_color);
    render_push_quad(right, top - BORDER_GLOW_WIDTH,
                      BORDER_GLOW_WIDTH, h + 2.0f * BORDER_GLOW_WIDTH, ATLAS_WHITE, glow_color);
    render_push_quad(left - BORDER_GLOW_WIDTH, top - BORDER_GLOW_WIDTH,
                      w + 2.0f * BORDER_GLOW_WIDTH, BORDER_GLOW_WIDTH, ATLAS_WHITE, glow_color);
    render_push_quad(left - BORDER_GLOW_WIDTH, bottom,
                      w + 2.0f * BORDER_GLOW_WIDTH, BORDER_GLOW_WIDTH, ATLAS_WHITE, glow_color);

    render_push_quad(left - BORDER_THICKNESS, top, BORDER_THICKNESS, h, ATLAS_WHITE, border_color);
    render_push_quad(right, top, BORDER_THICKNESS, h, ATLAS_WHITE, border_color);
    render_push_quad(left, top - BORDER_THICKNESS, w, BORDER_THICKNESS, ATLAS_WHITE, border_color);
    render_push_quad(left, bottom, w, BORDER_THICKNESS, ATLAS_WHITE, border_color);

    render_push_quad(left - CORNER_SIZE * 0.5f, top - CORNER_SIZE * 0.5f, CORNER_SIZE, CORNER_SIZE, ATLAS_WHITE, corner_color);
    render_push_quad(right - CORNER_SIZE * 0.5f, top - CORNER_SIZE * 0.5f, CORNER_SIZE, CORNER_SIZE, ATLAS_WHITE, corner_color);
    render_push_quad(left - CORNER_SIZE * 0.5f, bottom - CORNER_SIZE * 0.5f, CORNER_SIZE, CORNER_SIZE, ATLAS_WHITE, corner_color);
    render_push_quad(right - CORNER_SIZE * 0.5f, bottom - CORNER_SIZE * 0.5f, CORNER_SIZE, CORNER_SIZE, ATLAS_WHITE, corner_color);

    RGBA flash = { 1.0f, 1.0f, 1.0f, 0.0f };
    if (snap->edge_flash[0] > 0.0f) { flash.a = snap->edge_flash[0]; render_push_quad(left - BORDER_THICKNESS, top, BORDER_THICKNESS, h, ATLAS_WHITE, flash); }
    if (snap->edge_flash[1] > 0.0f) { flash.a = snap->edge_flash[1]; render_push_quad(right, top, BORDER_THICKNESS, h, ATLAS_WHITE, flash); }
    if (snap->edge_flash[2] > 0.0f) { flash.a = snap->edge_flash[2]; render_push_quad(left, top - BORDER_THICKNESS, w, BORDER_THICKNESS, ATLAS_WHITE, flash); }
    if (snap->edge_flash[3] > 0.0f) { flash.a = snap->edge_flash[3]; render_push_quad(left, bottom, w, BORDER_THICKNESS, ATLAS_WHITE, flash); }

    if (snap->boss_win_active) draw_boss_window(snap, sx, sy);

    if (snap->state == SIM_STATE_MENU) draw_menu_screen();
    else if (snap->state == SIM_STATE_UPGRADE) draw_upgrade_screen(snap);
    else if (snap->state == SIM_STATE_DEAD) draw_dead_screen(snap);

    render_flush();  // draw call: frames (+ screen text), unscissored

    // Beams are drawn UNSCISSORED: lasers cross the void, which is exactly what
    // gives the boss reach before the windows merge. Bullets don't get this.
    for (uint16_t i = 0; i < snap->beam_count; i++) {
        draw_beam(&snap->beams[i], sx, sy);
    }
    render_flush();  // draw call: beams

    // --- pass A: the player's room, clipped hard at the playfield edge ---
    // draw order (§7.1): enemies -> player shots -> player -> enemy bullets
    // LAST, so bullets sit on top. Bullet visibility is the whole ballgame.
    render_set_scissor(left, top, w, h);
    draw_enemies(snap, sx, sy, false);
    draw_pshots(snap, sx, sy);
    draw_player(snap, sx, sy);
    draw_bullets(snap, sx, sy);
    render_flush();  // draw call: player's room
    render_clear_scissor();

    // --- pass B: the boss's room. Without its own scissor the boss would be
    // clipped away entirely whenever it sits outside the player's rect, which
    // is most of the fight. Quads landing in the overlap get drawn in both
    // passes; they're opaque, so it's idempotent.
    if (snap->boss_win_active) {
        render_set_scissor(snap->boss_win_x + sx, snap->boss_win_y + sy,
                            snap->boss_win_w, snap->boss_win_h);
        draw_enemies(snap, sx, sy, true);
        draw_pshots(snap, sx, sy);   // your shots reaching into its room
        draw_bullets(snap, sx, sy);
        render_flush();  // draw call: boss's room
        render_clear_scissor();
    }
}
