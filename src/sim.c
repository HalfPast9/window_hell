#include "sim.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "platform.h"  // KEY_* bitmask

// ---------------------------------------------------------------- window --

// Bounds one axis of the rect: first its size (PRD §8.1's 140..1180 / 140..660),
// then its *position* — pushing a single edge translates the whole rect, and
// without a positional bound the window drifts off the 1280x720 screen. Once
// an edge passes the screen the player's shots despawn (PSHOT_DESPAWN_MARGIN)
// before they can reach it, so that edge becomes unpushable and the window
// silently starts collapsing again. Screen bound preserves size by sliding
// both edges together.
static void clamp_axis(WindowEdge* lo, WindowEdge* hi, float min_size, float max_size,
                        float bound_lo, float bound_hi) {
    float size = hi->pos - lo->pos;
    if (size < min_size) {
        float delta = (min_size - size) * 0.5f;
        lo->pos -= delta;
        hi->pos += delta;
    } else if (size > max_size) {
        float delta = (size - max_size) * 0.5f;
        lo->pos += delta;
        hi->pos -= delta;
    }

    float shift = 0.0f;
    if (lo->pos < bound_lo)      shift = bound_lo - lo->pos;
    else if (hi->pos > bound_hi) shift = bound_hi - hi->pos;
    if (shift != 0.0f) {
        lo->pos += shift;
        hi->pos += shift;
        // Re-anchor the springs, else they keep integrating toward an
        // off-screen target and fight the clamp every tick.
        lo->target += shift;
        hi->target += shift;
        lo->vel = 0.0f;
        hi->vel = 0.0f;
    }

    // Targets also get bounded: push adds to target, so an unbounded target
    // would bank arbitrarily far off-screen and the edge would refuse to
    // creep back in for a long time after.
    if (lo->target < bound_lo) lo->target = bound_lo;
    if (hi->target > bound_hi) hi->target = bound_hi;
}

static void reset_window(Sim* sim, float scale, float push_amount_seed) {
    float w = WINDOW_START_W * scale, h = WINDOW_START_H * scale;
    float left = (INTERNAL_W - w) * 0.5f, top = (INTERNAL_H - h) * 0.5f;
    float vals[4] = { left, left + w, top, top + h };
    for (int i = 0; i < 4; i++) {
        sim->edges[i].pos = vals[i];
        sim->edges[i].vel = 0.0f;
        sim->edges[i].target = vals[i];
        sim->edges[i].push_amount = push_amount_seed;
        sim->edges[i].last_push_tick = 0;
        sim->edge_flash[i] = 0.0f;
    }
    sim->shake_x = sim->shake_y = 0.0f;
}

static void add_shake(Sim* sim, float magnitude) {
    float angle = rng_float01(&sim->rng) * 6.2831853f;
    sim->shake_x += cosf(angle) * magnitude;
    sim->shake_y += sinf(angle) * magnitude;
}

static void step_window(Sim* sim) {
    // The room closes faster while the boss is up (§8.3).
    float rate = sim->shrink_rate * (sim->spiker_alive ? SPIKER_SHRINK_MULT : 1.0f);
    sim->edges[EDGE_LEFT].target   += rate * SIM_DT;
    sim->edges[EDGE_RIGHT].target  -= rate * SIM_DT;
    sim->edges[EDGE_TOP].target    += rate * SIM_DT;
    sim->edges[EDGE_BOTTOM].target -= rate * SIM_DT;

    for (int i = 0; i < 4; i++) {
        WindowEdge* e = &sim->edges[i];
        float accel = -WINDOW_SPRING_K * (e->pos - e->target)
                       - 2.0f * WINDOW_SPRING_ZETA * sqrtf(WINDOW_SPRING_K) * e->vel;
        e->vel += accel * SIM_DT;
        e->pos += e->vel * SIM_DT;
    }

    clamp_axis(&sim->edges[EDGE_LEFT], &sim->edges[EDGE_RIGHT],
                WINDOW_MIN_W, WINDOW_MAX_W, 0.0f, INTERNAL_W);
    clamp_axis(&sim->edges[EDGE_TOP], &sim->edges[EDGE_BOTTOM],
                WINDOW_MIN_H, WINDOW_MAX_H, 0.0f, INTERNAL_H);

    for (int i = 0; i < 4; i++) {
        sim->edge_flash[i] -= 0.5f;  // ~2 ticks to zero
        if (sim->edge_flash[i] < 0.0f) sim->edge_flash[i] = 0.0f;
    }
    sim->shake_x *= SHAKE_DECAY;
    sim->shake_y *= SHAKE_DECAY;
}

// ---------------------------------------------------------------- player --

static uint16_t radians_to_aim_q(float radians) {
    float turns = radians / SIM_TAU;
    turns -= floorf(turns);  // wrap into [0,1)
    return (uint16_t)((int)(turns * AIM_Q_STEPS) & 0xFFFF);
}

static void aim_q_to_vector(uint16_t q, float* out_dx, float* out_dy) {
    float radians = (float)q * (SIM_TAU / AIM_Q_STEPS);
    *out_dx = cosf(radians);
    *out_dy = sinf(radians);
}

// Aim is fully decoupled from movement: it points at the mouse cursor,
// recomputed every tick regardless of whether the player is moving or firing.
// Degenerate case (cursor exactly on the player) keeps the previous aim.
//
// aim_dx/aim_dy are derived from the quantized aim_q, never from the raw
// cursor delta — that's what lets replay (which supplies aim_q directly)
// reproduce a live run bit-for-bit.
static void step_aim(Sim* sim) {
    if (sim->input.use_aim_q) {
        sim->aim_q = sim->input.aim_q;
    } else {
        float dx = sim->input.mouse_x - sim->player_x;
        float dy = sim->input.mouse_y - sim->player_y;
        if (dx * dx + dy * dy > 1e-6f) {
            sim->aim_q = radians_to_aim_q(atan2f(dy, dx));
        }
    }
    aim_q_to_vector(sim->aim_q, &sim->aim_dx, &sim->aim_dy);
}

// Returns true if the confinement clamp had to move the player this tick —
// i.e. an edge's true position reached where the player actually was. That
// IS the crush condition (PRD §8.1): the same mechanism that keeps the
// player "clamped inside the animated rect" is what detects the edge
// catching up to a player who didn't move away in time.
static bool step_player(Sim* sim) {
    uint32_t keys = sim->input.keys;
    float dx = 0.0f, dy = 0.0f;
    if (keys & KEY_LEFT)  dx -= 1.0f;
    if (keys & KEY_RIGHT) dx += 1.0f;
    if (keys & KEY_UP)    dy -= 1.0f;
    if (keys & KEY_DOWN)  dy += 1.0f;

    if (dx != 0.0f || dy != 0.0f) {
        float len = sqrtf(dx * dx + dy * dy);
        dx /= len;
        dy /= len;
        sim->move_dx = dx;  // drives the keyboard-shoot fallback direction
        sim->move_dy = dy;

        float speed = (keys & KEY_FOCUS) ? sim->player_focus_speed : sim->player_speed;
        sim->player_x += dx * speed * SIM_DT;
        sim->player_y += dy * speed * SIM_DT;
    }

    float min_x = sim->edges[EDGE_LEFT].pos + PLAYER_HITBOX_R;
    float max_x = sim->edges[EDGE_RIGHT].pos - PLAYER_HITBOX_R;
    float min_y = sim->edges[EDGE_TOP].pos + PLAYER_HITBOX_R;
    float max_y = sim->edges[EDGE_BOTTOM].pos - PLAYER_HITBOX_R;

    bool crushed = sim->player_x < min_x || sim->player_x > max_x ||
                   sim->player_y < min_y || sim->player_y > max_y;

    if (sim->player_x < min_x) sim->player_x = min_x;
    if (sim->player_x > max_x) sim->player_x = max_x;
    if (sim->player_y < min_y) sim->player_y = min_y;
    if (sim->player_y > max_y) sim->player_y = max_y;

    return crushed;
}

