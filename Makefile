# dreame-vacuum-livestream — build / deploy / control
#
# All robot transfers use `ssh '… cat > file'` because the robot's BusyBox sshd
# has no sftp-server (so `scp -O` fails). See README.
#
# Run `make help` for a description of every target.

# config.mk is git-ignored and per-user. Copy config.mk.example → config.mk first.
-include config.mk

ROBOT              ?= root@ROBOT-IP-NOT-SET
REMOTE_DIR         ?= /data/camstream
GO2RTC_VERSION     ?= v1.9.9
VACUUMSTREAMER_DIR ?= ../vacuumstreamer

# ---- derived ---------------------------------------------------------------
SSH        := ssh -o ConnectTimeout=12 $(ROBOT)
GO2RTC_URL := https://github.com/AlexxIT/go2rtc/releases/download/$(GO2RTC_VERSION)/go2rtc_linux_arm64
BUILD      := build
STAGE      := $(BUILD)/stage          # mirrors what lands in $(REMOTE_DIR) on the robot

# Files taken from your built vacuumstreamer checkout (Source A).
VM_BIN     := $(VACUUMSTREAMER_DIR)/dist/usr/bin/video_monitor
VM_SHIM    := $(VACUUMSTREAMER_DIR)/vacuumstreamer.so
VM_CONF    := $(VACUUMSTREAMER_DIR)/dist/ava/conf/video_monitor

.DEFAULT_GOAL := help

# ---------------------------------------------------------------------------
.PHONY: help
help: ## Show this help
	@echo "dreame-vacuum-livestream — targets:"
	@grep -hE '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| sort | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-16s\033[0m %s\n",$$1,$$2}'
	@echo
	@echo "Typical flow:  make build → make upload → make install → make start"

# ---------------------------------------------------------------------------
.PHONY: build
build: build-go2rtc build-phase2 stage ## Build/fetch all local artifacts into build/stage
	@echo ">> build complete → $(STAGE)"

.PHONY: build-go2rtc
build-go2rtc: $(BUILD)/go2rtc ## Download the go2rtc arm64 binary
$(BUILD)/go2rtc:
	@mkdir -p $(BUILD)
	@echo ">> downloading go2rtc $(GO2RTC_VERSION)"
	curl -fL "$(GO2RTC_URL)" -o $@ && chmod +x $@

.PHONY: build-phase2
build-phase2: ## Cross-compile the optional Phase 2 IPC relay (best-effort)
	@if command -v clang >/dev/null 2>&1; then \
		$(MAKE) -C phase2-cleaning ROBOT="$(ROBOT)" all || \
		  echo ">> Phase 2 build skipped (optional). Needs 'apt install gcc-aarch64-linux-gnu' + a reachable ROBOT for 'make -C phase2-cleaning pull-libs'. Phase 1 works without it."; \
	else \
		echo ">> clang not found — skipping Phase 2 (optional). 'apt install clang gcc-aarch64-linux-gnu'. Phase 1 works without it."; \
	fi

