# tests/gui-legacy-signed.mk -- legacy gui_* packed x/y are signed 16-bit.
#
# Host test: the ring-3 encoders (through the LOGIT_SYS_HOOK seam) and the
# GENERATED decoders in include/abi/logit_pack.h must round-trip negative
# origins for every legacy call, and a clip at (-10,-10,50,50) must intersect in
# the real fb.c. (check-abi, a separate gate, covers the generator's drift.)
#
# THE CONTROL: -DGUI_LEGACY_UNSIGNED_NEGCTL decodes with the old unsigned mask.
# It must exit 1 and fail ONLY 'FAIL signed' checks. The positive and the
# control share one source list and flag set and differ only by that -D.

GLS_SRC := tests/unit/gui_legacy_signed_test.c c/kernel/gui/fb/fb.c \
           c/kernel/gui/fb/glass.c
GLS_FLAGS := -O2 -Wall -Wextra

.PHONY: test-gui-legacy-signed test-gui-legacy-signed-negctl

test-gui-legacy-signed-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(GLS_FLAGS) -DGUI_LEGACY_UNSIGNED_NEGCTL -o $(BUILD)/gui_legacy_signed_negctl $(GLS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -Ic/apps -Ic/apps/gui -Iinclude/abi -lm
	@$(BUILD)/gui_legacy_signed_negctl > $(BUILD)/gui_legacy_signed_negctl.log 2>&1; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		cat $(BUILD)/gui_legacy_signed_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: an unsigned decode passes the signed round-trip"; exit 1; \
	elif [ $$rc -ne 1 ]; then \
		cat $(BUILD)/gui_legacy_signed_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: exit $$rc (crash, not a verdict)"; exit 1; \
	elif ! grep -q '^FAIL signed' $(BUILD)/gui_legacy_signed_negctl.log; then \
		cat $(BUILD)/gui_legacy_signed_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL signed'"; exit 1; \
	elif grep '^FAIL' $(BUILD)/gui_legacy_signed_negctl.log | grep -v '^FAIL signed' | grep -q .; then \
		cat $(BUILD)/gui_legacy_signed_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than signed failed"; exit 1; \
	else \
		echo "negative control ok: $$(grep '^FAIL signed' $(BUILD)/gui_legacy_signed_negctl.log | head -1)"; \
	fi

test-gui-legacy-signed: test-gui-legacy-signed-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(GLS_FLAGS) -o $(BUILD)/gui_legacy_signed_test $(GLS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -Ic/apps -Ic/apps/gui -Iinclude/abi -lm
	@$(BUILD)/gui_legacy_signed_test

ci-host: test-gui-legacy-signed
