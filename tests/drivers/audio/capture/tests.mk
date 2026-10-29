AUDIO_CAPTURE_DIR := $(BUILD)/audio-capture
AUDIO_CAPTURE_SOURCE := tests/drivers/audio/capture
AUDIO_CAPTURE_ISO ?= $(ISO)
AUDIO_CAPTURE_DISK ?= build/disk.img

.PHONY: test-audio-capture-oracle audio-capture-backend
# Every guest execution also invokes these controls itself. This prerequisite
# keeps a future invocation through Make from bypassing a broken instrument.
test-audio-capture-oracle:
	@mkdir -p $(AUDIO_CAPTURE_DIR)
	@python3 $(AUDIO_CAPTURE_SOURCE)/oracle.py --self-test > $(AUDIO_CAPTURE_DIR)/oracle.json
	@python3 -c 'import json; data = json.load(open("$(AUDIO_CAPTURE_DIR)/oracle.json")); print("AUDIO_CAPTURE_ORACLE: %d positive/negative trajectory and lifecycle controls passed" % len(data["cases"]))'

$(AUDIO_CAPTURE_DIR)/backend: $(AUDIO_CAPTURE_SOURCE)/backend.c
	@mkdir -p $(AUDIO_CAPTURE_DIR)
	@pkg-config --exists gio-unix-2.0 || { echo 'SKIP: gio-unix-2.0 unavailable; install GIO and rerun audio-capture-backend'; exit 2; }
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined $< $$(pkg-config --cflags --libs gio-unix-2.0) -o $@

audio-capture-backend: $(AUDIO_CAPTURE_DIR)/backend


ci-host: test-audio-capture-oracle

