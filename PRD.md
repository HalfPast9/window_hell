# PRD — "WINDOWED HELL" (working title)
### A Windowkill-inspired bullet-hell roguelike built as a hard real-time system on QNX 8.0 (Raspberry Pi 4B target)

**Version:** 1.1
**Timeline:** ~48-hour hackathon (starts tomorrow, ends Sunday). Tonight = environment setup only.
**Author of intent:** Shri (Computer Systems Engineering, controls/RTOS background, prior QNX experience)
**Audience of this document:** A coding agent. This PRD is the single source of truth. Build exactly this. Where a decision is marked LOCKED, do not revisit it. Where marked STRETCH, do not start it until all LOCKED milestones pass their acceptance criteria.

**Changelog v1.0 → v1.1:** Enemy/boss/upgrade roster re-mapped to Windowkill v1 (Triangle/Circle/Octagon + **Spiker** boss; WK-authentic upgrade names). Added the missing half of the spec: **§0.1 what the player actually sees**, **§7.0 visual language & palette**, **§7.6 window/void/border rendering**, and an expanded **§8.1 with the window animation model** (spring-eased edges, push-pop, danger telegraph). Ripple edits in §6.3 snapshot, §7.5 atlas, §8.2/§8.4 collision, §10 milestones/cut order.

---

## 0. One-paragraph pitch

A 2D bullet-hell roguelike where the playable area is a "window" that constantly shrinks toward the player; the player shoots the window's edges to push them back out, trading damage-on-enemies for survival-space. The twist vs. prior art (Windowkill, by torcado — cite it, never claim originality of the mechanic): the game is engineered as a **hard real-time system**. The simulation runs on a fixed-tick, high-priority thread fully decoupled from rendering; the sim is deterministic by construction, giving bit-identical replays from tiny input logs; a live on-screen HUD shows frame time, sim jitter, and worst-case measurements. It is a game to the audience and a scheduling/RTOS demo to the QNX engineers judging it.

## 0.1 What the player actually sees (the game, described)

*This section exists because the rest of the PRD describes the machine, not the game. Build the game to feel like this.*

The screen is almost black — a cold near-black "void" (§7.0). Floating in the center is a brighter rectangle with a glowing cyan border: **the window**. Everything that matters happens inside it. The window is not the OS window; it's drawn by the game (D3), and it is **alive** — its four edges creep inward continuously, slowly, like the walls in a trash-compactor scene. The player is a small white ship near the center. The player's hitbox is tiny (3 px); the sprite is bigger — you learn to trust the dot, not the ship.

You hold shoot. Two pale-cyan bullet streams fire in your aim direction. When a stream crosses an edge and exits into the void, that edge **pops outward** — a quick elastic lurch with a white flash on that border segment and a soft outward jolt. That's the whole game in one gesture: **you claw the walls back by shooting outward, but every bullet spent on a wall is a bullet not spent on the enemies closing in.** Shoot inward and you kill enemies but the walls keep eating your space; shoot outward and you buy room but the enemies pile up.

Enemies arrive from the void, slide up to the window, and press in. Yellow **Triangles** home toward you and slow as they arrive. Light-blue **Circles** wind up and *dash* in a straight line — you juke them. Pink **Octagons** latch onto a window edge like limpets and spit aimed pink bullets across the playfield. Enemy bullets are hot-pink and fat because in danmaku **bullet visibility is the whole ballgame** — they clip out of existence the instant they cross an edge into the void (bullets only live inside the window). Enemies that end up *outside* the shrunken window go dim and desaturated and become invulnerable — a second reason to punch that wall out toward them.

As the window gets small, its border shifts from cyan toward red and starts to pulse, faster the tighter it gets — the telegraph that you're about to be crushed. If an edge reaches you, the screen slams: heavy shake, a beat of hit-stop where everything freezes, a white flash, and the window snaps back open to 60% size while you blink invulnerable for two seconds. You have three lives.

Every few waves the music-less tension spikes: **Spiker** teleports in — an 8-pointed pink-white star that sits dead still and throws 8-bullet radial waves, then telegraphs a laser with a dotted line before a beam sweeps across the window. Kill it and you're offered a choice of two upgrades (press ←/→): faster fire, an extra bullet, more speed, more push, a slower shrink. Then the walls start creeping again, faster than before.

In the top-left, always, floats a green-on-black **telemetry HUD** — FPS, frame time, sim jitter in microseconds, a live histogram, overrun counter. To a player it's set dressing. To a QNX engineer it's the entire pitch, and it never stutters.

---

## 1. Goals, non-goals, judging context

### 1.1 Goals (in priority order)
1. **A finished, fun, playable game.** 90 seconds in a judge's hands must produce one "ohh" moment (the window mechanic).
2. **A demonstrably real-time architecture** that a QNX engineer can interrogate: fixed-tick sim thread at elevated priority, decoupled render, measured jitter, deterministic replay.
3. **Runs well on the Raspberry Pi 4B under QNX 8.0** (hardware provided pre-loaded by the hackathon). 60 fps render, 0 missed sim ticks during normal gameplay.
4. **Portable core:** identical game code on Linux (dev) and QNX (target) behind a thin platform layer.

