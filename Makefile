.PHONY: all linux qnx qnx-x86 deploy replaycheck balance-probe clean

SRC_COMMON := src/main.c src/sim.c src/metrics.c src/render.c src/hud.c src/replay.c

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
QNX_LIBS      := -lscreen -lEGL -lGLESv2 -lm
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
