AUDIO_CAPTURE_DIR := $(BUILD)/audio-capture
AUDIO_CAPTURE_SOURCE := tests/drivers/audio/capture
AUDIO_CAPTURE_ISO ?= $(ISO)
AUDIO_CAPTURE_DISK ?= build/disk.img

.PHONY: test-audio-capture-oracle audio-capture-backend test-audio-capture-os test-audio-multicard-os
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

test-audio-capture-os: test-audio-capture-oracle audio-capture-backend test-audio-cards-host $(AUDIO_CAPTURE_ISO)
	@test -f "$(AUDIO_CAPTURE_DISK)" || { echo 'FAIL: existing audio capture disk template is missing'; exit 1; }
	@capture_run_root=$$(mktemp -d "$(AUDIO_CAPTURE_DIR)/guest.XXXXXX") && \
	 echo "AUDIO_CAPTURE_GUEST: $$capture_run_root" && \
	 for device in intel-hda AC97 ES1370; do \
	  ASAN_OPTIONS=detect_leaks=0 python3 $(AUDIO_CAPTURE_SOURCE)/guest.py \
	    --iso "$(AUDIO_CAPTURE_ISO)" --disk "$(AUDIO_CAPTURE_DISK)" --device $$device \
	    --backend "$(AUDIO_CAPTURE_DIR)/backend" --output "$$capture_run_root/$$device" || exit 1; \
	done

ci-host: test-audio-capture-oracle

# HDA has no ADC in these fixtures. Samples must therefore pass through the
# second card even though its playback registration was declined by the mixer.
test-audio-multicard-os: test-audio-capture-oracle audio-capture-backend test-audio-cards-host $(AUDIO_CAPTURE_ISO)
	@test -f "$(AUDIO_CAPTURE_DISK)" || { echo 'FAIL: existing audio capture disk template is missing'; exit 1; }
	@capture_run_root=$$(mktemp -d "$(AUDIO_CAPTURE_DIR)/multicard.XXXXXX") && \
	 echo "AUDIO_MULTICARD_GUEST: $$capture_run_root" && \
	 for device in AC97 ES1370; do \
	  ASAN_OPTIONS=detect_leaks=0 python3 $(AUDIO_CAPTURE_SOURCE)/guest.py \
	    --iso "$(AUDIO_CAPTURE_ISO)" --disk "$(AUDIO_CAPTURE_DISK)" --device $$device --output-device intel-hda \
	    --backend "$(AUDIO_CAPTURE_DIR)/backend" --output "$$capture_run_root/$$device" || exit 1; \
	done
