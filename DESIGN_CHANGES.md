# Design change — mouse aim/shoot + slow fire rate

**Status:** CONFIRMED — ready to implement. Both open questions below are
resolved: fire rate starts at **1.9/s**, and `KEY_SHOOT` stays wired as a
keyboard fallback alongside the mouse button.

**Motivation (in your words):** the gameplay loop needs tightening. Two changes:
1. Aim/shoot moves off the keyboard onto the mouse.
2. Fire rate drops "a ton." **Confirmed:** infinite ammo, hold-to-fire still
   works exactly like it does today — the resource being managed is *time*
   (each shot fired is a shot not fired for ~N ticks after), not a depletable
   pool. Simpler than my original ammo-pool proposal; see §2 below.

---

## 1. Input model: mouse aim + shoot

### What changes
- **Movement stays on keyboard** (arrows/WASD) — unchanged.
- **Aim** is no longer "last movement direction" (current `sim.c` behavior). It
  becomes the vector from the player to the mouse cursor, recomputed every tick,
  independent of movement. This makes it a twin-stick-style layout: move with one
  input, aim/shoot with the other.
- **Shoot** moves from the `J`/`Z` key to a mouse button (left click, held to
  fire — still subject to the new resource limit below).
- **Confirmed:** `KEY_SHOOT` stays wired as a keyboard fallback alongside the
  mouse button (fires in the last-movement direction, same as today), to
  hedge the one real unverified risk in this change — see D9 note below.

### What this touches
- **D9 is amended.** The PRD's original rationale — "judges will use a keyboard;
  controller support is schedule risk" — was written against *controller* risk,
  not mouse risk, and mouse is the standard input for this genre on desktop. I'd
  keep the keyboard-shoot fallback specifically to hedge the part of D9's
  rationale that's still valid (unproven input path on the QNX Screen target).
- **`platform.h` contract (§5)** — `PlatformInput` needs mouse fields:
  ```c
  typedef struct {
      uint32_t keys;
      float    mouse_x, mouse_y;   // internal 1280x720 space, already converted
      bool     mouse_down;
      bool     quit_requested;
  } PlatformInput;
  ```
  Coordinate conversion (window pixels → internal 1280×720 space) belongs in each
  platform impl, same place the viewport mapping already happens conceptually —
  keeps `sim.c` platform-agnostic per D8.
- **`platform_linux.c`** — needs `MotionNotify` + `ButtonPress`/`ButtonRelease`
  X11 events added to the event mask (currently only `KeyPressMask |
  KeyReleaseMask | StructureNotifyMask`).
- **`platform_qnx.c`** — needs `SCREEN_EVENT_POINTER` handling
  (`SCREEN_PROPERTY_POSITION`, `SCREEN_PROPERTY_BUTTONS`). **Unverified** — no
  QNX Screen mouse-support precedent in this codebase yet. Flagging as a real
  bring-up risk, same category as D9's original keyboard-quirk concern (PRD
  §5.4 step 7: "test a USB keyboard on the Pi during the first-hour spike" —
  same treatment needed for a mouse once hardware lands).
- **`input_ring.h` / `InputFrame`** — currently `{ uint32_t keys; uint64_t tick; }`.
  Needs mouse position added so the sim thread gets it through the same
  lock-free path as keys, preserving D6 determinism (sim reads input only from
  the folded `InputState`, never polls the platform directly).
- **Determinism / replay (§6.4, §6.5) — the one non-trivial ripple.** §6.5
  assumes replay files are cheap because they record *changes only* to a sparse
  keymask ("<4 KB for a 2-minute run"). Mouse position changes on most ticks,
  not sparsely, so a naive "record on any change" replay format balloons.
  Options, not yet decided:
  - (a) Quantize/delta-encode mouse position, record every N ticks instead of
    every change (replay is not implemented yet — M5 — so no existing format to
    break; we just design it right the first time).
  - (b) Record the *aim angle* instead of raw mouse coordinates — one float,
    still needs delta/quantization but simpler than an (x,y) pair.
  - Not blocking right now since replay is M5 scope, but worth deciding before
    `replay.c` gets written so we don't re-architect it twice.
- **Rendering** — a crosshair/reticle was built and then **removed by
  request**: the OS cursor is already visible over the window, so an in-game
  reticle was redundant. Nothing in §7 draws one. (This also removed the
  `cursor_x/cursor_y` fields that had been added to `SimSnapshot` to feed it —
  they had no other consumer.)
- **sim.c** — `step_player` (movement) and aim/shoot need to split apart; today
  they're coupled (`aim_dx/aim_dy` is set inside the movement branch). New aim
  computation is just `normalize(mouse - player)`, done unconditionally every
  tick regardless of movement or fire state.

---

## 2. Fire rate: infinite ammo, much slower cadence

