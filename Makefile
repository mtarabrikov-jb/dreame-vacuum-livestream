# dreame-vacuum-livestream — build / deploy / control
#
# All robot transfers use `tar | ssh 'tar -x'` because the robot's BusyBox sshd
# has no sftp-server (so `scp -O` fails). See README.
#
# Run `make help` for a description of every target.

# config.mk is git-ignored and per-user. Copy config.mk.example → config.mk first.
-include config.mk

ROBOT              ?= root@ROBOT-IP-NOT-SET
REMOTE_DIR         ?= /data/camstream
GO2RTC_VERSION     ?= v1.9.9

# ---- derived ---------------------------------------------------------------
SSH        := ssh -o ConnectTimeout=12 $(ROBOT)
GO2RTC_URL := https://github.com/AlexxIT/go2rtc/releases/download/$(GO2RTC_VERSION)/go2rtc_linux_arm64
BUILD      := build
# STAGE mirrors what lands in $(REMOTE_DIR) on the robot
STAGE      := $(BUILD)/stage

.DEFAULT_GOAL := help

# ---------------------------------------------------------------------------
.PHONY: help
help: ## Show this help
	@echo "dreame-vacuum-livestream — targets:"
	@grep -hE '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| sort | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-16s\033[0m %s\n",$$1,$$2}'
	@echo
	@echo "Typical flow:  make build → make upload → make install → make watch"

# ---------------------------------------------------------------------------
.PHONY: build
build: build-go2rtc build-shim stage ## Build/fetch all local artifacts into build/stage
	@echo ">> build complete → $(STAGE)"

.PHONY: build-go2rtc
build-go2rtc: $(BUILD)/go2rtc ## Download the go2rtc arm64 binary
$(BUILD)/go2rtc:
	@mkdir -p $(BUILD)
	@echo ">> downloading go2rtc $(GO2RTC_VERSION)"
	curl -fL "$(GO2RTC_URL)" -o $@ && chmod +x $@

.PHONY: build-shim
build-shim: ## Cross-compile the in-ava camera shim + relay (Docker preferred)
	@if command -v docker >/dev/null 2>&1; then \
		$(MAKE) -C phase2-cleaning docker; \
	elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then \
		$(MAKE) -C phase2-cleaning all; \
	else \
		echo ">> ERROR: need docker or gcc-aarch64-linux-gnu to build libcamtap.so + ava_cam_relay"; exit 1; \
	fi

# Assemble everything that gets shipped to the robot.
.PHONY: stage
stage: build-go2rtc
	@echo ">> staging → $(STAGE)"
	@rm -rf $(STAGE) && mkdir -p $(STAGE)
	cp $(BUILD)/go2rtc $(STAGE)/go2rtc
	cp phase2-cleaning/inject-ava.sh phase2-cleaning/run_ir.sh phase2-cleaning/go2rtc_ir.yaml $(STAGE)/
	@[ -f phase2-cleaning/ava_cam_relay ] && cp phase2-cleaning/ava_cam_relay $(STAGE)/ || \
		echo ">> (no ava_cam_relay — run 'make build-shim')"
	@[ -f phase2-cleaning/libcamtap.so ] && cp phase2-cleaning/libcamtap.so $(STAGE)/ || \
		echo ">> (no libcamtap.so — run 'make build-shim')"
	chmod +x $(STAGE)/*.sh $(STAGE)/go2rtc 2>/dev/null || true

# ---------------------------------------------------------------------------
.PHONY: upload
upload: stage ## Push build/stage to $(REMOTE_DIR) on the robot (tar-over-ssh, no sftp)
	@echo ">> mkdir $(REMOTE_DIR) on robot"
	$(SSH) 'mkdir -p $(REMOTE_DIR)'
	@echo ">> uploading via tar-over-ssh (BusyBox sshd has no sftp-server)"
	tar -C $(STAGE) -cf - . | $(SSH) 'tar -C $(REMOTE_DIR) -xf - && chmod +x $(REMOTE_DIR)/*.sh $(REMOTE_DIR)/go2rtc $(REMOTE_DIR)/ava_cam_relay 2>/dev/null; echo "  uploaded to $(REMOTE_DIR):"; ls -la $(REMOTE_DIR)'

# ---- install: inject the shim into ava + start the feed --------------------
.PHONY: install
install: ## Inject the camera shim into ava + start the feed (RESTARTS ava)
	@echo ">> This restarts ava (navigation). Robot should be idle on the dock."
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/inject-ava.sh install'

.PHONY: uninstall
uninstall: ## Remove the ava tap and restart stock ava
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/inject-ava.sh remove' || true

.PHONY: status
status: ## Show whether the in-ava tap is active
	@$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/inject-ava.sh status'

.PHONY: start
start: ## Start the camera feed (relay + go2rtc) on the robot
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/run_ir.sh'

.PHONY: stop
stop: ## Stop the camera feed (relay + go2rtc)
	$(SSH) 'REMOTE_DIR=$(REMOTE_DIR) sh $(REMOTE_DIR)/run_ir.sh --stop'

.PHONY: restart
restart: stop start ## Restart the camera feed (relay + go2rtc)

.PHONY: watch
watch: ## Print the URLs to watch the camera feed (via go2rtc)
	@host="$(word 2,$(subst @, ,$(ROBOT)))"; \
	echo "Live camera feed — color RGB on the dock, infrared while cleaning:"; \
	echo "  Web UI : http://$$host:1984/  (stream: camera)"; \
	echo "  RTSP   : rtsp://$$host:8554/camera    (VLC: Media > Open Network Stream)"; \
	echo "  WebRTC : http://$$host:1984/webrtc.html?src=camera"; \
	echo "  Snap   : http://$$host:1984/api/frame.jpeg?src=camera"

# ---------------------------------------------------------------------------
.PHONY: logs
logs: ## Tail the relay + go2rtc logs on the robot
	$(SSH) 'tail -n 40 /data/log/ir_relay.log /data/log/go2rtc.log 2>/dev/null'

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