static void player_hit(Sim* sim, bool reset_win) {
    sim->lives--;
    sim->invuln_ticks = PLAYER_INVULN_TICKS;
    sim->hit_flash = 1.0f;
    add_shake(sim, PLAYER_HIT_SHAKE);
    if (HITSTOP_PLAYER_HIT_TICKS > sim->hitstop_ticks) sim->hitstop_ticks = HITSTOP_PLAYER_HIT_TICKS;

    if (reset_win) {
        reset_window(sim, WINDOW_RESET_SCALE, sim->push_base);
        sim->player_x = INTERNAL_W * 0.5f;
        sim->player_y = INTERNAL_H * 0.5f;
    }

    if (sim->lives <= 0) {
        sim->state = SIM_STATE_DEAD;
    }
}

// ------------------------------------------------------------- shooting --

static void spawn_pshot_volley(Sim* sim, float dx, float dy) {
    float perp_x = -dy, perp_y = dx;
    float vx = dx * PSHOT_SPEED, vy = dy * PSHOT_SPEED;

    int n = sim->multishot_count;
    for (int i = 0; i < n; i++) {
        if (sim->pshot_count >= MAX_PSHOTS) break;
        float offset = (i - (n - 1) * 0.5f) * PSHOT_OFFSET;
        float ox = perp_x * offset, oy = perp_y * offset;

        PShot* s = &sim->pshots[sim->pshot_count++];
        s->x = sim->player_x + ox;
        s->y = sim->player_y + oy;
        s->vx = vx;
        s->vy = vy;
        s->exited = false;
    }
}

static void step_shooting(Sim* sim) {
    if (sim->shoot_cooldown_ticks > 0) sim->shoot_cooldown_ticks--;
    if (sim->shoot_cooldown_ticks != 0) return;

    // Mouse is primary; KEY_SHOOT is the fallback kept alive because QNX
    // Screen pointer support is unverified on target (DESIGN_CHANGES.md §1).
    // Mouse fires toward the cursor, keyboard fires along last movement.
    if (sim->input.mouse_down) {
        spawn_pshot_volley(sim, sim->aim_dx, sim->aim_dy);
    } else if (sim->input.keys & KEY_SHOOT) {
        spawn_pshot_volley(sim, sim->move_dx, sim->move_dy);
    } else {
        return;
    }
    sim->shoot_cooldown_ticks = sim->fire_cooldown_ticks_base;
}

static void try_push_edge(Sim* sim, int edge_idx, float outward_sign) {
    WindowEdge* e = &sim->edges[edge_idx];
    uint64_t since = sim->tick - e->last_push_tick;
    float amount = (e->last_push_tick != 0 && since < WINDOW_PUSH_DECAY_WINDOW_TICKS)
                       ? e->push_amount * WINDOW_PUSH_DECAY
                       : sim->push_base;
    if (amount < WINDOW_PUSH_FLOOR) amount = WINDOW_PUSH_FLOOR;

    e->target += outward_sign * amount;
    e->push_amount = amount;
    e->last_push_tick = sim->tick;

    sim->edge_flash[edge_idx] = 1.0f;
    switch (edge_idx) {
        case EDGE_LEFT:   sim->shake_x -= 2.0f; break;
        case EDGE_RIGHT:  sim->shake_x += 2.0f; break;
        case EDGE_TOP:    sim->shake_y -= 2.0f; break;
        case EDGE_BOTTOM: sim->shake_y += 2.0f; break;
    }
}

static void step_pshots(Sim* sim) {
    for (uint16_t i = 0; i < sim->pshot_count;) {
        PShot* s = &sim->pshots[i];
        s->x += s->vx * SIM_DT;
        s->y += s->vy * SIM_DT;

        if (!s->exited) {
            bool crossed = false;
            if (s->x < sim->edges[EDGE_LEFT].pos)   { try_push_edge(sim, EDGE_LEFT, -1.0f);   crossed = true; }
            if (s->x > sim->edges[EDGE_RIGHT].pos)  { try_push_edge(sim, EDGE_RIGHT, 1.0f);   crossed = true; }
            if (s->y < sim->edges[EDGE_TOP].pos)    { try_push_edge(sim, EDGE_TOP, -1.0f);    crossed = true; }
            if (s->y > sim->edges[EDGE_BOTTOM].pos) { try_push_edge(sim, EDGE_BOTTOM, 1.0f);  crossed = true; }
            if (crossed) s->exited = true;
        }

        bool offscreen = s->x < -PSHOT_DESPAWN_MARGIN || s->x > INTERNAL_W + PSHOT_DESPAWN_MARGIN ||
                          s->y < -PSHOT_DESPAWN_MARGIN || s->y > INTERNAL_H + PSHOT_DESPAWN_MARGIN;
        if (offscreen) {
            sim->pshots[i] = sim->pshots[sim->pshot_count - 1];
            sim->pshot_count--;
        } else {
            i++;
        }
    }
}

// -------------------------------------------------------------- enemies --

static float enemy_radius(uint8_t type) {
    switch (type) {
        case ENEMY_TRIANGLE: return TRIANGLE_RADIUS;
        case ENEMY_CIRCLE:   return CIRCLE_RADIUS;
        case ENEMY_OCTAGON:  return OCTAGON_RADIUS;
        default:             return SPIKER_RADIUS;
    }
}

static uint32_t enemy_score_value(uint8_t type) {
    switch (type) {
        case ENEMY_TRIANGLE: return TRIANGLE_SCORE;
        case ENEMY_CIRCLE:   return CIRCLE_SCORE;
        case ENEMY_OCTAGON:  return OCTAGON_SCORE;
        default:             return SPIKER_SCORE;
    }
}

static float enemy_hp_max(uint8_t type) {
    switch (type) {
        case ENEMY_TRIANGLE: return TRIANGLE_HP;
        case ENEMY_CIRCLE:   return CIRCLE_HP;
        case ENEMY_OCTAGON:  return OCTAGON_HP;
        default:             return SPIKER_HP_BASE;
    }
}

// Only Triangle and Circle deal contact damage (§8.4). Octagon is excluded on
// purpose, and not just for flavor: it latches at edge+OCTAGON_RADIUS while
// the player clamps at edge+PLAYER_HITBOX_R, so their hitboxes necessarily
// overlap. A shrinking window would then squeeze the player into unavoidable,
// repeating contact damage against an Octagon's edge.
static bool enemy_deals_contact_damage(uint8_t type) {
    return type == ENEMY_TRIANGLE || type == ENEMY_CIRCLE;
}