### 1.2 Non-goals (do not build)
- No networking, no leaderboards, no save system beyond replay files.
- No audio unless every LOCKED milestone is complete (audio on QNX is its own project).
- No meta-progression / full roguelike run structure beyond a simple upgrade-choice between waves. In particular: **no coin/Star Shop** (real Windowkill has one; we deliberately don't — say so in the README).
- No desktop OpenGL features, no Vulkan, no GL extensions without fallback.
- No engine dependencies (no raylib/SDL). Reason: the platform layer IS part of the pitch, and dependency bring-up on QNX is schedule risk. Everything is C + EGL + GLES2 + platform APIs.

### 1.3 Judging context (colors many decisions below)
Judges for this track are BlackBerry QNX engineers; recruiters attend. Optimize for: (a) flawless 90-second play session, (b) architecture defensible in deep Q&A, (c) visible artifacts that invite RTOS questions (the HUD). Business impact is irrelevant.

---

## 2. LOCKED architectural decisions (do not relitigate)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Language: **C (C11)**. Single binary. | Fast to cross-compile, zero runtime deps, judges read it easily. |
| D2 | Graphics: **EGL + OpenGL ES 2.0 only.** `precision mediump float` in all fragment shaders. No extensions. | Pi 4 VideoCore VI path on QNX is GLES2/3 via Screen; strict ES2 subset keeps Linux dev == Pi truth. |
| D3 | Window mechanic implemented **in-viewport** (one fullscreen OS window; the "window" is drawn by the game). | Fully portable, testable on Linux, resizes at any speed, allows juice (easing, border shake, hostile void). Compositor version is STRETCH only. |
| D4 | **Fixed internal render resolution 1280×720**, scaled to display. | Pi perf headroom; enables the compositor stretch goal (fixed buffer, displayed-size scaling). |
| D5 | Sim: **fixed 240 Hz tick** on its own thread, priority-elevated on QNX. Render thread separate, vsync-paced. | 240 divides into 60 cleanly (4 ticks/frame), fine-grained enough for bullet motion, cheap enough for Pi. |
| D6 | Sim is **deterministic**: integer/fixed-step logic driven only by (seed, tick, input log). No wall-clock, no float accumulation across ticks in game state where avoidable, single PRNG (xorshift64*) owned by sim. | Enables replays; determinism is a headline feature. Floats are OK inside a tick's math; determinism comes from fixed dt, fixed iteration order, one PRNG. Same binary+libm per platform ⇒ replays are per-platform bit-identical, which is sufficient (record and play back on the Pi for the demo). |
| D7 | Renderer: **single texture atlas, single interleaved VBO, batched quads, one draw call per frame layer** (max ~3 draw calls/frame: world, playfield frame/void, HUD text). | Thousands of bullets on VideoCore requires batching; draw-call-per-sprite is forbidden. |
| D8 | Platform abstraction: `platform.h` consumed by all game code; implementations `platform_linux.c` (dev) and `platform_qnx.c` (target). Game code never includes OS or windowing headers. | The whole dev strategy depends on this seam. |
| D9 | Input: keyboard only. Arrows/WASD move, J/Z shoot, K/X focus (slow move), R restart, F1 toggle HUD, F2 start/stop replay record, F3 play last replay, Esc quit. | Judges will use a keyboard; controller support is schedule risk. |
| D10 | Attribution: README and title screen credit "window mechanic inspired by Windowkill (torcado)". Enemy names (Triangle/Circle/Octagon/Spiker) are borrowed from WK on purpose — the fidelity IS the homage. | Prior art exists; honesty reads as taste, discovery reads as concealment. |

---

## 3. Repository layout (create exactly this)

```
windowed-hell/
├── README.md                  # pitch, architecture diagram, build/run, credits
├── PRD.md                     # this document
├── BRINGUP_LOG.md             # timestamped notes: every QNX/Pi gotcha encountered
├── Makefile                   # dual-target: `make linux`, `make qnx`, `make deploy`
├── deploy.sh                  # scp binary+assets to target, ssh run (env: TARGET_IP)
├── assets/
│   ├── atlas.png              # generated: see §7.5
│   └── atlas_gen.py           # generates atlas.png + atlas.h (UV table) from code
├── src/
│   ├── platform.h             # the ONLY OS-facing interface (see §5)
│   ├── platform_linux.c       # X11-or-Wayland via EGL; see §5.3
│   ├── platform_qnx.c         # QNX Screen + EGL; see §5.4
│   ├── main.c                 # thread startup, main loop wiring
│   ├── sim.h / sim.c          # ALL game logic; pure; no I/O, no GL, no platform calls
│   ├── render.h / render.c    # GLES2 batch renderer; consumes SimSnapshot
│   ├── snapshot.h             # SimSnapshot definition + triple buffer (see §6.3)
│   ├── replay.h / replay.c    # input-log record/playback (see §6.5)
│   ├── hud.h / hud.c          # timing HUD: frame time, jitter histogram, worst-case
│   ├── metrics.h / metrics.c  # lock-free timing capture (sim thread → render thread)
│   ├── rng.h                  # xorshift64* inline
│   ├── fixedmath.h            # helpers; sim uses float internally but fixed dt
│   └── shaders.h              # vertex/fragment shader source strings (ES2)
└── tools/
    └── font8x8.h              # public-domain 8x8 bitmap font baked into atlas
```

---

## 4. Threading & process model

```
┌────────────────────────────────────────────────────────┐
│ main thread (render):                                  │
│   platform_poll_input() → push InputFrame to sim queue │
│   read latest SimSnapshot (triple buffer)              │
│   render @ vsync (target 60 fps)                       │
│   draw HUD from metrics ring buffer                    │
├────────────────────────────────────────────────────────┤
│ sim thread (real-time):                                │
│   fixed 240 Hz loop; QNX: SCHED_FIFO elevated prio,    │
│   clock-absolute sleep (see §6.2); Linux: best-effort  │
│   consume InputFrames → step sim → publish snapshot    │
│   record tick timing into metrics ring buffer          │
└────────────────────────────────────────────────────────┘
```

- **Sim → render:** triple-buffered `SimSnapshot` (§6.3). Render never blocks sim; sim never blocks render.
- **Render → sim:** single-producer single-consumer lock-free ring of `InputFrame` (key bitmask + tick stamp). Sim consumes all pending frames each tick; latest state wins within a tick.
- On QNX, after threads start: sim thread `pthread_setschedparam(SCHED_FIFO, prio ≈ 60)`; render stays default. Wrap in `#ifdef __QNX__`. If setting priority fails (permissions), log and continue — do not crash.
- **Q&A ammo (write into README):** why FIFO not RR, why 240 Hz, what happens if a tick overruns (answer: detect via metrics, execute the tick late, never skip sim steps silently; overruns counted and displayed).

---

## 5. Platform layer (`platform.h`) — exact contract

```c
// platform.h — game code includes ONLY this.
typedef struct {
    int   width, height;        // framebuffer size actually created
    void* native;               // opaque, owned by platform impl
} PlatformWindow;

typedef struct {
    uint32_t keys;              // bitmask: see KEY_* below
    bool     quit_requested;
} PlatformInput;

enum { KEY_UP=1<<0, KEY_DOWN=1<<1, KEY_LEFT=1<<2, KEY_RIGHT=1<<3,
       KEY_SHOOT=1<<4, KEY_FOCUS=1<<5, KEY_RESTART=1<<6,
       KEY_HUD=1<<7, KEY_REC=1<<8, KEY_PLAY=1<<9 };

bool     plat_init(PlatformWindow* w, int desired_w, int desired_h); // fullscreen ok
void     plat_shutdown(PlatformWindow* w);
void     plat_poll(PlatformWindow* w, PlatformInput* out);           // non-blocking
void     plat_swap(PlatformWindow* w);                               // eglSwapBuffers
uint64_t plat_time_ns(void);                                         // monotonic
void     plat_sleep_until_ns(uint64_t abs_ns);                       // absolute-deadline sleep
// STRETCH (compositor mode) — may be stubbed to no-op:
void     plat_set_playfield_rect(PlatformWindow* w, int x,int y,int wd,int ht);
```

### 5.1 Rules
- `plat_init` creates an EGL context bound to **OpenGL ES 2.0** (`EGL_CONTEXT_CLIENT_VERSION 2`), config: RGB888, depth 0, stencil 0. Request no-vsync-off; leave vsync default (on).
- `plat_sleep_until_ns`: Linux → `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, …)`; QNX → same call (supported) — absolute deadlines prevent drift accumulation.
- `plat_poll` must never block.

### 5.2 Sizing model
Ask for the display's native mode fullscreen; render internally to 1280×720 via a fixed `glViewport` mapping (letterbox if aspect ≠ 16:9). All game/screen math uses the internal 1280×720 space. This makes Linux window ≡ Pi display.

### 5.3 `platform_linux.c`
Use **X11 + EGL** (simplest path; works under XWayland on the dev laptop's niri/Wayland setup). Create a 1280×720 resizable window (not fullscreen on dev — nicer to iterate). Map keys via `XLookupKeysym`. If X11 headers unavailable, fall back to EGL on a GBM/surfaceless path is NOT required — just document `sudo apt install libx11-dev libegl1-mesa-dev libgles2-mesa-dev`.

### 5.4 `platform_qnx.c` (QNX Screen)
Follow QNX Screen Developer's Guide patterns:
1. `screen_create_context(SCREEN_APPLICATION_CONTEXT)`
2. `screen_create_window`, set `SCREEN_PROPERTY_USAGE = SCREEN_USAGE_OPENGL_ES2`, `SCREEN_PROPERTY_FORMAT = SCREEN_FORMAT_RGBX8888`
3. `SCREEN_PROPERTY_BUFFER_SIZE = {1280,720}` (fixed — this is D4 and enables the STRETCH goal), `SCREEN_PROPERTY_SIZE = display native` (Screen scales in hardware)
4. `screen_create_window_buffers(win, 2)`
5. `eglGetDisplay(EGL_DEFAULT_DISPLAY)` → choose ES2 config → `eglCreateWindowSurface(dpy, cfg, screen_win, NULL)`
6. Input: Screen events (`SCREEN_EVENT_KEYBOARD`) via `screen_get_event` with 0 timeout in `plat_poll`; map `SCREEN_PROPERTY_SYM` to KEY_ bitmask; handle key repeat by tracking down/up (`SCREEN_PROPERTY_KEY_FLAGS` & KEY_DOWN).
7. If no keyboard events arrive on target (USB HID quirk), fall back to reading `/dev/kbd` style devices is NOT in scope — instead document and test a USB keyboard on the Pi during the first-hour spike.

**Agent instruction:** write this file early, compile it continuously with the QNX toolchain (even before target access) so it never rots, and smoke-test in the QNX x86_64 VM if available. Log every deviation from docs into `BRINGUP_LOG.md`.

---

## 6. Real-time core

### 6.1 Main loop wiring (`main.c`)
- Parse args: `--replay <file>` (headless-ish playback), `--seed <u64>`.
- Init platform, renderer, sim. Spawn sim thread. Render loop until quit.

### 6.2 Sim thread loop (the heart — get this exactly right)

```c
const uint64_t TICK_NS = 1000000000ull / 240;   // 4,166,666 ns
uint64_t next = plat_time_ns() + TICK_NS;
for (;;) {
    consume_input_ring();            // fold pending InputFrames into current InputState
    uint64_t t0 = plat_time_ns();
    sim_step(&sim, input_state);     // exactly one fixed step; dt is implicit
    uint64_t t1 = plat_time_ns();
    metrics_record(t0 - scheduled_time /*latency/jitter*/, t1 - t0 /*exec time*/);
    publish_snapshot(&sim);          // triple buffer swap (§6.3)
    if (t1 > next) { metrics_overrun(); next = t1; }   // late: run next tick immediately, count it
    plat_sleep_until_ns(next);
    scheduled_time = next;
    next += TICK_NS;
}
```

Jitter definition (displayed on HUD): `actual_wake_time − scheduled_time`. Track: last, mean (EWMA), max-since-reset, and a 16-bucket histogram (0–50 µs, 50–100, … , >1 ms).

### 6.3 Snapshot triple buffer (`snapshot.h`)
Three `SimSnapshot` slots + atomic indices (classic triple buffering: sim writes into `write`, publishes by swapping `write↔ready` with an atomic; render swaps `ready↔read` when it starts a frame). No locks, no tearing, render always gets the newest complete state. `SimSnapshot` is a flat POD copy of render-relevant state:

```c
// sprite index values (SnapEntity.sprite):
//   0=player 1=triangle 2=circle 3=octagon 4=spiker
//   5=enemy_bullet 6=player_shot 7=coin(optional/unused v1)
typedef struct { float x,y; uint8_t sprite; uint8_t flags; } SnapEntity;
// SnapEntity.flags bits: 1=outside_window(dim) 2=invuln_blink 4=spawn_pop 8=hit_white
typedef struct { float x,y,angle,len,width; uint8_t state; } SnapBeam; // Spiker laser
                                                                        // state: 0=telegraph(dotted) 1=firing

typedef struct {
    uint64_t tick;
    // --- window (all internal-res coords; these are the ANIMATED/eased edges, §8.1) ---
    float    playfield_x, playfield_y, playfield_w, playfield_h;
    float    danger;            // 0..1 telegraph: border cyan→red + pulse rate (sim-computed)
    float    edge_flash[4];     // L,R,T,B white-flash intensity 0..1 on push (decays in sim)
    // --- entities ---
    SnapEntity player;
    uint16_t bullet_count;  SnapEntity bullets[MAX_BULLETS];   // MAX_BULLETS = 4096
    uint16_t enemy_count;   SnapEntity enemies[MAX_ENEMIES];   // MAX_ENEMIES = 128
    uint16_t pshot_count;   SnapEntity pshots[MAX_PSHOTS];     // MAX_PSHOTS  = 256
    uint16_t beam_count;    SnapBeam   beams[MAX_BEAMS];       // MAX_BEAMS   = 4
    // --- run state + juice ---
    uint32_t score; uint8_t lives; uint8_t wave; uint8_t state; // MENU/PLAY/UPGRADE/DEAD
    uint8_t  upgrade_a, upgrade_b;                              // UPGRADE state: two offered ids (§8.3a)
    float    hit_flash, shake_x, shake_y;                       // juice params computed in sim
} SimSnapshot;
```

Yes, snapshots are ~100 KB memcpys at 240 Hz — that's ~25 MB/s, trivial. Simplicity wins; do not "optimize" this with deltas.

### 6.4 Determinism rules (enforce in code review of self)
- Sim state stepped ONLY in `sim_step` with implicit fixed dt (`SIM_DT = 1.0f/240`).
- One PRNG (xorshift64*), seeded at run start, advanced only inside sim, in deterministic order.
- No reads of time, no platform calls, no allocation after init inside sim. Fixed-capacity pools for bullets/enemies/shots; iteration order = pool index order always.
- Input affects sim only via the per-tick folded `InputState`.
- **All animation/easing is sim-side** (window edge springs, edge_flash decay, danger level, shake, hit-stop). The renderer is a dumb function of the snapshot so replays reproduce the *look*, not just the outcome.

### 6.5 Replay (`replay.c`)
File = header `{magic, version, seed, tick_count}` + array of `{uint32 tick, uint16 keys}` **changes only** (record a record when the key bitmask changes). Playback: run sim from `seed`, feed recorded keymask per tick instead of live input. Expected file size for a 2-minute run: <4 KB. HUD shows `REC`/`PLAY` badge. Store `last.replay` in CWD on F2-stop; F3 plays it. **Demo line this buys:** "4 KB file, bit-identical playback, because the sim is a deterministic real-time task."

---

## 7. Renderer (`render.c`)

### 7.0 Visual language & palette (LOCKED look — one source of truth)
All colors are premultiplied into the per-vertex `color` (the atlas is grayscale masks tinted at runtime). Keep these in one header (`palette.h`) so tuning is a one-line change.

| Element | Hex | Notes |
|---|---|---|
| Void (outside window / clear color) | `#0A0A12` | near-black blue-ish; the "hostile" space |
| Playfield fill (inside window) | `#12121C` | just barely lighter than void, subtle vignette |
| Window border (normal) | `#4FF0FF` | cyan, 3 px, faint outer glow |
| Window border (danger) | `#FF3B3B` | lerp border cyan→red by `danger` |
| Player ship | `#FFFFFF` | white, cyan trim; hitbox dot slightly brighter |
| Player shots | `#AEF5FF` | pale cyan, thin |
| Enemy bullets | `#FF5DA2` | hot pink, FAT — visibility is king |
| Triangle enemy | `#FFD23F` | yellow (WK-authentic) |
| Circle enemy | `#5BD1FF` | light blue (WK-authentic) |
| Octagon enemy | `#FF7AC6` | pink (WK-authentic) |
| Spiker boss | `#FF5DA2` core, `#FFFFFF` points | 8-point star |
| Enemy-outside-window | tint × `0.30`, desaturate | dim + invulnerable (flag bit 1) |
| HUD text | `#7CFFB0` | green-on-black telemetry look; must read from 2 m |

Overall aesthetic: **dark, high-contrast, neon-on-black danmaku.** No gradients that cost fill-rate; flat fills + a couple of additive glow quads only. Everything reads instantly at a glance.

### 7.1 Pipeline
- One shader program: pos(vec2) + uv(vec2) + color(vec4) interleaved; MVP = fixed ortho(0,1280,720,0). Fragment: `texture2D(atlas, uv) * color`.
- One dynamic VBO (orphan with `glBufferData(NULL)` then `glBufferSubData`, or map-free re-specify each frame — orphaning pattern; VideoCore-friendly).
- CPU builds the quad list each frame from the snapshot: void/frame quads → entities (enemies, player shots, player, then enemy bullets LAST so they render on top — bullet visibility is king in danmaku) → HUD glyph quads.
- Draw order in ≤3 `glDrawArrays(GL_TRIANGLES, …)` calls (world batch, then HUD batch is fine as second call).
- Clear color = void color. The "window" = a bright playfield quad + border quads; everything outside is void.
- Scissor the world batch to the playfield rect so bullets/enemies visually clip at the window edge (`glScissor` + enable): this sells the mechanic.

### 7.2 Camera/screen shake & hit flash
Shake offsets and flash intensity come from the snapshot (computed in sim, deterministic, replay-correct). Renderer applies shake as an offset on the world batch only, never the HUD.

### 7.3 Perf budget (Pi 4)
≤ 4,500 quads/frame worst case (4096 bullets + rest). At 720p with mediump and one texture: comfortably within VideoCore VI limits **if batched**. If measured frame time > 14 ms on Pi: first knob = MAX_BULLETS→2048; second = internal res 960×540. These are config constants in one header.

### 7.4 HUD (`hud.c`)
Top-left, toggle F1, rendered from baked 8×8 font:

```
FPS 60.0  FT 6.2ms  WORST 9.8ms
SIM 240Hz JIT avg 41us max 220us OVR 0
[jitter histogram: 16 vertical bars]
TICK 182400   REC● / PLAY▶        SCORE 12840
```

Worst-case values latch until R (restart) resets them. **This HUD is a designed Q&A hook — it must be visible and legible from 2 m away.**

### 7.5 Atlas (`atlas_gen.py`)
256×256 PNG generated by script (Pillow), all shapes procedurally drawn (circles/polys) — no art dependencies. Contents:
- `white` 1×1 (solid quads — used for border, playfield fill, void, laser beams, HUD bars).
- `player` ship (16×16).
- **`triangle`** (16×16, equilateral), **`circle`** (16×16 disc), **`octagon`** (16×16 regular octagon) — the three enemies, drawn as grayscale masks (tinted per §7.0 at runtime).
- **`spiker`** (32×32, 8-pointed star) — boss.
- `bullet` (8×8 filled circle) — enemy bullet; `pshot` (8×16) — player shot.
- 8×8 font strip from `font8x8.h`.

Laser beams and the dotted telegraph are the `white` quad stretched/tinted (no dedicated sprite). Script emits `atlas.h` (UV rects) **and** `atlas_data.h` (raw RGBA array, ~256 KB) compiled into the binary. Zero runtime file I/O for assets, zero image-decode dependencies, one less deploy artifact.

### 7.6 Window, void & border rendering (the look — build this exactly)
The single most important thing on screen. Draw order within the world batch:
1. **Void:** the `glClear` color is the void (`#0A0A12`). Optionally a *very* faint static grid/scanline in the void (skip if it costs fill or time).
2. **Playfield fill:** one quad of `#12121C` at the animated rect (§8.1). Add a subtle inner vignette (a darker-edged quad or per-vertex darkening at corners) so the arena reads as a lit stage against the void.
3. **Scissor** to the rect; draw all entities (they clip hard at the edge — this is what makes bullets "vanish into the void").
4. **Border:** four 3 px quads on the rect perimeter, colored `lerp(cyan, red, danger)`. Add one additive outer-glow quad per edge (border color, low alpha, ~8 px wide) for the neon bloom. When `danger` is high the border also **pulses**: multiply border brightness by `0.6 + 0.4*sin(phase)`, where the sim advances `phase` faster as `danger→1` (telegraph reads as an accelerating heartbeat).
5. **Edge flash:** for each edge, add a white additive quad along that segment at alpha `edge_flash[i]` — this is the "pop" when a shot pushes the wall (value set in sim on push, decays over ~2 ticks).
6. **Spiker beams:** telegraph = a dotted line (row of small `white` quads, dim) along `beams[i]` when `state==0`; firing = a solid bright quad of `width` when `state==1`.

Corners: draw small bright corner accent quads (2×2 of border thickness) so the rectangle reads as a deliberate "window frame," not just four lines. The window must feel like a physical object that lurches, pulses, and flashes — never a static border.

---

## 8. Game design (`sim.c`)

### 8.1 The mechanic + window animation model (core loop)
**Geometry.** The playfield is an axis-aligned rect; store each of the 4 edges as `{pos, vel, target}` (a 1-D spring). `playfield_*` in the snapshot are the *current animated* edge positions. Start centered at 900×560. Hard bounds: never larger than 1180×660, never smaller than 140×140.

**Shrink.** Every tick, each edge's `target` moves inward at `shrink_rate` px/s (base 8 px/s, +1.5 per wave; ×1.6 while Spiker alive). Shrink is smooth and constant — the slow, relentless squeeze.

**Push.** A player shot that exits through an edge bumps that edge's `target` **outward** by `push_per_shot` (base 6 px), with diminishing returns: ×0.85 per hit on the same edge within 0.5 s, floor 2 px. So shooting outward = reclaiming space; shooting enemies = progress. **This tension is the game.**

**Animation (this is what makes it feel good — do it in sim, deterministic):** each edge's actual `pos` follows `target` via a light **under-damped spring** (stiffness `k≈180`, damping ratio `ζ≈0.55`, integrated at `SIM_DT`). Result: a hard push makes the wall *lurch out and slightly overshoot* before settling (~120 ms) — a satisfying "pop"; the continuous shrink just reads as smooth creep. On each push, set that edge's `edge_flash=1.0` (renderer draws a white flash, §7.6) and add a small directional `shake` (2 px) so the whole window jolts. Keep the spring stable at 240 Hz; clamp `pos` to hard bounds after integration.

**Confinement & the void.** Player is clamped inside the animated rect. Enemy bullets **despawn** the instant they cross an edge (they live only inside the window). Enemies may sit **outside** the rect — drawn dim/desaturated (flag bit 1) and **invulnerable** — until the window grows to include them, giving a second reason to punch edges toward enemies. Player shots continue past the edge (to push it) but can no longer hit enemies once outside.

**Danger telegraph.** Compute `danger = clamp01((180 - min(w,h)) / 120)` (i.e. ramps up as the smaller dimension drops toward the 140 floor). It drives border color cyan→red and the pulse rate in §7.6. This is the "you're about to be crushed" read.

**Crush.** If any edge reaches the player: lose 1 life, reset rect `target`+`pos` to 60% of start size, 2 s invuln (4 Hz blink, flag bit 2), big screenshake (8 px), and **hit-stop 6 ticks** (sim keeps running but entities freeze — a freeze counter inside sim so replays stay correct).

### 8.2 Player
Speed 260 px/s (focus/K/X: 120 px/s — precise dodging). Hitbox **3 px radius** (tiny — danmaku standard); sprite 16 px. Shot: **2 parallel bullets** (aim = last movement direction, default up), 700 px/s, fire rate 12/s while held. 3 lives. Death by **enemy bullet OR enemy contact** = same reset as edge-crush **minus** the rect reset (2 s invuln + shake + 6-tick hit-stop, window unchanged).

### 8.3 Enemies & waves (Windowkill v1-authentic, kept SMALL and finishable)
Three enemy types + one boss. All fixed-pool, all parameterized. Names/shapes match Windowkill v1 (fidelity is the homage — D10).

1. **Triangle** (yellow) — enters from a window edge, **homes** toward the player and *slows as it nears*. **Contact damage only** (no bullets). HP 6. The baseline "bodies closing in" pressure enemy.
2. **Circle** (light blue) — periodically **dashes** in a straight line toward the player's position at dash-start, then coasts; telegraphs the dash with a ~0.3 s wind-up (brief scale/color tell). Must be juked. **Contact damage.** HP 4.
3. **Octagon** (pink) — drifts to the nearest window edge, **latches onto it**, then fires an aimed bullet at the player every 1.6 s (speed 210). This is the main bullet source and it ties to the window edge (thematic). HP 10.

**Boss — Spiker** (every 5th wave; the original Windowkill boss). An 8-pointed pink-white star, **immobile but teleports.** HP 48 (scale +50% on each subsequent appearance — "respawn buff").
- **Passive:** radial wave of **8 bullets every ~2.0 s** (interval shrinks as its internal wave counter climbs).
- **Active:** every ~8 s, a **laser** — telegraphed by a dotted line along the fire direction for ~0.8 s (`beam.state=0`), then a sustained beam (`beam.state=1`) that sweeps ~90° over ~1 s. Getting hit by the beam is a player-death event (§8.2).
- **Teleport:** once it takes enough cumulative damage, it blinks to a new spot and bumps its internal wave counter (attacks speed up). No separate scripted phases.
- While Spiker is alive: `shrink_rate ×1.6` — the room closes faster during the fight.

**Waves.** `wave_n` spawns `2 + n/2` Triangles + `n/3` Circles + (n≥3 ? `n/4` Octagons : 0) on a fixed schedule from the deterministic PRNG. Every 5th wave = Spiker instead of the normal spawn set. Clearing a wave (all enemies dead) → `UPGRADE` state (§8.3a) → next wave.

**STRETCH enemies** (only after M6, WK v1 roster, in order): **Square** (green; hops toward player, contact) → **Splitter** (splits into 2–3 small Triangles on death) → **Heptagon** (parks in view, creeps inward from outside).

### 8.3a Upgrades (Windowkill v1 stat vocabulary — the whole "roguelike" layer)
Between waves: **3-second UPGRADE state** offering **2 of 6** options (PRNG-picked, distinct). Player picks with ←/→, confirms with SHOOT (or auto-confirm on timer). `upgrade_a/upgrade_b` in the snapshot carry the two offered ids for rendering. Do **not** add a coin/Star Shop (§1.2).

| id | Name | Effect | Source |
|----|------|--------|--------|
| 0 | **Speed** | +10% player move speed | WK v1 |
| 1 | **Fire Rate** | −12% reload time (i.e. +fire rate) | WK v1 |
| 2 | **+1 Multi Shot** | +1 parallel bullet per shot | WK v1 |
| 3 | **Max Health** | +1 life (and heal 1) | WK v1 (WK gives +5 HP; we use lives) |
| 4 | **Wall Punch** | +2 `push_per_shot` | our window-mechanic-native twist |
| 5 | **Bulwark** | −15% `shrink_rate` | our window-mechanic-native twist |

First four are straight from Windowkill's basic Shop upgrades; last two are window-mechanic-native (label them as divergences in the README). **STRETCH upgrade:** id 6 **Piercing** (bullets pass through one enemy) — matches WK, cheap to add.

### 8.4 Collision
Circle-circle, brute force at 240 Hz on both platforms — trivially fast, no spatial hashing (do not add it):
- `bullets(≤4096) × 1 player` — enemy bullets vs player hitbox.
- `pshots(≤256) × enemies(≤128)` — player shots vs enemies (in-window only).
- `enemies(≤128) × 1 player` — **enemy body vs player** (contact damage; NEW — Triangle/Circle/Square are contact enemies).
- `beams(≤4) × 1 player` — Spiker laser (point-to-segment distance vs player hitbox) while `state==1`.
Enemies flagged outside-window are skipped for pshot collision (invulnerable) but Octagon-on-edge and contact enemies inside still collide normally.

### 8.5 States
`MENU` (title + "inspired by Windowkill" credit + press shoot) → `PLAY` → `UPGRADE` (between waves) → `DEAD` (score + press R). All state inside sim; renderer just draws by `snapshot.state`.

### 8.6 Feel checklist (sim-computed, snapshot-carried)
Hitstop 3 ticks on enemy kill, 6 on player hit/crush. Shake: enemy kill 2 px decay 0.9/tick; player hit 8 px; each edge-push 2 px directional. Edge-push emits the white flash on that border segment (`edge_flash`, §7.6) + the spring pop (§8.1). Enemy hit → 1-tick white tint (flag bit 8). Bullet spawn "pop" scale-in over 4 ticks (flag bit 4; renderer scales by age — keep simple, skip if time-pressed). Player invuln = 4 Hz sprite blink. Spiker teleport = brief flash + shake at the new position.

---

## 9. Build system

### 9.1 Makefile targets

```make
make linux    # cc, -O2 -g, links: -lX11 -lEGL -lGLESv2 -lpthread -lm
make qnx      # requires: source $QNX_SDP/qnxsdp-env.sh
              # CC=qcc -Vgcc_ntoaarch64le  (aarch64le target)
              # links: -lscreen -lEGL -lGLESv2 -lm   (NOT -lGLESv2_viv; on Pi BSP it's plain GLESv2)
make qnx-x86  # -Vgcc_ntox86_64  (for the QNX VM smoke test)
make deploy   # ./deploy.sh  (scp bin/windowed-hell-qnx root@$TARGET_IP:/tmp/ && ssh run)
make replaycheck  # linux build: run 10k-tick scripted input twice, diff final sim hash → determinism CI
```

Both `linux` and `qnx` targets must be built by default (`make all`) so cross-compile breakage is caught within minutes, always. `replaycheck` computes a FNV-1a hash over the final sim struct and asserts equality across two runs — the determinism unit test.

### 9.2 deploy.sh
`TARGET_IP` env (default 192.168.x.x placeholder), scp binary to `/tmp`, `ssh root@$TARGET_IP "slay windowed-hell; /tmp/windowed-hell-qnx"` (QNX `slay` = kill). Print a reminder if `TARGET_IP` unset.

---

## 10. Milestones, acceptance criteria, and cut order

### M0 — Tonight (environment; NO game code if rules forbid pre-work — check rules; scaffolding/tooling is typically fine)
- SDP 8.0 install verified; `qcc -Vgcc_ntoaarch64le` compiles hello-world. ✅ criterion: binary produced.
- Repo scaffolded per §3; Makefile dual-target skeleton builds an empty main for both targets.
- (If VM available) QNX x86_64 VM boots; ssh works.
- Watch 10 min of Windowkill gameplay; note 3 feel details to steal (the wall-pop, bullet-clip-at-edge, the accelerating danger pulse), 1 divergence (ours: enemies-outside-the-window are invulnerable + pick-2 upgrade instead of the coin/Star Shop).

### M1 — Platform + loop (target: hackathon hour ~4)
Linux window opens, ES2 clear color animates, input polls, sim thread runs at 240 Hz with metrics, HUD shows live jitter. ✅: HUD numbers move; Ctrl-C clean exit.

### M2 — Renderer + mechanic skeleton (hour ~8)
Batched quads from atlas; playfield rect draws with the §7.6 look (fill, glow border, corner accents), **shrinks with spring-eased edges**, player moves inside it, shots push edges with the pop + white flash. Scissor clipping works; danger telegraph turns the border red + pulses when small. ✅: the core tension is *feelable* with zero enemies.
**GATE: If M2 is fun-adjacent, proceed. If the mechanic feels bad, tune shrink/push/spring constants for max 1 hour, then proceed anyway — content sells it.**

### M3 — Pi first contact (the moment hardware is issued — PREEMPTS everything)
gears/gles2 demo runs (proves their image) → deploy.sh loop works → our binary runs → HUD framerate & jitter on real hardware recorded in BRINGUP_LOG. ✅: 60 fps with 2048 test bullets bouncing. If <60: apply §7.3 knobs, record numbers.

### M4 — Game (hour ~20)
Enemies **Triangle & Circle**, waves, all four collision loops (§8.4), lives, states, score, upgrades (§8.3a). ✅: a full run from menu to death is playable and losing feels fair.

### M5 — Juice + replay + boss (hour ~30)
Hitstop/shake/flash per §8.6, **Octagon** enemy, replay record/playback verified bit-identical on Pi (run `replaycheck` on target), **Spiker boss** (radial waves + telegraphed laser). ✅: F3 replays your last run perfectly on the Pi; a Spiker fight is winnable and reads clearly.

### M6 — Freeze & demo (Sunday noon LATEST)
Content freeze. README with architecture diagram + Q&A notes (§4, §6). Demo script rehearsed 3×: (1) 60-sec live play highlighting the mechanic, (2) point at HUD: "sim never missed a tick — that number is the RTOS", (3) F3: "here's that exact run replayed from a 4 KB file", (4) one architecture slide/diagram on paper. Backup: x86 QNX VM build + Linux build both on the laptop, second copy of binary on a USB stick.

### Cut order when behind (cut from the top)
1. **Spiker boss** → 2. Upgrade state (waves just escalate) → 3. **Octagon** enemy → 4. Bullet spawn pop / minor juice → 5. Replay UI (keep replaycheck test) → **never cut:** HUD, the mechanic (incl. the spring-pop + danger telegraph), hitstop/shake, M3.

### STRETCH (only after M6, in order)
S1. **Compositor mode** (§11). S2. STRETCH enemies (Square → Splitter → Heptagon, §8.3). S3. Second bullet pattern for Spiker (dense aimed fan). S4. Title-screen attract-mode replay. (Audio remains out — §1.2.)

---

## 11. STRETCH S1 — Compositor-level window (pre-architected, do not build early)
Because buffer size is fixed (D4) and playfield rect is centralized, compositor mode = `platform_qnx.c` only:
- Create a second fullscreen background window (black, ZORDER below) via a second `screen_create_window` with a tiny filled buffer.
- Game window: each frame call `plat_set_playfield_rect()` → `screen_set_window_property_iv(win, SCREEN_PROPERTY_POSITION, …)` + `SCREEN_PROPERTY_SIZE` mapped from internal coords to display coords. Buffer size NEVER changes; Screen hardware-scales. Renderer switches to drawing the playfield content full-buffer (viewport mode off) via a runtime flag `--compositor`.
- Accept: if resize visibly stutters or flickers within 1 hour of work, abandon; the in-viewport build is the judged build regardless. Demo use: 30-second "and here's the same game where the window is real" coda.

---

## 12. README must contain (agent: write it, it's part of the deliverable)
Pitch (3 sentences) · architecture ASCII diagram (threads + buffers) · "Why this is a real-time system" section (tick model, priority, jitter numbers FROM THE PI, overrun policy) · determinism & replay explanation · a short "the game" section (controls + the window mechanic in 3 sentences, §0.1 condensed) · build/run for both targets · controls · credits (Windowkill inspiration + that enemy/boss names are borrowed on purpose, font source) · BRINGUP_LOG link. Tone: engineer-to-engineer, no marketing.

## 13. Risks & mitigations (final register)
| Risk | Mitigation |
|---|---|
| Pi arrives late / never | M1–M2, M4–M5 are Pi-independent; QNX VM x86 build is the fallback demo; LOCKED D3 keeps the mechanic hardware-free |
| VideoCore shader/perf surprise | strict ES2+mediump from line 1; knobs in §7.3; M3 measures early |
| Keyboard input quirk on Pi/Screen | test in first hour of M3; USB keyboard in bag |
| Window spring feels bad / unstable at 240 Hz | spring constants are one-line tunable (§8.1); M2 GATE budgets 1 hr; fallback = critically-damped (no overshoot) |
| Scope creep (known failure mode of the human) | Cut order §10 is pre-agreed; M6 freeze is noon Sunday, non-negotiable |
| Demo-day crash | replay attract mode as safe demo; backup binaries (Pi/VM/Linux ×USB) |
| "Isn't this Windowkill?" | credited everywhere + divergences listed + "ours is a hard real-time implementation on QNX" one-liner |

---
*End of PRD. Agent: build M1→M6 in order, honor GATEs and the cut order, log all QNX findings to BRINGUP_LOG.md, and never let the engine outgrow the game.*