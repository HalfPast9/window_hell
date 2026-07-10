# Bring-up log

Timestamped notes on every QNX/Pi/toolchain gotcha encountered. Newest entries at top.

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