static bool enemy_is_outside(const Sim* sim, const Enemy* e) {
    // The Spiker plays by different rules: it lives in its own window and its
    // vulnerability is decided by window overlap, not by the player's rect.
    if (e->type == ENEMY_SPIKER) return !sim->boss_vulnerable;

    return e->x < sim->edges[EDGE_LEFT].pos || e->x > sim->edges[EDGE_RIGHT].pos ||
           e->y < sim->edges[EDGE_TOP].pos  || e->y > sim->edges[EDGE_BOTTOM].pos;
}

// The boss window is a pure function of the boss's position — no separate
// state to keep in sync with a teleport.
static void boss_window_rect(float bx, float by, float* x, float* y) {
    *x = bx - BOSS_WINDOW_W * 0.5f;
    *y = by - BOSS_WINDOW_H * 0.5f;
}

static bool rects_overlap(float ax, float ay, float aw, float ah,
                           float bx, float by, float bw, float bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static bool point_in_boss_window(const Sim* sim, float x, float y) {
    if (!sim->boss_win_active) return false;
    return x >= sim->boss_win_x && x <= sim->boss_win_x + BOSS_WINDOW_W &&
           y >= sim->boss_win_y && y <= sim->boss_win_y + BOSS_WINDOW_H;
}

// Recomputed each tick from the live Spiker (if any).
static void update_boss_window(Sim* sim) {
    sim->boss_win_active = false;
    sim->boss_vulnerable = false;

    for (uint16_t i = 0; i < sim->enemy_count; i++) {
        const Enemy* e = &sim->enemies[i];
        if (e->type != ENEMY_SPIKER) continue;

        boss_window_rect(e->x, e->y, &sim->boss_win_x, &sim->boss_win_y);
        sim->boss_win_active = true;

        float px = sim->edges[EDGE_LEFT].pos, py = sim->edges[EDGE_TOP].pos;
        float pw = sim->edges[EDGE_RIGHT].pos - px, ph = sim->edges[EDGE_BOTTOM].pos - py;
        sim->boss_vulnerable = rects_overlap(px, py, pw, ph,
                                              sim->boss_win_x, sim->boss_win_y,
                                              BOSS_WINDOW_W, BOSS_WINDOW_H);
        break;  // only ever one Spiker
    }
}

static void step_enemy_triangle(Sim* sim, Enemy* e) {
    float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 0.0001f) { dx /= dist; dy /= dist; } else { dx = 0.0f; dy = 0.0f; }

    float speed = TRIANGLE_SPEED;
    if (dist < TRIANGLE_SLOW_RADIUS) {
        float frac = dist / TRIANGLE_SLOW_RADIUS;
        if (frac < TRIANGLE_MIN_SPEED_FRAC) frac = TRIANGLE_MIN_SPEED_FRAC;
        speed *= frac;
    }
    e->x += dx * speed * SIM_DT;
    e->y += dy * speed * SIM_DT;
}

static void step_enemy_circle(Sim* sim, Enemy* e) {
    switch (e->dash_state) {
        case CIRCLE_IDLE: {
            float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > 0.0001f) { dx /= dist; dy /= dist; }
            e->x += dx * CIRCLE_APPROACH_SPEED * SIM_DT;
            e->y += dy * CIRCLE_APPROACH_SPEED * SIM_DT;
            if (--e->dash_timer_ticks <= 0) {
                e->dash_state = CIRCLE_WINDUP;
                e->dash_timer_ticks = CIRCLE_WINDUP_TICKS;
            }
            break;
        }
        case CIRCLE_WINDUP: {
            if (--e->dash_timer_ticks <= 0) {
                float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 0.0001f) { dx = 0.0f; dy = -1.0f; dist = 1.0f; }
                dx /= dist;
                dy /= dist;
                e->dash_vx = dx * CIRCLE_DASH_SPEED;
                e->dash_vy = dy * CIRCLE_DASH_SPEED;
                e->dash_state = CIRCLE_DASHING;
                e->dash_timer_ticks = CIRCLE_DASH_TICKS;
            }
            break;
        }
        case CIRCLE_DASHING: {
            e->x += e->dash_vx * SIM_DT;
            e->y += e->dash_vy * SIM_DT;
            if (--e->dash_timer_ticks <= 0) {
                e->dash_state = CIRCLE_IDLE;
                e->dash_timer_ticks = CIRCLE_IDLE_TICKS;
            }
            break;
        }
        default: break;
    }
}

static void spawn_bullet(Sim* sim, float x, float y, float vx, float vy) {
    if (sim->bullet_count >= MAX_BULLETS) return;
    Bullet* b = &sim->bullets[sim->bullet_count++];
    b->x = x; b->y = y; b->vx = vx; b->vy = vy;
    b->age_ticks = 0;
}

// Drifts to the nearest window edge, latches onto it like a limpet, then
// fires aimed shots at the player on a timer (PRD §8.3).
static void step_enemy_octagon(Sim* sim, Enemy* e) {
    if (!e->latched) {
        float dl = fabsf(e->x - sim->edges[EDGE_LEFT].pos);
        float dr = fabsf(sim->edges[EDGE_RIGHT].pos - e->x);
        float dt = fabsf(e->y - sim->edges[EDGE_TOP].pos);
        float db = fabsf(sim->edges[EDGE_BOTTOM].pos - e->y);

        float best = dl; uint8_t edge = EDGE_LEFT;
        if (dr < best) { best = dr; edge = EDGE_RIGHT; }
        if (dt < best) { best = dt; edge = EDGE_TOP; }
        if (db < best) { best = db; edge = EDGE_BOTTOM; }
        e->latch_edge = edge;

        float tx = e->x, ty = e->y;
        switch (edge) {
            case EDGE_LEFT:   tx = sim->edges[EDGE_LEFT].pos + OCTAGON_RADIUS;   break;
            case EDGE_RIGHT:  tx = sim->edges[EDGE_RIGHT].pos - OCTAGON_RADIUS;  break;
            case EDGE_TOP:    ty = sim->edges[EDGE_TOP].pos + OCTAGON_RADIUS;    break;
            default:          ty = sim->edges[EDGE_BOTTOM].pos - OCTAGON_RADIUS; break;
        }

        float dx = tx - e->x, dy = ty - e->y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= OCTAGON_LATCH_EPS) {
            e->latched = true;
            e->x = tx;
            e->y = ty;
        } else {
            e->x += (dx / dist) * OCTAGON_DRIFT_SPEED * SIM_DT;
            e->y += (dy / dist) * OCTAGON_DRIFT_SPEED * SIM_DT;
        }
        return;  // no firing while still closing on its edge
    }

    // Latched: ride the edge as it springs/shrinks, so it stays stuck to it.
    // Both axes matter. Pinning only the latched axis lets the *perpendicular*
    // edges creep past it as the window shrinks — the Octagon then counts as
    // "outside", which means dim (0.30 tint) and invulnerable, i.e. it looks
    // dead but isn't, and reappears when you push that wall back over it.
    float left = sim->edges[EDGE_LEFT].pos + OCTAGON_RADIUS;
    float right = sim->edges[EDGE_RIGHT].pos - OCTAGON_RADIUS;
    float top = sim->edges[EDGE_TOP].pos + OCTAGON_RADIUS;
    float bottom = sim->edges[EDGE_BOTTOM].pos - OCTAGON_RADIUS;

    switch (e->latch_edge) {
        case EDGE_LEFT:   e->x = left;   break;
        case EDGE_RIGHT:  e->x = right;  break;
        case EDGE_TOP:    e->y = top;    break;
        default:          e->y = bottom; break;
    }

    // slide along the edge so it stays within the window's other dimension
    if (e->latch_edge == EDGE_LEFT || e->latch_edge == EDGE_RIGHT) {
        if (top <= bottom) {
            if (e->y < top) e->y = top;
            if (e->y > bottom) e->y = bottom;
        }
    } else {
        if (left <= right) {
            if (e->x < left) e->x = left;
            if (e->x > right) e->x = right;
        }
    }

    if (--e->fire_timer_ticks <= 0) {
        e->fire_timer_ticks = OCTAGON_FIRE_INTERVAL_TICKS;
        float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 0.0001f) { dx = 0.0f; dy = 1.0f; dist = 1.0f; }
        spawn_bullet(sim, e->x, e->y,
                      (dx / dist) * ENEMY_BULLET_SPEED,
                      (dy / dist) * ENEMY_BULLET_SPEED);
    }
}

