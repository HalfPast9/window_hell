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
is also built and verified. Co-op multiplayer (see below) is built and
verified live on two Pi 5s over a direct ethernet link. Hand-tracking control
(see below) is verified live over WiFi. See `BRINGUP_LOG.md` for full
milestone history and live bring-up notes.*

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

The front end is `MODE SELECT -> [WAITING ROOM, multiplayer only] ->
CHOOSE YOUR SHIP -> play`. Arrows/WASD navigate, shoot (mouse or `J`/`Z`)
confirms. `R` always returns to `MODE SELECT` — from anywhere, including
mid-run — never a shortcut back into play.

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
for a pick-2 upgrade. Three lives solo (four, shared, in co-op); an edge (or
the void) that reaches a ship costs one.

## Multiplayer

Two-player co-op in one shared window: two ships, one arena, one shared lives
pool, one shared upgrade pick per wave. `MODE SELECT` offers **HOST GAME** /
**JOIN GAME** alongside single player; picking either drops into a
`WAITING ROOM` (`R` cancels) until a peer is found, then both machines land on
an identical, synchronized `CHOOSE YOUR SHIP` screen together.

**Architecture:** lockstep, not client/server. The sim is already a pure
function of `(seed, per-tick input)` — `replaycheck` proves it bit-identical
across Linux/QNX-x86/QNX-aarch64. Two identical binaries fed identical input
therefore produce identical states, so co-op is "both machines run the full
sim; exchange only each tick's ~20-byte input packet" — a replay streamed
live instead of read from disk. Local input is buffered 4 ticks
(`NET_INPUT_DELAY_DEFAULT`, ~16.7 ms) before use, hiding the direct-cable's
sub-millisecond RTT completely; a tick only steps once both sides' input for
it is in hand. Every second, each side stamps a full-state hash into its
packets — the peer compares it against its own hash for that same tick, and
the in-game HUD's `NET HOST/JOIN D4 STALL n SYNC OK` line reports the result
live (see `src/netplay.h` for the full protocol comment).

**Verified on real hardware:** two Raspberry Pi 5s, QNX 8.0, connected by a
direct ethernet cable (`cgem0`, auto-assigned IPv4 link-local addresses,
~0.15 ms RTT). A full co-op session — WAITING ROOM sync, CHOOSE YOUR SHIP,
a full run played to completion — ran with **0 stalls, 0 overruns on both
sides**, jitter in the same ~500–600 µs range as single-player on the same
hardware. Also verified: `make mp-check` (the multiplayer analogue of
`replaycheck`, over real loopback UDP, nothing mocked) passes with 0 stalls
and matching hashes; killing one side correctly flips the other's HUD to
`PEER LOST` within 2 s. Two real bugs were found and fixed during this
bring-up (input-delay bootstrap gap, QNX key-repeat flooding UI navigation)
— see BRINGUP_LOG.md. `--mp-host` / `--mp-join <ip>` / `--mp-port <port>`
pre-fill the front end for scripted launches (`deploy.sh`); interactive play
never needs them. F2/F3 (replay record/playback) are single-player only —
the on-disk format is one input stream, not two.

## Hand-tracking control

`src/handtrack.h`/`src/handtrack.c` is a generic UDP receiver (`--handtrack
[port]` / `WH_HANDTRACK=1`, default port 47800) that folds hand-derived
movement/aim/shoot into the same input path as keyboard/mouse — a dead or
absent tracker just leaves the game keyboard-only, no special handling
needed anywhere else. It was built and verified independent of any real
camera (a synthetic fake sender lives at `tools/handtrack_fake_sender.py`).

[`qnx-handtrack-collab/`](qnx-handtrack-collab/) is a teammate's
(tan-pixel's) companion project — a MediaPipe two-hand webcam tracker that
sends this exact `HtPacket` protocol — vendored into this repo for the
Devpost submission (original:
[tan-pixel/qnx-handtrack-collab](https://github.com/tan-pixel/qnx-handtrack-collab)).
Its `bullethell-cv-prototype/joystick_controller.py` runs on any laptop with
a webcam (standard `opencv-python`/`mediapipe`, no QNX build required) and
was verified live against a Pi over WiFi: left hand → movement, right hand →
aim, right fist → shoot, all decoding correctly with no protocol changes on
either side. The repo's own native on-Pi camera path (`docs/PI_DEPLOYMENT.md`)
is a separate, unfinished effort blocked on MediaPipe's QNX aarch64 Bazel
cross-compile — not needed for the webcam-over-WiFi path above.

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
breakage is caught immediately. Both single-player and co-op multiplayer are
verified running live on real Pi 5 hardware (see Multiplayer above and
BRINGUP_LOG.md).

`make mp-check` runs the multiplayer determinism CI (real loopback UDP, see
Multiplayer above) — the co-op analogue of `make replaycheck`.

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
- Hand-tracking sender (`qnx-handtrack-collab/`): **tan-pixel**, vendored
  from [tan-pixel/qnx-handtrack-collab](https://github.com/tan-pixel/qnx-handtrack-collab)
  for this submission — see Hand-tracking control above.

See `BRINGUP_LOG.md` for QNX/Pi bring-up notes and `PRD.md` for the full spec
this was built against.