# Assemble everything that gets shipped to the robot.
.PHONY: stage
stage: check-vacuumstreamer build-go2rtc
	@echo ">> staging → $(STAGE)"
	@rm -rf $(STAGE) && mkdir -p $(STAGE)/ava_conf_video_monitor
	# Source A (from your vacuumstreamer checkout)
	cp $(VM_BIN)  $(STAGE)/video_monitor
	cp $(VM_SHIM) $(STAGE)/vacuumstreamer.so
	cp $(VM_CONF)/* $(STAGE)/ava_conf_video_monitor/
	# go2rtc
	cp $(BUILD)/go2rtc $(STAGE)/go2rtc
	# our scripts + go2rtc config
	cp phase1-base/*.sh $(STAGE)/
	cp phase1-base/go2rtc.yaml $(STAGE)/
	# Phase 2 binary if it was built
	@[ -f phase2-cleaning/ava_cam_relay ] && cp phase2-cleaning/ava_cam_relay $(STAGE)/ || \
		echo ">> (no Phase 2 binary — Source B disabled; Phase 1 still fully functional)"
	chmod +x $(STAGE)/*.sh $(STAGE)/video_monitor $(STAGE)/go2rtc 2>/dev/null || true

.PHONY: check-vacuumstreamer
check-vacuumstreamer:
	@test -f "$(VM_BIN)"  || { echo "ERROR: $(VM_BIN) missing. Set VACUUMSTREAMER_DIR in config.mk to a built vacuumstreamer checkout."; exit 1; }
	@test -f "$(VM_SHIM)" || { echo "ERROR: $(VM_SHIM) missing (run 'make' in your vacuumstreamer checkout first)."; exit 1; }

# ---------------------------------------------------------------------------
.PHONY: upload
upload: stage ## Push build/stage to $(REMOTE_DIR) on the robot (ssh+cat, no sftp)
	@echo ">> mkdir $(REMOTE_DIR) on robot"
	$(SSH) 'mkdir -p $(REMOTE_DIR)'
	@echo ">> uploading via tar-over-ssh (BusyBox sshd has no sftp-server)"
	tar -C $(STAGE) -cf - . | $(SSH) 'tar -C $(REMOTE_DIR) -xf - && chmod +x $(REMOTE_DIR)/*.sh $(REMOTE_DIR)/video_monitor $(REMOTE_DIR)/go2rtc $(REMOTE_DIR)/ava_cam_relay 2>/dev/null; echo "  uploaded to $(REMOTE_DIR):"; ls -la $(REMOTE_DIR)'

# ---------------------------------------------------------------------------
.PHONY: install
install: ## One-time robot setup: config overlay + persistence in _root_postboot.sh
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/install.sh'

.PHONY: uninstall
uninstall: ## Remove from robot: stop, unmount, strip the postboot block
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/install.sh uninstall' || true

# ---------------------------------------------------------------------------
.PHONY: start
start: ## Start the supervisor daemon on the robot
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) setsid sh $(REMOTE_DIR)/supervisor.sh >/data/log/camstream_supervisor.log 2>&1 < /dev/null & echo "supervisor started"'

.PHONY: stop
stop: ## Stop the supervisor + both sources (camera released)
	$(SSH) 'sh $(REMOTE_DIR)/supervisor.sh --stop; echo stopped'

.PHONY: restart
restart: stop start ## Restart the supervisor

# ---------------------------------------------------------------------------
.PHONY: status
status: ## Show robot state, active source, listening ports, stream URLs
	@$(SSH) 'sh $(REMOTE_DIR)/supervisor.sh --status'
	@echo
	@echo "Stream URLs (once a source is live):"
	@echo "  web   : http://$(word 2,$(subst @, ,$(ROBOT))):1984"
	@echo "  rtsp  : rtsp://$(word 2,$(subst @, ,$(ROBOT))):8554/camera"

.PHONY: logs
logs: ## Tail supervisor / video_monitor / relay logs on the robot
	$(SSH) 'tail -n 40 /data/log/camstream_supervisor.log /data/log/video_monitor.log /data/log/ava_cam_relay.log 2>/dev/null'

.PHONY: view
view: ## Grab one still frame to build/frame.jpg (needs ffmpeg locally) to verify the feed
	@command -v ffmpeg >/dev/null || { echo "ffmpeg not installed"; exit 1; }
	ffmpeg -y -rtsp_transport tcp -i "rtsp://$(word 2,$(subst @, ,$(ROBOT))):8554/camera" -frames:v 1 -update 1 $(BUILD)/frame.jpg
	@echo ">> saved $(BUILD)/frame.jpg"

# ---------------------------------------------------------------------------
.PHONY: clean
clean: ## Remove local build artifacts
	rm -rf $(BUILD)
	$(MAKE) -C phase2-cleaning clean 2>/dev/null || true