// Teleports anywhere on screen — it is NOT confined to the player's window.
// The only constraint is that its own window stays fully on screen. Getting
// back within reach is the player's problem, solved by pushing walls outward.
static void spiker_teleport(Sim* sim, Enemy* e) {
    float half_w = BOSS_WINDOW_W * 0.5f + BOSS_WINDOW_SCREEN_MARGIN;
    float half_h = BOSS_WINDOW_H * 0.5f + BOSS_WINDOW_SCREEN_MARGIN;
    float lo_x = half_w, hi_x = INTERNAL_W - half_w;
    float lo_y = half_h, hi_y = INTERNAL_H - half_h;

    e->x = lo_x + rng_float01(&sim->rng) * (hi_x - lo_x);
    e->y = lo_y + rng_float01(&sim->rng) * (hi_y - lo_y);

    e->damage_since_blink = 0.0f;
    e->spiker_wave_counter++;   // attacks speed up with each blink
    e->hit_flash_ticks = 2;     // brief flash at the new position (§8.6)
    add_shake(sim, SPIKER_TELEPORT_SHAKE);
}

// Immobile but teleports. Passive: radial bullet waves on a timer that tightens
// as its wave counter climbs. Active: a telegraphed laser that then sweeps.
static void spawn_blaster_volley(Sim* sim) {
    for (int n = 0; n < BLASTER_VOLLEY; n++) {
        Blaster* b = NULL;
        for (int i = 0; i < MAX_BLASTERS; i++) {
            if (!sim->blasters[i].active) { b = &sim->blasters[i]; break; }
        }
        if (!b) return;  // all in flight

        // Somewhere on screen, but never right on top of the player — the
        // telegraph must be dodgeable, and it can't be dodged from inside it.
        float bx, by, dx, dy, d2;
        int tries = 0;
        do {
            bx = rng_float01(&sim->rng) * INTERNAL_W;
            by = rng_float01(&sim->rng) * INTERNAL_H;
            dx = bx - sim->player_x;
            dy = by - sim->player_y;
            d2 = dx * dx + dy * dy;
        } while (d2 < BLASTER_SPAWN_MIN_DIST * BLASTER_SPAWN_MIN_DIST && ++tries < 8);

        // Lock onto where the player is NOW and never re-aim.
        float adx = sim->player_x - bx, ady = sim->player_y - by;
        float len = sqrtf(adx * adx + ady * ady);
        if (len < 0.0001f) { adx = 1.0f; ady = 0.0f; len = 1.0f; }

        b->x = bx;
        b->y = by;
        b->angle = atan2f(ady, adx);
        b->state = SPIKER_LASER_TELEGRAPH;
        b->timer_ticks = BLASTER_TELEGRAPH_TICKS;
        b->active = true;
    }
}

static void step_blasters(Sim* sim) {
    for (int i = 0; i < MAX_BLASTERS; i++) {
        Blaster* b = &sim->blasters[i];
        if (!b->active) continue;
        if (--b->timer_ticks > 0) continue;

        if (b->state == SPIKER_LASER_TELEGRAPH) {
            b->state = SPIKER_LASER_FIRING;
            b->timer_ticks = BLASTER_FIRE_TICKS;
            add_shake(sim, 1.5f);
        } else {
            b->active = false;
        }
    }
}

static void step_enemy_spiker(Sim* sim, Enemy* e) {
    // No clamp to the player's window: the boss roams the whole screen and
    // carries its own window with it (see update_boss_window).

    // --- blaster volleys: the boss's reach before the windows merge ---
    if (--sim->blaster_timer_ticks <= 0) {
        sim->blaster_timer_ticks = BLASTER_INTERVAL_TICKS;
        spawn_blaster_volley(sim);
    }

    // --- passive: radial wave ---
    if (--e->fire_timer_ticks <= 0) {
        int interval = SPIKER_RADIAL_INTERVAL_TICKS;
        for (int i = 0; i < e->spiker_wave_counter; i++) {
            interval = (int)(interval * SPIKER_RADIAL_SPEEDUP);
        }
        if (interval < SPIKER_RADIAL_MIN_TICKS) interval = SPIKER_RADIAL_MIN_TICKS;
        e->fire_timer_ticks = interval;

        // offset each wave so successive rings don't overlap into corridors
        float phase = rng_float01(&sim->rng) * (SIM_TAU / SPIKER_RADIAL_BULLETS);
        for (int i = 0; i < SPIKER_RADIAL_BULLETS; i++) {
            float a = phase + (float)i * (SIM_TAU / SPIKER_RADIAL_BULLETS);
            spawn_bullet(sim, e->x, e->y, cosf(a) * ENEMY_BULLET_SPEED, sinf(a) * ENEMY_BULLET_SPEED);
        }
    }

    // --- active: locked-aim laser. Aim is fixed the moment the telegraph
    // starts and never re-aims, so the telegraph is a real dodge window: step
    // off the line and the beam misses.
    switch (e->laser_state) {
        case SPIKER_LASER_IDLE:
            if (--e->laser_timer_ticks <= 0) {
                float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
                if (dx * dx + dy * dy < 0.0001f) { dx = 1.0f; dy = 0.0f; }
                e->laser_angle = atan2f(dy, dx);
                e->laser_state = SPIKER_LASER_TELEGRAPH;
                e->laser_timer_ticks = SPIKER_LASER_TELEGRAPH_TICKS;
            }
            break;
        case SPIKER_LASER_TELEGRAPH:
            if (--e->laser_timer_ticks <= 0) {
                e->laser_state = SPIKER_LASER_FIRING;
                e->laser_timer_ticks = SPIKER_LASER_FIRE_TICKS;
                add_shake(sim, 3.0f);
            }
            break;
        case SPIKER_LASER_FIRING:
            if (--e->laser_timer_ticks <= 0) {
                e->laser_state = SPIKER_LASER_IDLE;
                e->laser_timer_ticks = SPIKER_LASER_INTERVAL_TICKS;
            }
            break;
        default: break;
    }
}

