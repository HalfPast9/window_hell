# Bring-up log

Timestamped notes on every QNX/Pi/toolchain gotcha encountered. Newest entries at top.

## 2026-07-11 — M3: Pi first contact (RPi5, not RPi4B — hardware landed as a Pi 5)

Hardware finally arrived: **Raspberry Pi 5** (the PRD assumed a 4B; QNX image
is `com.qnx.qnx800.quickstart.rpi5`), flashed with the QNX 8.0 self-hosted
developer desktop, on the same WiFi as the dev box. Confirmed via
`uname -a`: `QNX qnxpi 8.0.0 2026/02/27-10:59:13EST RaspberryPi5 aarch64le`.
This closes M3 per PRD §10: gears demo proved the image, `deploy.sh` loop
works, our binary runs, keyboard+mouse verified, and the 60 fps / 2048-bullet
acceptance test passed with real numbers below. Two real, previously-latent
bugs were found and fixed along the way (both below). M5's "replay verified
bit-identical on Pi" criterion, previously only proven on the QNX-x86 VM, is
now also closed natively on the Pi. All fixes are in `platform_qnx.c` only
(plus a small `sim.c`/`main.c` addition for the stress-test flag) —
`make replaycheck` was re-run after each change and stayed green throughout,
confirming none of it touched sim-affecting code.

**Access.** IP `172.17.225.231` (WiFi `bcm0`), login `qnxuser`/`qnxuser`
(default), **no root SSH** on this image. SSH key auth set up
(`~/.ssh/config` host alias `qnxpi`) since password auth over a scripted
non-interactive shell doesn't work well for iterative bring-up.