**Confirmed direction:** no ammo pool, no depletable resource. Hold the shoot
button and it fires continuously, exactly like today — just at a much lower
rate. The resource being managed is the *window of time* between shots: every
shot you fire is ~N ticks where you can't fire again, so choosing to spend a
shot (on a wall vs. an enemy) has real opportunity cost even though supply is
unlimited. This is a pure retune of the existing cooldown gate
(`shoot_cooldown_ticks` in `sim.c` already works exactly this way) — no new
sim state, no new HUD element, no upgrade-semantics rework. Simplest possible
version of the ask.

**Confirmed starting number:**
```
PSHOT_FIRE_HZ_DEFAULT = 1.9   // was 12.0 — ~6.3x slower, ~526ms between shots
```
`1.9/s` with the existing 2-bullet multishot is ~3.8 bullets/sec sustained,
down from ~24/sec today. Still a starting point for the playtest pass (see
below), not a final tuned value — but confirmed as where implementation
starts.

Upgrade id 1 ("Fire Rate," −12% cooldown per pick) needs no redefinition —
it already reduces `fire_cooldown_ticks_base`, which is exactly the right
knob under this model. No change needed there.

### Balance ripple — this is the part that actually costs time

M4's enemy HP, wave spawn density, and spawn cadence were tuned against a
12/s-hold, 2-bullets-per-shot output (~24 bullets/sec sustained). Dropping
sustained output to ~3.8/sec is roughly an order of magnitude less sustained
DPS. Left untouched, waves get substantially harder — maybe unfairly so.
Likely needs:
- Lower `TRIANGLE_HP`/`CIRCLE_HP`, or
- Slower wave spawn density (`2+n/2` triangles formula), or
- Both, tuned by feel once mouse-aim is in and playable.

This can't be fully pre-computed on paper — needs a playtest pass after the
input change lands, same as M2's own GATE ("tune shrink/push/spring constants
for max 1 hour, then proceed anyway — content sells it"). I'd budget a similar
fixed playtest/tune window here rather than trying to solve it analytically.

---

## Post-implementation findings (2026-07-10)

Implemented and verified: mouse aim, mouse shoot, keyboard-shoot fallback,
`PSHOT_FIRE_HZ_DEFAULT = 1.9`. All three targets build clean. Two
consequences of the fire-rate drop turned up that aren't obvious from the
change itself, and that a playtest would feel but might misattribute:

### 1. The push diminishing-returns mechanic is now effectively dead

`WINDOW_PUSH_DECAY_WINDOW_TICKS` is `SIM_HZ/2` = 120 ticks (500 ms). The new
fire cooldown is `240/1.9` = **126 ticks (525 ms)**. Since 525 ms > 500 ms,
**consecutive volleys never decay each other** — every volley's first bullet
lands the full 6 px push. The `×0.85`-per-hit / 2 px-floor system now only
applies *within* a single volley (bullets 2..n of a multishot fire on the
same tick, so bullet 2 gets 6×0.85 = 5.1 px).

That system was designed against a 12/s fire rate, where ~6 volleys landed
inside each 500 ms window and push decayed to the 2 px floor almost
immediately. It's now a no-op between volleys. Not a crash, not a compile
error — a balance mechanic that silently stopped doing its job. Options:
leave it (the slow fire rate is itself the limiter now, so decay is
arguably redundant), or shorten the window / tie it to the fire interval if
we still want repeat-hit punishment.

### 2. Push can no longer outpace shrink in aggregate

| | old (12/s) | new (1.9/s) |
|---|---|---|
| push on one focused edge | ~48 px/s (decayed to floor) | **21.1 px/s** (no decay) |
| shrink, all 4 edges | 32 px/s total (8 px/s each) | 32 px/s total |
| net if 100% of fire goes to walls | **positive** | **negative** |

Even dumping *every* shot into the walls, the window now net-shrinks: you
can win one edge (+13.1 px/s on it) while the other three lose 8 px/s each.
Under the old rate you could hold the whole window open and still have fire
to spare.

Whether this is wrong depends on intent. It sharpens the §0.1 tension
enormously — but §0.1 also promises *"you claw the walls back by shooting
outward,"* which is now only locally true, never globally. The window's
slide toward the 140 px floor is essentially inevitable; from 900 px wide
that's ~48 s of pure shrink, so a run has a soft time limit unless
`Bulwark` (−15% shrink) / `Wall Punch` (+2 push) upgrades stack.

**This needs a human feel-pass, not more math.** Levers, roughly in order of
how surgically they hit the problem:
- `WINDOW_SHRINK_RATE_DEFAULT` (8 px/s) — the most direct dial.
- `WINDOW_PUSH_BASE_DEFAULT` (6 px) — restores reclaim power without
  touching the fire rate.
- `PSHOT_FIRE_HZ_DEFAULT` (1.9) — but this is the thing we're deliberately
  making scarce, so it should be the *last* lever, not the first.
- Enemy HP (`TRIANGLE_HP` 6, `CIRCLE_HP` 4) — orthogonal; controls how much
  of your scarce fire the enemies demand, and so indirectly how much is left
  for walls. Time-to-kill is now 1.58 s (Triangle) / 1.05 s (Circle) of
  sustained on-target fire; wave 10 needs ~14 s of it.