// Rebuilt from scratch each tick — beams are a pure view of Spiker + blaster
// state, so nothing can linger once its source is gone.
static void rebuild_beams(Sim* sim) {
    sim->beam_count = 0;

    for (uint16_t i = 0; i < sim->enemy_count && sim->beam_count < MAX_BEAMS; i++) {
        const Enemy* e = &sim->enemies[i];
        if (e->type != ENEMY_SPIKER || e->laser_state == SPIKER_LASER_IDLE) continue;

        SnapBeam* b = &sim->beams[sim->beam_count++];
        b->x = e->x;
        b->y = e->y;
        b->angle = e->laser_angle;
        b->len = SPIKER_LASER_LEN;
        b->width = SPIKER_LASER_WIDTH;
        b->state = (e->laser_state == SPIKER_LASER_FIRING) ? 1u : 0u;
    }

    for (int i = 0; i < MAX_BLASTERS && sim->beam_count < MAX_BEAMS; i++) {
        const Blaster* bl = &sim->blasters[i];
        if (!bl->active) continue;

        SnapBeam* b = &sim->beams[sim->beam_count++];
        b->x = bl->x;
        b->y = bl->y;
        b->angle = bl->angle;
        b->len = BLASTER_LEN;
        b->width = BLASTER_WIDTH;
        b->state = (bl->state == SPIKER_LASER_FIRING) ? 1u : 0u;
    }
}

static void step_enemies(Sim* sim) {
    const float inset = 4.0f;
    for (uint16_t i = 0; i < sim->enemy_count; i++) {
        Enemy* e = &sim->enemies[i];
        switch (e->type) {
            case ENEMY_TRIANGLE: step_enemy_triangle(sim, e); break;
            case ENEMY_CIRCLE:   step_enemy_circle(sim, e);   break;
            case ENEMY_OCTAGON:  step_enemy_octagon(sim, e);  break;
            default:             step_enemy_spiker(sim, e);   break;
        }

        if (e->x < inset) e->x = inset;
        if (e->x > INTERNAL_W - inset) e->x = INTERNAL_W - inset;
        if (e->y < inset) e->y = inset;
        if (e->y > INTERNAL_H - inset) e->y = INTERNAL_H - inset;

        if (e->hit_flash_ticks > 0) e->hit_flash_ticks--;
    }
}

// Enemy bullets live only inside a window: crossing into the void despawns them
// instantly (PRD §8.1) — that's what makes the void read as hostile. With a boss
// on the field there are two windows, so a bullet survives inside EITHER. The
// boss's radial waves therefore fill its own room and only spill into yours once
// the windows overlap: until you commit to the merge, neither of you can reach
// the other with bullets (lasers still cross the void).
static void step_bullets(Sim* sim) {
    float left = sim->edges[EDGE_LEFT].pos, right = sim->edges[EDGE_RIGHT].pos;
    float top = sim->edges[EDGE_TOP].pos, bottom = sim->edges[EDGE_BOTTOM].pos;

    for (uint16_t i = 0; i < sim->bullet_count;) {
        Bullet* b = &sim->bullets[i];
        b->x += b->vx * SIM_DT;
        b->y += b->vy * SIM_DT;
        if (b->age_ticks < 255) b->age_ticks++;

        if (sim->stress_mode) {
            // Bounce off the fixed screen bounds instead of the window edges:
            // a stable, gameplay-independent load for the M3 perf test.
            if (b->x < 0.0f)       { b->x = 0.0f;       b->vx = -b->vx; }
            if (b->x > INTERNAL_W) { b->x = INTERNAL_W; b->vx = -b->vx; }
            if (b->y < 0.0f)       { b->y = 0.0f;       b->vy = -b->vy; }
            if (b->y > INTERNAL_H) { b->y = INTERNAL_H; b->vy = -b->vy; }
            i++;
            continue;
        }

        bool in_player_win = b->x >= left && b->x <= right && b->y >= top && b->y <= bottom;
        if (!in_player_win && !point_in_boss_window(sim, b->x, b->y)) {
            sim->bullets[i] = sim->bullets[sim->bullet_count - 1];
            sim->bullet_count--;
        } else {
            i++;
        }
    }
}

static void spawn_enemy(Sim* sim, uint8_t type) {
    if (sim->enemy_count >= MAX_ENEMIES) return;

    float left = sim->edges[EDGE_LEFT].pos, right = sim->edges[EDGE_RIGHT].pos;
    float top = sim->edges[EDGE_TOP].pos, bottom = sim->edges[EDGE_BOTTOM].pos;

    int edge = rng_range_i(&sim->rng, 0, 3);
    float x, y;
    switch (edge) {
        case EDGE_LEFT:
            x = left - ENEMY_SPAWN_MARGIN;
            y = top + rng_float01(&sim->rng) * (bottom - top);
            break;
        case EDGE_RIGHT:
            x = right + ENEMY_SPAWN_MARGIN;
            y = top + rng_float01(&sim->rng) * (bottom - top);
            break;
        case EDGE_TOP:
            x = left + rng_float01(&sim->rng) * (right - left);
            y = top - ENEMY_SPAWN_MARGIN;
            break;
        default:  // EDGE_BOTTOM
            x = left + rng_float01(&sim->rng) * (right - left);
            y = bottom + ENEMY_SPAWN_MARGIN;
            break;
    }
    const float inset = 10.0f;
    if (x < inset) x = inset;
    if (x > INTERNAL_W - inset) x = INTERNAL_W - inset;
    if (y < inset) y = inset;
    if (y > INTERNAL_H - inset) y = INTERNAL_H - inset;

    Enemy* e = &sim->enemies[sim->enemy_count++];
    e->x = x;
    e->y = y;
    e->type = type;
    e->hp = enemy_hp_max(type);
    e->dash_state = CIRCLE_IDLE;
    e->dash_timer_ticks = CIRCLE_IDLE_TICKS;
    e->dash_vx = e->dash_vy = 0.0f;
    e->latch_edge = EDGE_LEFT;
    e->latched = false;
    e->fire_timer_ticks = OCTAGON_FIRE_INTERVAL_TICKS;
    e->laser_state = SPIKER_LASER_IDLE;
    e->laser_timer_ticks = SPIKER_LASER_INTERVAL_TICKS;
    e->laser_angle = 0.0f;
    e->spiker_wave_counter = 0;
    e->damage_since_blink = 0.0f;
    e->hit_flash_ticks = 0;

    if (type == ENEMY_SPIKER) {
        // Arrives already merged with the player's window (centered on it), so
        // the fight opens in contact; every teleport after that is the boss
        // running and the player having to push walls to catch up.
        e->x = (sim->edges[EDGE_LEFT].pos + sim->edges[EDGE_RIGHT].pos) * 0.5f;
        e->y = (sim->edges[EDGE_TOP].pos + sim->edges[EDGE_BOTTOM].pos) * 0.5f;
        sim->blaster_timer_ticks = BLASTER_INTERVAL_TICKS;
        float hp = SPIKER_HP_BASE;
        for (int i = 1; i < sim->spiker_appearances; i++) hp *= SPIKER_HP_GROWTH;
        e->hp = hp;
        e->fire_timer_ticks = SPIKER_RADIAL_INTERVAL_TICKS;
        e->hit_flash_ticks = 3;
        sim->spiker_alive = true;
        add_shake(sim, SPIKER_TELEPORT_SHAKE);
    }
}

