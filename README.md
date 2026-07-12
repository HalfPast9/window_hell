# windowed-hell

A 2D bullet-hell roguelike where the playable area is a "window" that constantly
shrinks toward the player; you shoot the window's edges to push them back out,
trading damage-on-enemies for survival-space. Built as a hard real-time system:
a fixed-tick sim thread fully decoupled from rendering, deterministic by
construction, with a live on-screen HUD showing frame time, sim jitter, and
worst-case measurements. It's a game to the audience and an RTOS demo to the
QNX engineers judging it.

> Window mechanic inspired by **Windowkill** (torcado) — see Credits below.

*Status: M0–M3 complete (M3 — Pi first contact — verified on real Raspberry
Pi 5 hardware). M4/M5 content (enemies, waves, upgrades, Spiker boss, replay)
is also built and verified; see `BRINGUP_LOG.md` for full milestone history
and live bring-up notes.*

## Architecture

```
┌────────────────────────────────────────────────────────┐
│ main thread (render):                                  │
│   platform_poll_input() → push InputFrame to sim queue │
│   read latest SimSnapshot (triple buffer)               │
│   render @ vsync (target 60 fps)                        │
│   draw HUD from metrics ring buffer                     │
├────────────────────────────────────────────────────────┤
│ sim thread (real-time):                                 │
│   fixed 240 Hz loop; QNX: SCHED_FIFO elevated prio,      │
│   clock-absolute sleep; Linux: best-effort               │
│   consume InputFrames → step sim → publish snapshot      │
│   record tick timing into metrics ring buffer            │
└────────────────────────────────────────────────────────┘
```

`platform.h` is the only OS-facing seam; `platform_linux.c` (X11+EGL, dev) and
`platform_qnx.c` (QNX Screen+EGL, target) are the two implementations. Game
logic never touches an OS or windowing header.

## Why this is a real-time system

- **Fixed 240 Hz tick** on its own thread, decoupled from a vsync-paced render
  thread. 240 divides cleanly into 60 fps (4 ticks/frame).
- On QNX, the sim thread runs `SCHED_FIFO` at elevated priority
  (`pthread_setschedparam`); render stays default priority.
- **Jitter** = `actual_wake_time − scheduled_time`, tracked as last/mean
  (EWMA)/max-since-reset plus a 16-bucket histogram, shown live on the HUD.
- **Overrun policy:** if a tick runs long, it is executed late and counted —
  sim steps are never silently skipped.
- **Pi jitter numbers** (Raspberry Pi 5, QNX 8.0, M3 measurements — see
  `BRINGUP_LOG.md` for full detail):
  - Normal gameplay: `FPS 60.0  FT 16.7ms  WORST 38.8ms  JIT avg 460us max 887us  OVR 0`
  - Stress test (`--stress 2048`, 2048 bouncing bullets): `FPS 60.0  FT 16.7ms  WORST 52.0ms  JIT avg 570us max 985us  OVR 0`
  - Sim thread confirmed running `SCHED_FIFO` priority 60 on-target (via `pidin`).

## Determinism & replay

The sim is a pure function of `(seed, tick, input log)`: fixed-capacity
entity pools, deterministic iteration order, a single xorshift64* PRNG owned
by the sim, no wall-clock reads inside `sim_step`. That means a ~4 KB input
log (keymask changes only) reproduces a run bit-for-bit — `F2` starts/stops
recording, `F3` replays the last run. `make replaycheck` runs a 10k-tick
scripted input twice and diffs a hash of the final sim state as a determinism
regression test.

## The game

Move with arrows/WASD, hold `J`/`Z` to shoot two parallel streams, `K`/`X` to
focus-move (slower, precise). The window creeps inward every tick; a bullet
stream that exits through an edge pops that edge back outward. Shoot inward
to kill enemies, shoot outward to reclaim room — you can't do both at once,
and that tension is the entire game. Enemies (Triangle, Circle, Octagon) come
from Windowkill v1's roster on purpose; every 5th wave the Spiker boss arrives
carrying **its own window** — push your walls out until the two windows touch
and they merge into one room you can walk through; only then can it be hurt.
It telegraphs its escape teleport with a ghost outline of where its window
will jump, so don't be standing in its half when the floor leaves. Kill it
for a pick-2 upgrade. Three lives; an edge (or the void) that reaches you
costs one.

## Controls

| Key | Action |
|---|---|
| Arrows / WASD | Move |
| J / Z | Shoot |
| K / X | Focus (slow, precise move) |
| R | Restart |
| F1 | Toggle HUD |
| F2 | Start/stop replay recording |
| F3 | Play last replay |
| Esc | Quit |

## Build & run

### Linux (dev)

```sh
sudo apt install libx11-dev libegl-dev libgles-dev
make linux
./bin/windowed-hell-linux
```

### QNX (Raspberry Pi target — a Pi 5 in practice; the PRD assumed a 4B)

```sh
source $QNX_SDP_INSTALL_DIR/qnxsdp-env.sh
make qnx
TARGET_IP=<pi-ip> ./deploy.sh   # ssh/scp as qnxuser, no root; see BRINGUP_LOG.md
```

`make qnx-x86` builds for the QNX x86_64 VM (bring-up smoke test before Pi
access). `make all` builds both Linux and QNX targets so cross-compile
breakage is caught immediately.

## Credits

- Window mechanic, enemy names (Triangle/Circle/Octagon), and the Spiker boss
  are from **Windowkill** by torcado — borrowed on purpose; the fidelity is
  the homage. This build does not claim originality of the mechanic.
- Deliberate divergences from Windowkill: enemies caught outside the shrunken
  window are invulnerable (rather than just off-screen), the roguelike
  layer is a pick-2-of-6 upgrade choice between waves instead of a coin/Star
  Shop, and the Spiker boss carries its own window that merges with yours on
  contact (reaching it by pushing your walls outward is the fight).
- 8×8 bitmap font: public domain (`tools/font8x8.h`).

See `BRINGUP_LOG.md` for QNX/Pi bring-up notes and `PRD.md` for the full spec
this was built against.