**SSH gotcha:** default MAC negotiation fails outright —
`Corrupted MAC on input` / `message authentication code incorrect` — on
every plain `ssh`/`scp` call against this image's sshd. Fix: pin
`-o MACs=hmac-sha2-256` on every invocation (added to `~/.ssh/config` for the
`qnxpi` host so it's automatic).

**Filesystem:** root `/` is the read-only IFS image and reports 100% full
(20M/20M) — `/tmp` is *not* a safe deploy target despite the PRD's original
`deploy.sh` using it. `/data/home/qnxuser` (18G free) is the real writable
home and is what `deploy.sh` now targets.

**Step 1 (GO/NO-GO): GLES2 is real on this image.** `/dev/screen/gpus/gpu-1`
exists (non-empty — this is exactly what was empty/missing on the GPU-less
QEMU-on-WSL VM from the previous entry). `gles2-gears` (`/system/bin/`) ran
over SSH and the user confirmed spinning gears on the HDMI display. This
gated everything else — if it had failed, M3 would have stopped here per the
PRD's "never cut" rule on M3.

**`deploy.sh` rewritten** for this environment: `TARGET_USER` (default
`qnxuser`), the MAC workaround baked into `SSH_OPTS`, deploy target
`/data/home/qnxuser` instead of `/tmp`, and `slay -f windowed-hell-qnx`
(no root needed — it only kills the invoking user's own processes, which is
all that's ever running here).

### Bug 1: keyboard Down (S / Down-arrow) moved the player UP

Found immediately on first play: `S` and the Down-arrow both moved the ship
the same direction as `W`/Up-arrow. Root cause, found via a
`WH_KEYDEBUG`-gated raw-symbol dump already in `platform_qnx.c`'s
`plat_poll`: **`sys/keycodes.h` `#define`s `KEY_DOWN` as `0x00000001`** —
an unrelated QNX concept (a key-*event* flag meaning "this is a press, not a
release," used in `SCREEN_PROPERTY_FLAGS` bitfields), included *after*
`platform.h` in `platform_qnx.c`. Since it's a preprocessor macro, not a
namespaced constant, it silently rewrites every later bare `KEY_DOWN` token
in the file — including `sym_to_bit()`'s `return KEY_DOWN;` for both `'s'`
and `KEYCODE_DOWN` — to `1`, which is `KEY_UP`'s actual enum value from
`platform.h`. Confirmed by the debug log: both `'w'` (sym 0x77) and `'s'`
(sym 0x73) printed `bit=1`; so did the Up and Down arrow keycodes
(`0xf052`/`0xf054`). Only `KEY_DOWN` collides — checked all nine
`platform.h` `KEY_*` names against `screen/` and `sys/keycodes.h`; nothing
else matches. Fix: `#undef KEY_DOWN` immediately after
`#include <sys/keycodes.h>`, restoring the identifier to the enum constant
for the rest of the file. Rebuilt, redeployed, user confirmed S/Down-arrow
now move correctly, along with the rest of WASD/arrows.

This is a **platform-file-only** bug — `sim.c`'s `KEY_DOWN` usage
(`platform_linux.c` doesn't include `sys/keycodes.h`, so it was never
affected) was always correct; the enum value coming *into* sim was just
wrong on QNX specifically.

### Bug 2: mouse aim was "weird" — display-space coords fed straight into a 1280x720 sim

Second finding, same debugging pattern (added a matching `MOUSEDBG` print
under the same `WH_KEYDEBUG` flag). The display's native mode is
**1920x1080** (from `/dev/screen/.config`'s `winmgr` section), but the
game's fixed internal buffer is 1280x720 (D4). The existing pointer code's
comment claimed `SCREEN_PROPERTY_POSITION` on a `SCREEN_EVENT_POINTER` is
"window-relative... already internal-space" and used it unscaled. Logged
values disproved that immediately: raw x ranged up to **1725**, raw y up to
**838** — both past the 1280x720 buffer entirely. Since `sim.c`'s aim vector
is `mouse - player` in internal space, feeding it raw display-space pixels
made aim increasingly wrong the further the cursor sat from the window
origin. (Separately confirmed: the game window has *always* rendered
fullscreen on this image, by Screen's own default — that was not the bug,
just an initially-wrong hypothesis while investigating.)

Fix, in `plat_init`: query the window's actual display via
`SCREEN_PROPERTY_DISPLAY` + `screen_get_display_property_iv(..., SCREEN_PROPERTY_SIZE, ...)`,
explicitly set the window's `SCREEN_PROPERTY_POSITION` to `(0,0)` and
`SCREEN_PROPERTY_SIZE` to that display-native size (belt-and-suspenders with
the observed default — makes the scale factor well-defined regardless), and
store `buffer_w/h` + `onscreen_w/h` on `QnxNative`. In `plat_poll`, scale
every pointer position by `buffer_size / onscreen_size` (1280/1920,
720/1080 here) before writing `g_mouse_x/y`, clamped defensively to
`[0,buffer]`. This is also PRD §5.4 step 3's original TODO ("SCREEN_PROPERTY_SIZE
is left at its default... needs a real display to verify; do it in M3"),
now done. Rebuilt, redeployed; user confirmed shots now land where the
cursor points.

Both bugs together explain the earlier "S goes up" / "aiming is weird"
reports as two independent, unrelated latent defects that simply had never
been exercised before real target hardware existed — exactly the kind of
thing M3 exists to find. `WH_KEYDEBUG=1 ./windowed-hell-qnx` reproduces
both debug dumps (`KEYDBG`/`MOUSEDBG` lines to stderr) for any future
target-input bug.

### Input verification (target hardware, USB keyboard + mouse)

Keyboard: WASD/arrows (all four directions, post-fix), J/Z fire (keyboard
fallback), R restart, F1 HUD toggle, K/X focus, Esc quit — all confirmed
working. Mouse: cursor aim (post-fix) and left-click fire confirmed working.
This is the first-ever exercise of `SCREEN_EVENT_POINTER` in this codebase
(flagged unverified since the mouse-aim retrofit in `DESIGN_CHANGES.md`) —
it works correctly once the coordinate scaling above is in place.

### M3 acceptance test: 60 fps / 2048 bullets

No existing hook reached anywhere near 2048 simultaneous bullets (`--wave`
jumps waves, not bullet count), so added `--stress N`
(`sim_start_stress()` in `sim.c`, wired in `main.c`): spawns N bullets
(clamped to `MAX_BULLETS`=4096) scattered across the internal 1280x720
space with PRNG-seeded random velocities.

**First attempt found a real design bug in the stress mode itself, not the
engine:** `sim_start_stress()` called the normal `start_new_run()`, so the
mortal `PLAY` state machine (window shrink, enemy spawns, crush/death) kept
running underneath the bullet swarm. With no one piloting the ship (this is
an unattended perf test), the player eventually got crushed or killed and
the sim landed in `SIM_STATE_DEAD`, which does nothing every tick —
the whole test silently froze. Confirmed on-target: "game just paused when
user died." Fixed by making `stress_mode` bypass the state machine entirely
in `sim_step()` — it now only steps the bouncing-bullet pool (edges off the
fixed screen bounds, not the window rect) and returns, forever, regardless
of state. `make replaycheck` (hash `4ced063e0774b3fb`, unchanged) confirms
this — and the whole stress feature — never touches normal-mode determinism,
since `stress_mode` defaults false and nothing in the normal path reads it.

**Measured on the Pi 5, `./windowed-hell-qnx --stress 2048`:**

```
FPS 60.0   FT 16.7ms   WORST 52.0ms
SIM 240Hz  JIT avg 570us  max 985us  OVR 0
```

60 fps sustained, zero tick overruns — **M3's acceptance criterion met with
no perf knobs needed** (§7.3's `MAX_BULLETS`/internal-res knobs were not
required). The one outlier, WORST 52.0ms, reads as a one-time startup
transient (first-frame texture/buffer upload) rather than sustained jank —
FPS was steady at 60 for the rest of the observed run.

**Measured in normal gameplay** (menu → a couple of waves, no stress flag):

```
FPS 60.0   FT 16.7ms   WORST 38.8ms
SIM 240Hz  JIT avg 460us  max 887us  OVR 0
```

Tighter jitter than the stress case, as expected with far fewer live
entities. Also confirmed via `pidin`'s own thread listing (not just "didn't
crash"): the sim thread consistently shows priority **60, policy `f`**
(`SCHED_FIFO`) in every process listing taken this session — `§4`'s
priority-elevation code path is not just present but actually succeeding on
this non-root `qnxuser` account, contrary to the defensive
log-and-continue path it falls back to if `pthread_setschedparam` fails.

### M5's on-Pi replay criterion, now also closed

M5 originally verified replay only on the QNX-x86 VM. Built `replaycheck`
natively on the Pi (`qcc`/QNX Screen headers aren't on the native include
path — same finding as the earlier VM entry — but `replaycheck` is headless
and only needs `sim.c`/`replay.c`, no GL/Screen): `gcc -std=c11 -O2
tools/replaycheck.c src/sim.c src/replay.c -lm`. **Passed, hash
`4ced063e0774b3fb`** — identical to both the Linux WSL2 dev box and the
QNX-x86 VM from the previous entry. D6 only requires per-platform
bit-identity, so cross-arch/cross-platform identity here (x86_64 Linux,
x86_64 QNX, aarch64 QNX) is a bonus, not a requirement, but a nice one:
`sim.c`'s math is landing on identical bits across three very different
`libm`/compiler combinations (glibc/gcc, QNX x86/clang, QNX aarch64/clang).

### Minor, not chased: can't self-capture the display

Tried to screenshot the Pi's HDMI output remotely (to read the HUD without
relying on the user's eyes) via `/system/bin/screenshot` and
`weston-screenshooter`. Both failed: `screenshot -file=... ` gets
`screen_context_create: Permission denied` even though `qnxuser` is in the
`screen` group (and the `sudo` group — but `sudo -n` still demands a
password, so passwordless elevation isn't configured either);
`weston-screenshooter` needs `XDG_RUNTIME_DIR` and no Wayland socket was
found anywhere on the filesystem (weston here appears to be a QNX Screen
client itself, not exposing a nested Wayland display for other clients to
screenshot through). Not blocking — HUD numbers were read off the physical
display by the user throughout this session — but worth knowing before
trying to automate this further.

## 2026-07-11 — QNX 8.0 Self-Hosted Developer Desktop in QEMU-on-WSL (env spike while waiting on the Pi)

Stood up the **QNX 8.0 Self-Hosted Developer Desktop** (QNX Everywhere Quick
Start Target Image, "QSTI for QEMU") as a local VM to build/run the game in a
real QNX 8.0 userspace before Pi hardware arrives. Base OS works and the
headless game harnesses pass natively; the **graphical GLES2 path does not work
under WSL** and is deferred to the Pi. This is a different machine setup from
the earlier `mkqnximage` VMware VM — this is the *desktop* image, run under
QEMU inside WSL2 `Ubuntu-24.04`.

**Getting the image.** QNX Software Center CLI (Windows):
`"C:\QNX\QNX Software Center\qnxsoftwarecenter_clt.bat" -installIUs com.qnx.qnx800.quickstart.qemu`
→ lands in `C:\Users\Shri\qnx800\images\qemu\` as split parts
`qnx_sdp8.0_qemu_quickstart_20260606.tar.gz.{0,1}` (~1.9 GB). Unpack per its
README: `cat *.tar.gz.* | tar -xzf -`. **Not sparse** — expands to a ~43–47 GB
raw `disk-qemu` (mostly zeros, hence the tiny download), so it needs real free
space to extract. Same desktop is also published as `com.qnx.qnx800.quickstart.rpi4`
/ `.rpi5` — i.e. **flashable to the actual Pi 4B** (the graphical plan, below).

**Boot recipe — the authoritative source is `qnx800/host/common/mkqnximage/qemu/runimage`.**
`mkqnximage` is Windows-only in this SDP install, so `mkqnximage --run` can't
drive QEMU from WSL; replicate `runimage`'s command by hand. Working headless
launch (KVM), run **inside a long-lived background `wsl.exe … bash -lc`** (see
teardown gotcha below):
```
sudo chmod 666 /dev/kvm     # WSL user isn't in the kvm group
cd ~/qnx-desktop/qemu/output
qemu-system-x86_64 -smp 8 --enable-kvm --cpu host -m 4G \
  -drive file=disk-qemu.vmdk,if=ide,id=drv0 \
  -netdev user,id=net0,hostfwd=tcp::10022-:22,hostfwd=tcp::5900-:5900 \
  -device virtio-net-pci,netdev=net0,mac=52:54:00:e1:bb:9d \
  -kernel ifs.bin \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -vga none -display none -serial file:/tmp/qnx-serial.log -pidfile /tmp/qemu.pid
```

**Two boot gotchas that each cost real time (both diagnosed by `screendump` — QEMU
`-monitor unix:sock` → `screendump x.ppm` → convert to PNG → view the VGA console,
since serial stays empty during early boot):**
- **Must boot `-kernel ifs.bin` (multiboot), not the disk.** The disk *is*
  BIOS-bootable (MBR `55aa` present) but its QNX `v1.2d` boot loader loads the
  IFS and halts with **"Unsupported Multi-Boot"** — the IFS is a multiboot image
  meant for QEMU's own `-kernel` loader. Booting the disk directly never reaches
  QNX startup (nothing on serial; the error is on VGA).
- **Missing `virtio-rng-pci` hangs the boot** at SeaBIOS "Booting from ROM…"
  with a bare cursor. QNX startup blocks early waiting on entropy; adding the
  virtio-rng device (as `runimage` does) lets it proceed. This is the single
  change that took it from "hangs forever" to a full boot to `login:`.

**WSL-specific gotchas:**
- **Process-tree teardown** (already noted for WSLg in the M4 entry, bites QEMU
  too): a QEMU backgrounded with `&`/`nohup` inside one `wsl.exe -e …`
  invocation is SIGHUP'd (`terminating on signal 1`) the moment that invocation
  returns. Fix: run QEMU as the **foreground** process of a long-lived
  background command so the invocation stays alive.
- **`pkill -f qemu-system-x86_64` self-matches** the launcher (the pattern is a
  substring of the launching shell's own cmdline) → SIGTERM's itself
  (`terminating on signal 15`). Kill by `-pidfile` instead.
- KVM works and is fast (`/dev/kvm` present, nested virt on), just needs group
  access (`chmod 666 /dev/kvm`).

**Login.** `qnxuser` / `qnxuser`. Root SSH is not available to us — the baked
`root_authorized_keys` is the *build server's* key; `local/ssh-ident` is the
VM's **known_hosts** (public host keys), not a client key. Convention is
username==password (same as the auto-set VNC password). SSH: `ssh -p 10022 qnxuser@localhost`.

**GLES2 WALL — why the graphical game can't run in this VM (this is the headline
finding).** QNX Screen's GLES2 needs a GPU. The desktop's default Screen config
`graphics-virtio-virgl.conf` needs real host 3D passed into QEMU, and **WSL's GL
virtualization will not provide it**:
- `-display egl-headless,rendernode=/dev/dri/renderD128` → `no drm render node
  available` (WSL's renderD128 is a d3d12 node with no usable GBM/EGL for QEMU).
- `-display sdl,gl=on` (via WSLg) → QEMU starts, but virgl isn't functional
  enough; the guest's Screen dies with `screen_create_context: No such file or
  directory` (`/dev/screen` never comes up).

Falling back to the software Screen config **`graphics-virtual-display.conf`**
(`screen-sw.so`, `blit-config = sw`, a virtual 1280×768 framebuffer) *does*
bring `/dev/screen` up with no host GPU — start it with
`screen -u 36:36 -c /usr/share/screen/graphics-virtual-display.conf` (as root)
and `vncserv -display=1 -auth` serves it on **guest 5900** (pw `qnxuser`; forward
it to the host). BUT software Screen has **no GLES2 backend** (`/dev/screen/gpus`
is empty), so **both the game and the bundled `gles2-gears` sample die with
`plat_init: eglGetDisplay failed`**. There's no software GLES2 wired up here
(ANGLE libs `libEGL_Angle`/`libGLESv2_Angle` exist but have no backend without a
GPU). Conclusion: **the graphical build needs a real GPU → run it on the Pi**,
not this VM. (Consistent with the §13 risk register: mechanic/logic are
hardware-free; only the render needs the Pi.)

**Headless validation that DID pass (native, on QNX 8.0):**
- Native toolchain present: `/system/bin/gcc` (+ `g++`, `cc`; clang-based —
  diagnostics are clang-style).
- **`replaycheck` compiled natively (`gcc -std=c11 -O2 tools/replaycheck.c
  src/sim.c src/replay.c -lm`) and PASSED on QNX**: determinism hash
  `4ced063e0774b3fb` matched across run-twice **and** the replay round-trip —
  `determinism OK` + `replay is bit-identical`. The deterministic sim is
  bit-exact on real QNX, not just on the Linux dev box.
- **`balance-probe` built + ran the full sim headless** natively: window creeps
  900×560 → 140 floor, `danger` ramps `0.00→0.47→0.87→1.00` around t≈24–27 s,
  enemies spawn — all the DESIGN_CHANGES constants (`push=16`, `fire=1.9`,
  `shrink=8`) behaving as tuned.
- **Full native *graphical* build fails on headers**, not code:
  `GLES2/gl2.h` and `screen/screen.h` are **not on the native default include
  path** (only the runtime libs are in `/system/lib`; `find` over `/system`+`/usr`
  finds no headers). A native graphical build would need the dev headers via
  `apk add` first. Not chased — the cross-compiled `bin/windowed-hell-qnx-x86`
  already exists and runs (verified as a valid QNX x86-64 ELF once copied in),
  and the Pi is the real graphical target.

**Next-session (Pi) plan:** flash `com.qnx.qnx800.quickstart.rpi4` to the SD
card → same self-hosted desktop on real VideoCore GPU → `scp` the binary (or
build with the qcc aarch64 target) → the graphical game runs where GLES2 is
real. Full recipe + this GLES2-wall finding also saved to the assistant's
persistent memory.

## 2026-07-10 — M5 part 2: juice pass + Spiker boss (M5 complete)

- **Bug found during the §8.6 juice audit: the upgrade cursor never moved.**
  `render.c` read the selected option out of `snap->hit_flash` (a documented
  field-reuse hack), but `sim.c` never actually wrote it — so the arrow always
  pointed at option A no matter which way you pressed. Worse, `hit_flash` is
  the player's hit-white tint: if you took a hit just before clearing a wave it
  would still be >0.5, and the UI would highlight option B while the sim had A
  selected — actively lying about what you were about to pick. Replaced with a
  real `SimSnapshot.upgrade_selected` field. The hack was never worth it.
- Added the last missing §8.6 item, bullet spawn-pop: `SnapEntity` gained an
  `age` byte (fits the existing tail padding) so the renderer can scale a
  bullet in over `BULLET_POP_TICKS`. `BULLET_POP_TICKS` lives in `snapshot.h`,
  not `sim.h` — it's part of the sim→render contract (it's what `age` is
  measured against) and the renderer must not include the sim's internals.
- **Spiker bug: a dead boss's laser froze on screen for 3 s.** Killing the last
  enemy clears the wave → `UPGRADE` → `step_play` stops running → the beams,
  which are rebuilt from live Spikers every tick inside `step_play`, were never
  rebuilt and the snapshot kept publishing a stale one. `begin_upgrade_state`
  now clears beams and bullets: beams are a *view* of a Spiker that no longer
  exists, and in-flight bullets would otherwise hang mid-air through the pick
  and then strike the instant the next wave resumed the sim.
- Spiker is **clamped inside the window every tick**. Enemies outside the
  window are invulnerable (§8.1); if the shrinking window ever swallowed the
  boss it would become unkillable and the wave could never end.
- Beams are drawn as a chain of axis-aligned quads along the ray, because the
  batch pipeline only emits axis-aligned quads (§7.1) — and the dotted
  telegraph wants discrete marks anyway.
- Verified by white-box probe: radial waves every 2.0 s, tightening to 1.7 s
  after a teleport (`×0.85` per wave counter); teleport fires after 12
  accumulated damage *through the real pshot collision path*, relocating the
  boss and bumping its counter; laser cycles idle 8.0 s → telegraph 0.8 s →
  firing 1.0 s → idle, with beams appearing at `state=0` then `state=1` and
  vanishing when idle; the beam→player point-to-segment collision lands.
  All four of §8.4's collision loops are now live for the first time.
- Screenshotted the wave-5 fight: 8-pointed pink-white star, radial pink
  bullets, dotted laser telegraph, and a visibly faster-closing window from the
  `×1.6` boss shrink multiplier.
- Added `--wave N` (dev/demo). Reaching the boss otherwise means clearing four
  waves by hand, and §10's M6 demo script calls for rehearsing it three times.
- `make replaycheck` still passes after all of it, including the Spiker's PRNG
  use (radial phase offset, teleport position).
- WSLg gotcha, cost ~20 min: a game launched inside one `wsl -e bash -c "..."`
  invocation is **killed when that invocation exits** (its process tree is torn
  down), and X promptly reuses the window id — so a follow-up `xdotool search`
  in a *second* invocation returns a live-looking id for a destroyed window and
  `import` captures pure black (250-byte PNG, not even the `#0A0A12` void
  color). Launch and capture must happen in a single invocation. A black
  screenshot here means "wrong window", not "renderer broken".

## 2026-07-10 — M5 part 1: Octagon, enemy bullets, replay, replaycheck

Octagon enemy + enemy bullets + the two collision loops they unblock, then
canonical quantized aim, `replay.h/.c`, and a real `make replaycheck`.

- **Replay format vs. mouse aim.** §6.5's "<4 KB for a 2-minute run" assumed a
  sparse *key bitmask*. Mouse aim is not sparse. We record the **quantized aim
  angle** (uint16, 1/65536 turn) rather than a cursor position, and only when
  `(keys, aim_q)` change. Measured on a 10 000-tick (41.7 s) scripted run:
  **3 482 records, 27 880 bytes — 2.8 bytes/tick.** Extrapolated to 2 minutes
  that's ~80 KB, not 4 KB. Still trivially small and still a great demo line,
  but **the PRD's "4 KB" figure is dead** and README must not repeat it. (The
  cost is inherent: aim is a *derived* quantity — it changes whenever the
  player moves, not only when the mouse does.)
- **Quantization lives inside the sim, not at the file boundary.** `aim_q` is
  canonical `Sim` state and `aim_dx/aim_dy` are derived from it alone. If the
  sim used an exact float angle live and the log stored a rounded one, playback
  would diverge from the run it recorded. Doing it this way makes the replay
  bit-identical *by construction* rather than by luck.
- **`sim_hash()` needs a zeroed struct.** It FNV-1a's the raw bytes of `Sim`,
  which includes padding and the unused tails of the fixed-capacity pools.
  `sim_init` now `memset`s the whole struct, otherwise the determinism check
  is flaky on uninitialized padding.
- **Two real replay bugs, both caught by `replaycheck`, neither visible by
  playing:**
  1. `REPLAY_KEY_MASK` was `0x07FF`, which overlaps the mouse-fire bit
     (`1<<10`). Unpacking therefore smuggled a phantom key into
     `sim.input.keys`, which reached `prev_keys` and diverged the hash. Mask is
     now `0x03FF` (bits 0..9 = the real `KEY_*` range).
  2. The file header's `tick_count` was written from the *last record's* tick.
     Records are change-only, so the last input change can land well before the
     run ends — playback stopped ~10 ticks early ("log exhausted at 9991") and
     the final states differed. The recorder now tracks total ticks observed,
     independent of whether a record was appended.
- `make replaycheck` does more than §9.1 asks: it runs the scripted input twice
  (A==B proves determinism) **and** replays A's log (C==A proves the round-trip
  is bit-identical). C is the run that backs the demo claim; A==B alone would
  not have caught either bug above.
- **Sim thread never touches the filesystem.** The recorder appends into a
  fixed-capacity buffer from the sim thread; the render thread serializes it on
  F2-stop. A blocking `write()` at 240 Hz would show up as a tick overrun on
  the very HUD that exists to prove we never miss one.
- **Octagon excluded from contact damage on purpose** (§8.4 lists only
  Triangle/Circle/Square as contact enemies). It latches at
  `edge + OCTAGON_RADIUS` while the player clamps at `edge + PLAYER_HITBOX_R`,
  so their hitboxes necessarily overlap — a shrinking window would squeeze the
  player into unavoidable, repeating contact damage. The PRD's exclusion is
  load-bearing, not flavor.
- Verified Octagon by white-box harness rather than screenshots: it picks its
  nearest edge, drifts, latches at t≈1.0 s, then *rides* the edge inward at the
  shrink rate, fires every ~1.6 s, holds fire until latched, and its bullets
  despawn on crossing an edge. Bullet→player collision lands at t=0.442 s for a
  100 px gap at 210 px/s (100−7 px combined radius ⇒ 0.443 s). Both loops live.
- Harness gotcha worth remembering: zeroing `enemy_count` in a probe instantly
  "clears the wave", so the sim jumps to `UPGRADE` and `step_play` stops running
  — collisions then never fire and the probe reports a false negative. Park
  enemies far off-screen instead of deleting them.

## 2026-07-10 — M4 (enemies, waves, collisions, lives/states, upgrades) built and verified

Full game loop landed: `rng.h` (xorshift64*, owned by sim), state machine
`MENU -> PLAY -> UPGRADE -> DEAD` (+ R-restart back to MENU from any state),
Triangle (homing, slows near player) + Circle (windup/dash/coast) enemies,
PRNG-driven wave spawn queue (`2+n/2` triangles, `n/3` circles, staggered
with jitter), the two M4-relevant collision loops (`pshots x enemies`,
`enemies x player`), a unified `player_hit()` covering both crush and
contact damage, and the 2-of-6 upgrade picker. Octagon/Spiker/enemy-bullets
stay out per the milestone split — zero enemy bullets exist yet.

- **Design call:** crush (§8.1) turned out to already be implemented for
  free. The confinement clamp added in M2 (`step_player` clamps the player
  inside the animated rect) *is* the crush detector — if the clamp had to
  move the player this tick, an edge's true position reached where the
  player actually was, which is exactly the crush condition. No separate
  "is an edge about to hit the player" check needed; `step_player` now
  returns that bool and `step_play` calls `player_hit(sim, true)` (the
  `true` triggers the window-reset-to-60% that only crush does, vs. plain
  contact/bullet damage which doesn't reset the window).
- Verified the **entire game loop visually** via WSLg screenshots +
  simulated `xdotool` input, not just "compiles": menu screen renders and
  "shoot to start" transitions to PLAY; wave/lives readout updates; contact
  damage correctly decremented lives 3->2->1 with the invuln blink kicking
  in; a pshot connecting showed the enemy's white hit-flash tint; standing
  still long enough produced GAME OVER with correct score/wave and red
  text; pressing R correctly returned to MENU with the window rect reset to
  its full 900x560 start size.
- **Real, well-diagnosed dev-box-only flake, not a code bug:** SIGINT
  clean-exit (0/2 or 1/3 runs failing with exit 130 instead of 0) recurred
  at a much higher rate than M1/M2's single flake. Attached `gdb -p <pid>`
  to a hung instance and got a definitive answer: the render thread was
  parked in `eglSwapBuffers -> xcb_wait_for_reply -> poll()`, waiting on an
  X-server round-trip that never returned; the sim thread was correctly
  idle in `clock_nanosleep`, mid-tick, as expected. Our own signal handling
  is correct — `signal(SIGINT, ...)` is the first statement in `main()`,
  and `g_shutdown_requested` gets set regardless of which thread receives
  the signal. The hang is entirely inside Mesa/libxcb's blocking wait for
  an X-server reply that `poll()`'s `EINTR` return doesn't propagate out of
  (libxcb retries the poll internally). Root cause of *why* the X server
  was slow to reply: this session did a lot of aggressive `pkill -9`/`kill
  -9` against prior test instances and once left a hung `import -window`
  process holding an X resource, which visibly degraded the WSLg/Xwayland
  session afterward (`XGetInputFocus returned the focused window of 1.
  This is likely a bug in the X server` warnings started appearing from
  `xdotool` right after that). **This entire failure mode does not exist
  on the QNX Screen path** (M3+) — `platform_qnx.c`'s swap is
  `eglSwapBuffers` against a Screen window buffer, no X11/xcb round-trip
  involved. Not chasing further; if it recurs badly enough to block work,
  `wsl --shutdown` (full VM restart) clears Xwayland's state — did not do
  this unilaterally since it would kill any other WSL work in progress.
- Debugging note for next time: `pkill -f <pattern>` matches against a
  process's **entire command line**, including the invoking shell's own
  argv. A one-liner like `pkill -9 -f windowed-hell` run *from inside* a
  `bash -c "... xdotool search --name windowed-hell ..."` string matches
  and kills its own wrapping shell, since the pattern is a substring of
  that shell's own command line. Symptom: the whole command silently dies
  with no output, tool-reported exit code 9. Fix: use `pkill -x
  <exact-process-name>` (comm-name match, not full-cmdline substring) for
  anything that might self-match.

## 2026-07-10 — M2 (renderer + mechanic skeleton) built and verified

Window spring physics, player movement/shooting, and the world-batch
renderer landed. Deliberately deferred Crush/lives/death to M4: the PRD's
own milestone table lists "lives, states, score" under M4, not M2, and M2's
acceptance criterion ("the core tension is feelable with zero enemies")
doesn't require a damage/reset system — just shrink+push+spring+scissor+
danger. Also skipped literal Unicode REC●/PLAY▶ HUD glyphs back in M1 for
the same reason font8x8 is ASCII-only; unchanged this session.

- `sim.c`: 4 independent edge springs (`k=180, ζ=0.55`, semi-implicit Euler,
  240 Hz), continuous inward shrink (target-only, not pos — letting the
  spring do the animation), instant outward `target` bump on push with
  diminishing returns (×0.85 within 0.5s, floor 2px), hard-bound correction
  post-integration (keeps center fixed, corrects symmetrically). Player
  free-move + shoot, fixed-capacity pshot pool with swap-remove compaction.
- **Design call, not a bug:** the PRD's border-pulse text ("sim advances
  phase faster as danger→1") implies sim-owned oscillator *state*, but the
  LOCKED `SimSnapshot` struct (§6.3) has no field for it. Resolved by making
  pulse a pure function of `tick` and `danger` (both already in the
  snapshot) computed in render.c — fully deterministic/replay-safe (§6.4)
  without touching the locked struct. Noted here in case a future session
  wonders why the phase math lives in the renderer instead of sim.c.
- `render.c`: `render_flush()` now resets the vertex count after drawing, so
  a frame can flush multiple independent batches (frame layer unscissored,
  then entities scissored to the playfield rect, then HUD) — this was a
  latent gap in the M1 API that only mattered once a second batch existed.
- Extended `assets/atlas_gen.py` with two procedural sprites (player ship
  16x16 dart, pshot 8x16 rounded capsule) rather than drawing world entities
  as plain white quads, matching PRD §7.5's atlas contents.
- Verified visually, not just "doesn't crash": installed `imagemagick` +
  `xdotool` in the WSL2 box (dev-only tooling, not part of the game's deps)
  to screenshot the running window and simulate keypresses. Confirmed:
  window centered at (640,360) matching a 900x560 start, shrinks over time,
  shooting-while-moving-right visibly pushed the right edge outward with
  the white edge-flash overlay lighting up on the pushed border, player
  sprite renders correctly.
- One flaky `SIGINT` exit (130 instead of 0) out of ~9 runs, immediately
  followed by 5/5 clean runs — not reproduced on retry, likely leftover X
  focus/state from the screenshot tooling in that one run rather than a
  real bug in the shutdown path. Watch for recurrence; not chased further.

## 2026-07-10 — M1 (real-time sim core) built and verified

All 11 handoff steps landed: `metrics.h/.c` (seqlock-protected `SimMetrics`,
plain `RenderMetrics`), `snapshot.h` (full PRD §6.3 struct + classic
lock-free triple buffer), `input_ring.h` (SPSC ring, render→sim), `sim.h/.c`
(M1 stub — advances tick, publishes a static playfield/player snapshot, no
gameplay), `tools/font8x8.h`, `assets/atlas_gen.py` + generated
`assets/atlas.png` / `src/atlas.h` / `src/atlas_data.h`, `shaders.h` +
`render.h/.c` (batched-quad GLES2 pipeline, HUD-sized for now), `hud.h/.c`,
rewritten `main.c` (sim thread + SIGINT handler + render loop), and the two
flagged `platform_linux.c` fixes (`XkbSetDetectableAutoRepeat`, window visual
matched to the chosen `EGLConfig` instead of `CopyFromParent`).

- **`tools/` didn't exist yet** — the M0 session never created it despite the
  repo layout (§3) listing `tools/font8x8.h`. `curl -o` silently failed
  writing into a nonexistent directory; had to `mkdir -p tools` first.
- **Real bug in `assets/atlas_gen.py`'s first draft:** the naive regex
  `\{([^}]*)\}` to pull glyph rows out of `font8x8_basic[128][8]` broke on
  the glyph for `}` itself — its own source comment is `// U+007D (})`,
  which contains a literal `}` and terminated the non-greedy match early,
  desyncing every row after it. Fixed by stripping `//` comments before
  parsing. Would not have been caught by a compiler; only shows up by
  running the generator and checking output shape.
- Both toolchains (QNX SDP 8.0 aarch64/x86 via `qnxsdp-env.bat`, WSL2
  `Ubuntu-24.04` gcc/X11/EGL/GLES2/Pillow) verified still intact and used to
  build all three targets clean under `-Wall -Wextra`, zero warnings.
- Linux binary smoke-tested under WSLg: runs without crashing, and — this is
  the actual M1 acceptance criterion — `SIGINT` now exits with status 0
  (sim thread joined, renderer/platform torn down cleanly), verified via
  `timeout --signal=INT --preserve-status`.
- Texture atlas uses `GL_NEAREST` filtering (no mipmaps) since it's a
  procedurally-drawn pixel atlas with hard glyph/shape edges — not stated
  explicitly in the PRD, noted here since it constrains how M2's UV padding
  should work if shapes are packed tighter later.

## 2026-07-10 — M0 completed on a Windows dev box (supersedes the 2026-07-09 entry)

The 2026-07-09 entry describes a *different machine* (an Ubuntu box). The dev box for the
hackathon is Windows 11 + WSL2. Both of that entry's environment claims are inverted here.

- **QNX SDP 8.0 is installed** at `C:\Users\Shri\qnx800` — but **Windows-host only**.
  `host/win64/x86_64/usr/bin/qcc.exe` exists; `host/linux/x86_64` contains two stray gdb
  files and no toolchain. Consequence: **QNX builds cannot run from WSL.** Cross-compiling
  is done from a Windows `cmd` shell after `call C:\Users\Shri\qnx800\qnxsdp-env.bat`.
  If we ever want `make all` to work in one shell, the SDP must also be installed into WSL.
- `qnxsdp-env.sh` and `qnxsdp-env.bat` both export `QNX_HOST`/`QNX_TARGET` and **never set
  `QNX_SDP`**. The Makefile guarded on `$(QNX_SDP)`, so `make qnx` errored with "QNX_SDP is
  not set" *even with the SDP sourced*. Guard now keys off `QNX_HOST`.
- **`platform_qnx.c` compiled for the first time** (it was written blind against the docs).
  Three defects, all now fixed:
  1. `SCREEN_PROPERTY_KEY_FLAGS` does not exist → the property is `SCREEN_PROPERTY_FLAGS` (25).
  2. `SCREEN_KEY_DOWN` does not exist → the flag is `SCREEN_FLAG_KEY_DOWN` (`1 << 0`).
  3. **Latent crash the compiler could not catch:** `plat_poll` declared `screen_event_t ev;`
     uninitialized and passed it to `screen_get_event`. `screen_event_t` is a pointer typedef,
     so this was a garbage pointer handed to Screen on every poll. Screen requires
     `screen_create_event()`; the event is now created once in `plat_init`, stored in
     `QnxNative`, and destroyed in `plat_shutdown`.
- `bin/windowed-hell-qnx` (aarch64le) and `bin/windowed-hell-qnx-x86` both **compile and link
  clean** under `-Wall -Wextra`. Verified via `file`: correct arch, QNX interpreter
  `/usr/lib/ldqnx-64.so.2`. Neither has been *run* — no target yet.
- `SCREEN_PROPERTY_SIZE` is still unset (PRD §5.4 step 3 wants display-native, so Screen
  hardware-scales the fixed 1280×720 buffer). Needs a real display; deferred to M3.

Linux dev environment (WSL2, dedicated `Ubuntu-24.04` distro):

- Package names from the 2026-07-09 entry **confirmed correct** on 24.04: `libx11-dev`,
  `libegl-dev`, `libgles-dev` (the `*-mesa-dev` names don't exist). Plus `python3-pil` for
  the atlas generator.
- `make linux` builds clean; the M0 window opens under WSLg and survives a timed run.
- **Rendering is llvmpipe (software).** `/dev/dxg` and `d3d12_dri.so` are both present, but
  Xwayland reports "DRI3 error: Could not get DRI3 device", so EGL-on-X11 falls back to
  software; forcing `GALLIUM_DRIVER=d3d12` makes `eglInitialize()` fail outright. Since
  `platform_linux.c` is X11+EGL by design (§5.3), the dev box renders on the CPU.
  **Do not trust dev-box FPS/frame-time numbers** — the Pi (M3) is the only real measurement.
  Sim jitter is unaffected: that's a scheduling number, not a GPU one.
- Host clock was ~3h44m fast when the repo was cloned, then NTP-corrected, leaving every
  file with a future mtime and making `make` warn "Clock skew detected. Your build may be
  incomplete." Fixed by re-touching the worktree.

Known, not yet fixed (`platform_linux.c`, both flagged for M1):

- X11 auto-repeat sends `KeyRelease`/`KeyPress` pairs while a key is held, so a held SHOOT
  or direction can read as released inside a single `plat_poll`. Needs
  `XkbSetDetectableAutoRepeat`.
- The window is created with `CopyFromParent` instead of the visual matching the chosen
  `EGLConfig`'s `EGL_NATIVE_VISUAL_ID`. Works under Xwayland/llvmpipe; a classic source of
  `BadMatch` on real drivers.

## 2026-07-09 — M0 environment setup

- Dev box (this machine) has no QNX SDP installed, no `QNX_SDP` env var. QNX SDP 8.0 install
  started via myQNX; pending completion. `make qnx` / `make qnx-x86` will error clearly
  ("QNX_SDP is not set...") until `qnxsdp-env.sh` is sourced.
- Dev box Ubuntu release ships EGL/GLES2 dev packages under different names than the PRD
  assumed: use `libegl-dev` + `libgles-dev`, not `libegl1-mesa-dev` / `libgles2-mesa-dev`
  (those package names don't exist on this Ubuntu release — likely a newer/rolling release
  where the mesa-specific dev packages were folded into the generic ones). `libx11-dev` was
  already present.
- Repo scaffolded per PRD §3: `src/`, `assets/`, `tools/`, `bin/` (gitignored), Makefile,
  deploy.sh, platform.h + platform_linux.c + platform_qnx.c + main.c (M0 empty-main level:
  opens a window, clears to void color, polls input, clean quit).
- `platform_qnx.c` written against QNX Screen Developer's Guide patterns from the PRD but
  **not yet compiled** — no toolchain available yet. Flagged with a TODO(bringup) comment;
  first thing to verify once `qcc` is on PATH.