static void begin_wave(Sim* sim, int wave_n) {
    // Every 5th wave is the boss instead of the normal spawn set (§8.3).
    if (wave_n % SPIKER_WAVE_INTERVAL == 0) {
        sim->spiker_appearances++;
        sim->spawn_queue[0].type = ENEMY_SPIKER;
        sim->spawn_queue_count = 1;
        sim->spawn_queue_index = 0;
        sim->spawn_timer_ticks = 0;
        return;
    }

    int n_tri = 2 + wave_n / 2;
    int n_cir = wave_n / 3;
    int n_oct = (wave_n >= 3) ? wave_n / 4 : 0;
    int total = n_tri + n_cir + n_oct;
    if (total > SPAWN_QUEUE_CAP) total = SPAWN_QUEUE_CAP;

    // Round-robin so the types interleave in the spawn order rather than
    // arriving in three separate clumps.
    int ti = 0, ci = 0, oi = 0, count = 0;
    while (count < total && (ti < n_tri || ci < n_cir || oi < n_oct)) {
        if (ti < n_tri && count < total) { sim->spawn_queue[count++].type = ENEMY_TRIANGLE; ti++; }
        if (ci < n_cir && count < total) { sim->spawn_queue[count++].type = ENEMY_CIRCLE; ci++; }
        if (oi < n_oct && count < total) { sim->spawn_queue[count++].type = ENEMY_OCTAGON; oi++; }
    }
    sim->spawn_queue_count = count;
    sim->spawn_queue_index = 0;
    sim->spawn_timer_ticks = 0;
}

static void step_spawn_queue(Sim* sim) {
    if (sim->spawn_queue_index >= sim->spawn_queue_count) return;
    if (sim->spawn_timer_ticks > 0) { sim->spawn_timer_ticks--; return; }

    spawn_enemy(sim, sim->spawn_queue[sim->spawn_queue_index].type);
    sim->spawn_queue_index++;
    sim->spawn_timer_ticks = SPAWN_INTERVAL_TICKS + rng_range_i(&sim->rng, 0, SPAWN_INTERVAL_JITTER_TICKS);
}

// ------------------------------------------------------------ collisions --

static void step_collisions(Sim* sim) {
    for (uint16_t pi = 0; pi < sim->pshot_count;) {
        PShot* s = &sim->pshots[pi];
        bool consumed = false;
        for (uint16_t ei = 0; ei < sim->enemy_count; ei++) {
            Enemy* e = &sim->enemies[ei];
            if (enemy_is_outside(sim, e)) continue;  // outside window = invulnerable
            float r = enemy_radius(e->type) + PSHOT_HIT_RADIUS;
            float dx = s->x - e->x, dy = s->y - e->y;
            if (dx * dx + dy * dy <= r * r) {
                e->hp -= PSHOT_DAMAGE;
                e->hit_flash_ticks = ENEMY_HIT_FLASH_TICKS;
                if (e->type == ENEMY_SPIKER && e->hp > 0.0f) {
                    e->damage_since_blink += PSHOT_DAMAGE;
                    if (e->damage_since_blink >= SPIKER_TELEPORT_DAMAGE) {
                        spiker_teleport(sim, e);
                    }
                }
                consumed = true;
                break;
            }
        }
        if (consumed) {
            sim->pshots[pi] = sim->pshots[sim->pshot_count - 1];
            sim->pshot_count--;
        } else {
            pi++;
        }
    }

    for (uint16_t ei = 0; ei < sim->enemy_count;) {
        if (sim->enemies[ei].hp <= 0.0f) {
            if (sim->enemies[ei].type == ENEMY_SPIKER) sim->spiker_alive = false;
            sim->score += enemy_score_value(sim->enemies[ei].type);
            add_shake(sim, ENEMY_KILL_SHAKE);
            if (HITSTOP_ENEMY_KILL_TICKS > sim->hitstop_ticks) sim->hitstop_ticks = HITSTOP_ENEMY_KILL_TICKS;
            sim->enemies[ei] = sim->enemies[sim->enemy_count - 1];
            sim->enemy_count--;
        } else {
            ei++;
        }
    }

    if (sim->invuln_ticks <= 0) {
        // Spiker laser x player (§8.4): point-to-segment distance, firing beams only
        for (uint16_t bi = 0; bi < sim->beam_count; bi++) {
            const SnapBeam* beam = &sim->beams[bi];
            if (beam->state != 1u) continue;  // telegraph doesn't hurt

            float ex = cosf(beam->angle), ey = sinf(beam->angle);
            float px = sim->player_x - beam->x, py = sim->player_y - beam->y;
            float t = px * ex + py * ey;               // projection onto the beam ray
            if (t < 0.0f) t = 0.0f;
            if (t > beam->len) t = beam->len;
            float cx = px - t * ex, cy = py - t * ey;  // perpendicular offset
            float r = beam->width * 0.5f + PLAYER_HITBOX_R;
            if (cx * cx + cy * cy <= r * r) {
                player_hit(sim, false);
                break;
            }
        }
    }

    if (sim->invuln_ticks <= 0) {
        // enemy bullets x player (§8.4)
        for (uint16_t bi = 0; bi < sim->bullet_count; bi++) {
            const Bullet* b = &sim->bullets[bi];
            float r = ENEMY_BULLET_RADIUS + PLAYER_HITBOX_R;
            float dx = sim->player_x - b->x, dy = sim->player_y - b->y;
            if (dx * dx + dy * dy <= r * r) {
                player_hit(sim, false);
                break;
            }
        }
    }

    if (sim->invuln_ticks <= 0) {
        // enemy body x player, contact damage (§8.4)
        for (uint16_t ei = 0; ei < sim->enemy_count; ei++) {
            Enemy* e = &sim->enemies[ei];
            if (!enemy_deals_contact_damage(e->type)) continue;
            if (enemy_is_outside(sim, e)) continue;
            float r = enemy_radius(e->type) + PLAYER_HITBOX_R;
            float dx = sim->player_x - e->x, dy = sim->player_y - e->y;
            if (dx * dx + dy * dy <= r * r) {
                player_hit(sim, false);
                break;
            }
        }
    }
}

// ------------------------------------------------------------- upgrades --

static void begin_upgrade_state(Sim* sim) {
    // The arena goes quiet for the pick. Beams are a view of a Spiker that no
    // longer exists, and step_play (which rebuilds them) doesn't run during
    // UPGRADE — leaving them would freeze a dead boss's laser on screen for the
    // whole 3 s. Bullets get the same treatment: they'd hang mid-air and then
    // strike the instant the next wave resumed the sim.
    sim->beam_count = 0;
    sim->bullet_count = 0;
    for (int i = 0; i < MAX_BLASTERS; i++) sim->blasters[i].active = false;
    sim->boss_win_active = false;
    sim->boss_vulnerable = false;

    int a = rng_range_i(&sim->rng, 0, UPGRADE_COUNT - 1);
    int b;
    do { b = rng_range_i(&sim->rng, 0, UPGRADE_COUNT - 1); } while (b == a);

    sim->upgrade_a = (uint8_t)a;
    sim->upgrade_b = (uint8_t)b;
    sim->upgrade_selected = 0;
    sim->upgrade_timer_ticks = UPGRADE_STATE_TICKS;
    sim->state = SIM_STATE_UPGRADE;
}

static void apply_upgrade(Sim* sim, uint8_t id) {
    switch (id) {
        case 0:  // Speed
            sim->player_speed *= 1.10f;
            sim->player_focus_speed *= 1.10f;
            break;
        case 1: {  // Fire Rate
            int c = (int)(sim->fire_cooldown_ticks_base * 0.88f);
            sim->fire_cooldown_ticks_base = c < 1 ? 1 : c;
            break;
        }
        case 2:  // +1 Multi Shot
            sim->multishot_count += 1;
            break;
        case 3:  // Max Health
            sim->lives_max += 1;
            sim->lives += 1;
            if (sim->lives > sim->lives_max) sim->lives = sim->lives_max;
            break;
        case 4:  // Wall Punch
            sim->push_base += UPGRADE_WALL_PUNCH_ADD;
            break;
        case 5:  // Bulwark
            sim->shrink_rate *= 0.85f;
            break;
        default: break;
    }
}