### 2a. Retune: `WINDOW_PUSH_BASE_DEFAULT` 6 -> 16

Confirmed direction: rather than speed the gun back up, make **each shot shove
harder**, so wall upkeep is an active concern you interleave with fighting and
you never quite bank the arena you want.

Holding both window dimensions costs 32 px/s of push (each axis loses 16 px/s:
8 from each of its two edges). Supply is `1.9 volleys/s × (p + 0.85p)`:

| push_base | supply px/s | % of fire spent holding walls | left for enemies |
|---|---|---|---|
| 6 (old) | 21.1 | **152%** — impossible | none |
| 14 | 49.2 | 65% | 35% |
| **16** | **56.2** | **57%** | **43%** |
| 18 | 63.3 | 51% | 49% |

`16` lands where asked: structurally short, walls always creeping, but a
focused burst reclaims real ground. `Wall Punch` also went `+2` -> `+5`
(`UPGRADE_WALL_PUNCH_ADD`) — the PRD's `+2` was +33% of a 6 px base and would
be a 12.5% dud on 16. A divergence from the PRD's literal number, faithful to
its intent.

### 2b. Two bugs the retune exposed

Found with `make balance-probe` — a headless harness (`tools/balance_probe.c`)
that drives the real `sim_step()` with scripted input, no GL, no X. Both were
latent before; the bigger push just made them fire fast enough to see.

**(i) The window could drift off-screen, silently killing the push mechanic.**
`clamp_axis` bounded the rect's *size* (140..1180 / 140..660) but never its
*position*. Pushing one edge translates the whole rect, so firing right long
enough shoved the right edge past x=1280. Player shots despawn at
`PSHOT_DESPAWN_MARGIN` (60 px) outside the screen — so they died before ever
reaching the edge, that edge became unpushable, and the window quietly began
collapsing again while the player kept shooting at it. At `push=6` this took
too long to notice. At `16` it happened in 7 seconds. `clamp_axis` now also
bounds position to the 1280x720 screen, sliding both edges together (size
preserved) and re-anchoring the springs so they stop fighting the clamp.

**(ii) `danger` could never exceed 0.33 — the crush telegraph never fired.**
PRD §8.1 defines `danger = clamp01((180 - min(w,h)) / 120)` and describes it
as *"ramps up as the smaller dimension drops toward the 140 floor."* But the
floor **is** 140, so `min(w,h)` bottoms out there and the expression maxes at
`(180-140)/120 = 0.333`. The border never finished going red; the pulse never
reached full rate — despite §10's cut order listing the danger telegraph as
**never cut**. The intent is unambiguous, the divisor is just wrong. Now
`(DANGER_ONSET_PX - min) / (DANGER_ONSET_PX - WINDOW_MIN_H)`, i.e. `/40`.
Verified: danger now sweeps 0.07 -> 0.47 -> 0.87 -> 1.00 over ~3 s as the
window closes on the floor.

Measured behavior after both fixes (`make balance-probe`):
- never shoot: height hits the 140 floor at t≈27 s, width at t≈49 s; danger
  reaches 1.00 at t≈27 s.
- all fire at one edge: that edge pins against the screen bound (correct and
  legible — you must go work another edge), the opposite edge keeps creeping.
- fire split across all four edges: window *grows* (900x560 -> ~1008x641 in
  10 s), so 100%-on-walls genuinely reclaims. With enemies taking ~57% of
  your fire, you roughly tread water. That is the requested feel.

### 3. Fallback semantics, confirmed on screen
Mouse fires toward the cursor; `KEY_SHOOT` (J/Z) fires along the last
movement direction. Both verified by screenshot. No in-game crosshair — the
OS cursor serves that role (see §1).

---

## Roadmap impact

This is a retrofit into **already-built M2 (push mechanic) and M4 (enemy
balance)** work, not new milestone content. Proposed sequencing:

1. **Platform layer**: mouse fields in `platform.h`, wire up
   `platform_linux.c` (fast, we can test it immediately under WSLg).
   `platform_qnx.c` mouse support can follow — not blocking Linux-side
   iteration, but shouldn't be deferred indefinitely given it's unverified;
   worth a smoke-test in the QNX x86 VM once that's set up, well before M3.
2. **sim.c**: decouple aim from movement (mouse-driven), retune
   `PSHOT_FIRE_HZ_DEFAULT` down to the new starting number.
3. **render.c**: no changes needed — no crosshair (OS cursor suffices), and
   infinite ammo means no new HUD readout.
4. **Playtest/retune pass**: enemy HP and wave density against the new output,
   budget it like M2's GATE (fixed time box, tune by feel, move on).
5. Resume forward milestone work (M5: Octagon, Spiker, replay, juice) —
   replay's format should be designed with mouse-aim input in mind from the
   start (per the determinism note above), so this retrofit should land
   *before* `replay.c` gets written, not after.

Not proposing new milestone numbers (M4.5, etc.) — this is corrective work on
M2/M4, cleanest to just treat as "finish M4 properly" before M5 starts.
