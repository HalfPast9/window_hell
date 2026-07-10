.PHONY: all linux qnx qnx-x86 deploy replaycheck clean

SRC_COMMON := src/main.c

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
# Requires: source $$QNX_SDP/qnxsdp-env.sh
QNX_CC        := qcc -Vgcc_ntoaarch64le
QNX_CFLAGS    := -std=c11 -O2 -g -Wall -Wextra
QNX_LIBS      := -lscreen -lEGL -lGLESv2 -lm
QNX_SRC       := $(SRC_COMMON) src/platform_qnx.c
QNX_BIN       := $(BIN_DIR)/windowed-hell-qnx

qnx: | $(BIN_DIR)
ifeq ($(QNX_SDP),)
	$(error QNX_SDP is not set. Run: source $$QNX_SDP_INSTALL_DIR/qnxsdp-env.sh)
endif
	$(QNX_CC) $(QNX_CFLAGS) -o $(QNX_BIN) $(QNX_SRC) $(QNX_LIBS)

# --- QNX x86_64 (VM smoke test) build ---
QNXX86_CC     := qcc -Vgcc_ntox86_64
QNXX86_BIN    := $(BIN_DIR)/windowed-hell-qnx-x86

qnx-x86: | $(BIN_DIR)
ifeq ($(QNX_SDP),)
	$(error QNX_SDP is not set. Run: source $$QNX_SDP_INSTALL_DIR/qnxsdp-env.sh)
endif
	$(QNXX86_CC) $(QNX_CFLAGS) -o $(QNXX86_BIN) $(QNX_SRC) $(QNX_LIBS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# --- Deploy to Pi target over ssh/scp ---
deploy: qnx
	./deploy.sh

# --- Determinism CI: run scripted input twice, diff final sim hash ---
replaycheck: linux
	@echo "replaycheck: not wired up yet (needs sim.c + replay.c, M1+)"

clean:
	rm -rf $(BIN_DIR)