// ------------------------------------------------------- state machine --

static void reset_common(Sim* sim) {
    reset_window(sim, 1.0f, WINDOW_PUSH_BASE_DEFAULT);
    sim->player_x = INTERNAL_W * 0.5f;
    sim->player_y = INTERNAL_H * 0.5f;
    // default aim: straight up (screen y grows downward, so -tau/4)
    sim->aim_q = radians_to_aim_q(-SIM_TAU * 0.25f);
    aim_q_to_vector(sim->aim_q, &sim->aim_dx, &sim->aim_dy);
    sim->move_dx = 0.0f;
    sim->move_dy = -1.0f;  // keyboard-shoot fallback direction, default up
    sim->shoot_cooldown_ticks = 0;
    sim->invuln_ticks = 0;
    sim->hitstop_ticks = 0;
    sim->hit_flash = 0.0f;
    sim->pshot_count = 0;
    sim->enemy_count = 0;
    sim->bullet_count = 0;
    sim->beam_count = 0;
    sim->spiker_alive = false;
    sim->boss_win_active = false;
    sim->boss_vulnerable = false;
    for (int i = 0; i < MAX_BLASTERS; i++) sim->blasters[i].active = false;
    sim->blaster_timer_ticks = BLASTER_INTERVAL_TICKS;
    sim->spawn_queue_count = 0;
    sim->spawn_queue_index = 0;
    sim->spawn_timer_ticks = 0;
}

static void start_new_run(Sim* sim) {
    reset_common(sim);

    sim->player_speed = PLAYER_SPEED_DEFAULT;
    sim->player_focus_speed = PLAYER_FOCUS_SPEED_DEFAULT;
    sim->fire_cooldown_ticks_base = (int)(SIM_HZ / PSHOT_FIRE_HZ_DEFAULT);
    sim->multishot_count = PSHOT_MULTISHOT_DEFAULT;
    sim->lives_max = LIVES_START;
    sim->push_base = WINDOW_PUSH_BASE_DEFAULT;
    sim->shrink_rate = WINDOW_SHRINK_RATE_DEFAULT;

    sim->lives = sim->lives_max;
    sim->score = 0;
    sim->wave = 1;
    sim->spiker_appearances = 0;
    begin_wave(sim, sim->wave);
    sim->state = SIM_STATE_PLAY;
}

static void reset_to_menu(Sim* sim) {
    reset_common(sim);
    sim->state = SIM_STATE_MENU;
}

// "shoot was just pressed this tick", from either trigger — used by the UI
// states (start run, confirm upgrade) where a click and a keypress mean the
// same thing.
static bool shoot_pressed_edge(const Sim* sim) {
    bool key_edge = (sim->input.keys & KEY_SHOOT) && !(sim->prev_keys & KEY_SHOOT);
    bool click_edge = sim->input.mouse_down && !sim->prev_mouse_down;
    return key_edge || click_edge;
}

static void step_menu(Sim* sim) {
    if (shoot_pressed_edge(sim)) {
        start_new_run(sim);
    }
}

static void step_upgrade_state(Sim* sim) {
    uint32_t keys = sim->input.keys, prev = sim->prev_keys;
    if ((keys & KEY_LEFT) && !(prev & KEY_LEFT)) sim->upgrade_selected = 0;
    if ((keys & KEY_RIGHT) && !(prev & KEY_RIGHT)) sim->upgrade_selected = 1;

    sim->upgrade_timer_ticks--;
    bool confirmed = shoot_pressed_edge(sim) || sim->upgrade_timer_ticks <= 0;
    if (confirmed) {
        apply_upgrade(sim, sim->upgrade_selected == 0 ? sim->upgrade_a : sim->upgrade_b);
        sim->wave++;
        begin_wave(sim, sim->wave);
        sim->state = SIM_STATE_PLAY;
    }
}

static void step_play(Sim* sim) {
    if (sim->hitstop_ticks > 0) {
        sim->hitstop_ticks--;
        return;
    }

    step_window(sim);

    bool crushed = step_player(sim);
    if (crushed && sim->invuln_ticks <= 0) {
        player_hit(sim, true);
    }

    step_aim(sim);  // after movement: aim from the player's final position this tick
    step_shooting(sim);
    step_pshots(sim);
    step_spawn_queue(sim);
    step_enemies(sim);
    step_blasters(sim);
    // Boss window follows the boss, and decides vulnerability + where bullets
    // may live — so it must be current before bullets and collisions run.
    update_boss_window(sim);
    rebuild_beams(sim);   // after enemies move: beams follow this tick's state
    step_bullets(sim);
    step_collisions(sim);

    if (sim->invuln_ticks > 0) sim->invuln_ticks--;
    if (sim->hit_flash > 0.0f) {
        sim->hit_flash -= 0.1f;
        if (sim->hit_flash < 0.0f) sim->hit_flash = 0.0f;
    }

    if (sim->state == SIM_STATE_PLAY &&
        sim->spawn_queue_index >= sim->spawn_queue_count && sim->enemy_count == 0) {
        begin_upgrade_state(sim);
    }
}

// ------------------------------------------------------------ public API --

void sim_init(Sim* sim, uint64_t seed) {
    // Zero everything, including struct padding and the unused tails of the
    // fixed-capacity pools: sim_hash() hashes the raw bytes of this struct,
    // so any uninitialized byte would make the determinism check flaky.
    memset(sim, 0, sizeof(*sim));

    sim->tick = 0;
    sim->seed = seed;
    rng_seed(&sim->rng, seed);
    sim->input.keys = 0;
    // Start the cursor at screen center so the first tick's aim is
    // well-defined even before any pointer motion arrives.
    sim->input.mouse_x = INTERNAL_W * 0.5f;
    sim->input.mouse_y = INTERNAL_H * 0.5f;
    sim->input.mouse_down = false;
    sim->prev_keys = 0;
    sim->prev_mouse_down = false;

    sim->player_speed = PLAYER_SPEED_DEFAULT;
    sim->player_focus_speed = PLAYER_FOCUS_SPEED_DEFAULT;
    sim->fire_cooldown_ticks_base = (int)(SIM_HZ / PSHOT_FIRE_HZ_DEFAULT);
    sim->multishot_count = PSHOT_MULTISHOT_DEFAULT;
    sim->lives_max = LIVES_START;
    sim->push_base = WINDOW_PUSH_BASE_DEFAULT;
    sim->shrink_rate = WINDOW_SHRINK_RATE_DEFAULT;

    sim->lives = sim->lives_max;
    sim->score = 0;
    sim->wave = 0;

    reset_common(sim);
    sim->state = SIM_STATE_MENU;
}

void sim_start_at_wave(Sim* sim, int wave) {
    if (wave < 1) wave = 1;
    start_new_run(sim);
    sim->wave = wave;
    // Match what the wave counter implies: every 5th wave is a boss, so a run
    // started at wave 10 is the Spiker's second appearance (and gets its HP buff).
    sim->spiker_appearances = wave / SPIKER_WAVE_INTERVAL;
    if (wave % SPIKER_WAVE_INTERVAL == 0) sim->spiker_appearances--;  // begin_wave increments
    begin_wave(sim, wave);
}

