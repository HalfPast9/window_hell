.PHONY: all linux qnx qnx-x86 native deploy replaycheck mp-check balance-probe clean

SRC_COMMON := src/main.c src/sim.c src/metrics.c src/render.c src/hud.c src/replay.c src/handtrack.c src/netplay.c

BIN_DIR := bin

all: linux qnx

# --- Linux dev build ---
LINUX_CC      := cc
LINUX_CFLAGS  := -std=c11 -O2 -g -Wall -Wextra
LINUX_LIBS    := -lX11 -lEGL -lGLESv2 -lpthread -lm
LINUX_SRC     := $(SRC_COMMON) src/platform_linux.c
LINUX_BIN     := $(BIN_DIR)/windowed-hell-linux

linux: $(LINUX_BIN)

$(LINUX_BIN): $(LINUX_SRC) src/platform.h | $(BIN_DIR)
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ $(LINUX_SRC) $(LINUX_LIBS)

# --- QNX aarch64 (Pi 4B target) build ---
# Requires: source $$QNX_SDP_INSTALL_DIR/qnxsdp-env.sh
# (that script exports QNX_HOST/QNX_TARGET, never QNX_SDP — guard on QNX_HOST)
QNX_CC        := qcc -Vgcc_ntoaarch64le
QNX_CFLAGS    := -std=c11 -O2 -g -Wall -Wextra
QNX_LIBS      := -lscreen -lEGL -lGLESv2 -lm -lsocket
QNX_SRC       := $(SRC_COMMON) src/platform_qnx.c
QNX_BIN       := $(BIN_DIR)/windowed-hell-qnx

qnx: | $(BIN_DIR)
ifeq ($(QNX_HOST),)
	$(error QNX_HOST is not set. Run: source $$QNX_SDP_INSTALL_DIR/qnxsdp-env.sh)
endif
	$(QNX_CC) $(QNX_CFLAGS) -o $(QNX_BIN) $(QNX_SRC) $(QNX_LIBS)

# --- QNX x86_64 (VM smoke test) build ---
QNXX86_CC     := qcc -Vgcc_ntox86_64
QNXX86_BIN    := $(BIN_DIR)/windowed-hell-qnx-x86

qnx-x86: | $(BIN_DIR)
ifeq ($(QNX_HOST),)
	$(error QNX_HOST is not set. Run: source $$QNX_SDP_INSTALL_DIR/qnxsdp-env.sh)
endif
	$(QNXX86_CC) $(QNX_CFLAGS) -o $(QNXX86_BIN) $(QNX_SRC) $(QNX_LIBS)

# --- QNX native build (run ON the self-hosted developer desktop itself,
# e.g. the Pi) — no cross toolchain, no qcc, no Windows SDP involved. Uses the
# box's own gcc/clang against on-device headers, which are NOT installed by
# default: `sudo apk add qnx-screen-dev gles-headers egl-headers qnx-khr-dev`.
# Source has to physically be on the machine (git clone or scp) since this is
# a local build, not a target/host split.
NATIVE_CC     := gcc
NATIVE_CFLAGS := -std=c11 -O2 -Wall -Wextra
NATIVE_LIBS   := -lscreen -lEGL -lGLESv2 -lm -lsocket
NATIVE_SRC    := $(SRC_COMMON) src/platform_qnx.c
NATIVE_BIN    := $(BIN_DIR)/windowed-hell-qnx-native

native: | $(BIN_DIR)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -o $(NATIVE_BIN) $(NATIVE_SRC) $(NATIVE_LIBS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# --- Deploy to Pi target over ssh/scp ---
deploy: qnx
	./deploy.sh

# --- Determinism CI: same scripted input twice + a replay round-trip,
# all three must hash identically (§9.1). Headless: no GL, no X.
REPLAYCHECK_BIN := $(BIN_DIR)/replaycheck

replaycheck: $(REPLAYCHECK_BIN)
	@$(REPLAYCHECK_BIN)

$(REPLAYCHECK_BIN): tools/replaycheck.c src/sim.c src/replay.c src/sim.h src/replay.h | $(BIN_DIR)
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ tools/replaycheck.c src/sim.c src/replay.c -lm

# --- Lockstep determinism CI: the multiplayer analogue of replaycheck, over
# real loopback UDP sockets (no mocked networking) — see tools/mp_check.c.
MP_CHECK_BIN := $(BIN_DIR)/mp-check

mp-check: $(MP_CHECK_BIN)
	@$(MP_CHECK_BIN)

$(MP_CHECK_BIN): tools/mp_check.c src/sim.c src/netplay.c src/sim.h src/netplay.h | $(BIN_DIR)
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ tools/mp_check.c src/sim.c src/netplay.c -lm

# --- Headless balance harness: drives sim_step with scripted input, no GL/X.
# Answers window-tuning questions (does pushing outpace shrink?) against the
# real sim instead of by eyeballing a software-rendered window.
BALANCE_PROBE_BIN := $(BIN_DIR)/balance-probe

balance-probe: $(BALANCE_PROBE_BIN)
	@$(BALANCE_PROBE_BIN)

$(BALANCE_PROBE_BIN): tools/balance_probe.c src/sim.c src/sim.h src/snapshot.h | $(BIN_DIR)
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ tools/balance_probe.c src/sim.c -lm

clean:
	rm -rf $(BIN_DIR)