void sim_start_stress(Sim* sim, int n) {
    start_new_run(sim);
    sim->stress_mode = true;
    if (n < 0) n = 0;
    if (n > MAX_BULLETS) n = MAX_BULLETS;
    for (int i = 0; i < n; i++) {
        Bullet* b = &sim->bullets[i];
        float angle = rng_float01(&sim->rng) * SIM_TAU;
        float speed = 200.0f + rng_float01(&sim->rng) * 200.0f;  // 200-400 px/s
        b->x = rng_float01(&sim->rng) * INTERNAL_W;
        b->y = rng_float01(&sim->rng) * INTERNAL_H;
        b->vx = cosf(angle) * speed;
        b->vy = sinf(angle) * speed;
        b->age_ticks = 255;  // fully spawned-in already, skip the pop animation
    }
    sim->bullet_count = (uint16_t)n;
}

void sim_consume_input(Sim* sim, InputRing* ring) {
    InputFrame f;
    while (input_ring_pop(ring, &f)) {
        sim->input.keys = f.keys;
        sim->input.mouse_x = f.mouse_x;
        sim->input.mouse_y = f.mouse_y;
        sim->input.mouse_down = f.mouse_down;
        sim->input.aim_q = f.aim_q;
        sim->input.use_aim_q = f.use_aim_q;
    }
}

uint64_t sim_hash(const Sim* sim) {
    const unsigned char* p = (const unsigned char*)sim;
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (size_t i = 0; i < sizeof(*sim); i++) {
        h ^= p[i];
        h *= 1099511628211ull;            // FNV-1a prime
    }
    return h;
}

void sim_step(Sim* sim) {
    sim->tick++;
    uint32_t keys = sim->input.keys;
    uint32_t prev = sim->prev_keys;

    if (sim->stress_mode) {
        // Bypass the mortal PLAY state machine entirely: the M3 perf test
        // runs unattended (no one piloting the ship), so leaving window
        // shrink/enemies/crush live means it eventually gets crushed or
        // killed and freezes at SIM_STATE_DEAD — exactly what happened the
        // first time this ran on target. Pure bullet-bounce load instead,
        // runs indefinitely regardless of state.
        step_bullets(sim);
        sim->prev_keys = keys;
        sim->prev_mouse_down = sim->input.mouse_down;
        return;
    }

    bool restart_pressed = (keys & KEY_RESTART) && !(prev & KEY_RESTART);

    switch (sim->state) {
        case SIM_STATE_MENU:    step_menu(sim); break;
        case SIM_STATE_PLAY:    step_play(sim); break;
        case SIM_STATE_UPGRADE: step_upgrade_state(sim); break;
        case SIM_STATE_DEAD:    break;
        default: break;
    }

    if (restart_pressed && sim->state != SIM_STATE_MENU) {
        reset_to_menu(sim);
    }

    sim->prev_keys = keys;
    sim->prev_mouse_down = sim->input.mouse_down;
}

void sim_publish(const Sim* sim, SnapshotBuffer* sb) {
    SimSnapshot* snap = snapshot_begin_write(sb);

    snap->tick = sim->tick;

    float left = sim->edges[EDGE_LEFT].pos;
    float right = sim->edges[EDGE_RIGHT].pos;
    float top = sim->edges[EDGE_TOP].pos;
    float bottom = sim->edges[EDGE_BOTTOM].pos;

    snap->playfield_x = left;
    snap->playfield_y = top;
    snap->playfield_w = right - left;
    snap->playfield_h = bottom - top;

    float min_dim = snap->playfield_w < snap->playfield_h ? snap->playfield_w : snap->playfield_h;
    float danger = (DANGER_ONSET_PX - min_dim) / (DANGER_ONSET_PX - WINDOW_MIN_H);
    if (danger < 0.0f) danger = 0.0f;
    if (danger > 1.0f) danger = 1.0f;
    snap->danger = danger;

    for (int i = 0; i < 4; i++) snap->edge_flash[i] = sim->edge_flash[i];

    snap->boss_win_x = sim->boss_win_x;
    snap->boss_win_y = sim->boss_win_y;
    snap->boss_win_w = BOSS_WINDOW_W;
    snap->boss_win_h = BOSS_WINDOW_H;
    snap->boss_win_active = sim->boss_win_active ? 1u : 0u;
    snap->boss_vulnerable = sim->boss_vulnerable ? 1u : 0u;

    snap->player.x = sim->player_x;
    snap->player.y = sim->player_y;
    snap->player.sprite = 0;
    snap->player.flags = (sim->invuln_ticks > 0) ? 2u : 0u;  // invuln_blink
    snap->player.age = 0;  // only bullets use age (spawn pop)

    uint16_t np = sim->pshot_count;
    for (uint16_t i = 0; i < np; i++) {
        snap->pshots[i].x = sim->pshots[i].x;
        snap->pshots[i].y = sim->pshots[i].y;
        snap->pshots[i].sprite = 6;  // player_shot
        snap->pshots[i].flags = 0;
        snap->pshots[i].age = 0;
    }
    snap->pshot_count = np;

    uint16_t ne = sim->enemy_count;
    for (uint16_t i = 0; i < ne; i++) {
        const Enemy* e = &sim->enemies[i];
        snap->enemies[i].x = e->x;
        snap->enemies[i].y = e->y;
        // sprite ids per §6.3: 1=triangle 2=circle 3=octagon 4=spiker
        snap->enemies[i].sprite = (uint8_t)(e->type + 1u);
        uint8_t flags = 0;
        if (enemy_is_outside(sim, e)) flags |= 1u;  // dim + invulnerable
        // Circle's dash wind-up borrows the hit-white tint as its telegraph.
        bool windup = (e->type == ENEMY_CIRCLE && e->dash_state == CIRCLE_WINDUP);
        if (e->hit_flash_ticks > 0 || windup) flags |= 8u;
        snap->enemies[i].flags = flags;
        snap->enemies[i].age = 0;
    }
    snap->enemy_count = ne;

    uint16_t nb = sim->bullet_count;
    for (uint16_t i = 0; i < nb; i++) {
        const Bullet* b = &sim->bullets[i];
        snap->bullets[i].x = b->x;
        snap->bullets[i].y = b->y;
        snap->bullets[i].sprite = 5;  // enemy_bullet
        snap->bullets[i].flags = (b->age_ticks < BULLET_POP_TICKS) ? 4u : 0u;  // spawn_pop
        snap->bullets[i].age = b->age_ticks;
    }
    snap->bullet_count = nb;

    uint16_t nbeam = sim->beam_count;
    for (uint16_t i = 0; i < nbeam; i++) snap->beams[i] = sim->beams[i];
    snap->beam_count = nbeam;

    snap->score = sim->score;
    snap->lives = (uint8_t)sim->lives;
    snap->wave = (uint8_t)sim->wave;
    snap->state = sim->state;
    snap->upgrade_a = sim->upgrade_a;
    snap->upgrade_b = sim->upgrade_b;
    snap->upgrade_selected = (uint8_t)sim->upgrade_selected;

    snap->hit_flash = sim->hit_flash;
    snap->shake_x = sim->shake_x;
    snap->shake_y = sim->shake_y;

    snapshot_publish(sb);
}
