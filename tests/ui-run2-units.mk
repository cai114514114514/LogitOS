# tests/ui-run2-units.mk -- SYS_GUI_TEXT_RUN2 takes x, y and px in 24.8 logical
# points, as logit_abi.h documents and aui sends (design P0, survey C.2).
#
# HOST: tests/unit/ui_run2_units_test.c links the real handler
# (c/kernel/gui/wm_draw2_text.c) under a recording text_draw_run_a, with the
# real text.c compiled into the test's translation unit for its weight -> face
# answer. The control builds the SAME sources and flags plus -DUI_RUN2_NEGCTL
# (the old integer-point reading) and must print `FAIL run2-px24`, then reach
# the harness's `done` line (a crash is not the stated failure).
#
# GUEST: tests/qmp/qmp_ui_run2.py boots the test-aui disk (the product apps plus
# Gallery, which the product disk does not carry -- the same `$(MAKE) DISK=...
# GALLERY_AEX=...` mechanism test-aui uses), opens Gallery and Activity Monitor
# and counts the ink inside run2-drawn labels (segmented, select, badges, list
# and table cells). The control boots a kernel built into $(BUILD)-run2neg
# with -DUI_RUN2_NEGCTL against the SAME disk and must print
# `FAIL text-present` and no `FAIL harness` line.

.PHONY: test-ui-run2-units test-ui-run2-units-negctl \
        test-ui-run2-units-guest test-ui-run2-units-guest-negctl

UR2_DIR := $(BUILD)/ui-run2-units
UR2_SRC := tests/unit/ui_run2_units_test.c c/kernel/gui/wm_draw2_text.c \
           $(sort $(filter-out c/kernel/gui/text.c,$(FONT_WEIGHT_SRC)))
UR2_INC := $(FONT_WEIGHT_INC)
UR2_DEP := c/kernel/gui/text.c c/kernel/gui/text.h c/kernel/gui/wm_draw2.h \
           c/kernel/gui/wm2_scale.h include/abi/logit_abi.h

$(UR2_DIR)/current: $(UR2_SRC) $(UR2_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(UR2_INC) -o $@ $(UR2_SRC) -lm
$(UR2_DIR)/negctl: $(UR2_SRC) $(UR2_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DUI_RUN2_NEGCTL $(UR2_INC) -o $@ $(UR2_SRC) -lm

test-ui-run2-units-negctl: $(UR2_DIR)/negctl
	@rc=0; $(UR2_DIR)/negctl > $(UR2_DIR)/negctl.log 2>&1 || rc=$$?; \
	 if [ $$rc -ne 1 ]; then cat $(UR2_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	 grep -q '^FAIL run2-px24 ' $(UR2_DIR)/negctl.log || { cat $(UR2_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED: no 'FAIL run2-px24' line"; exit 1; }; \
	 grep -q '^done [0-9]* checks$$' $(UR2_DIR)/negctl.log || { cat $(UR2_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON: the harness did not finish"; exit 1; }; \
	 echo "negative control ok: $$(grep -m1 '^FAIL run2-px24 ' $(UR2_DIR)/negctl.log)"

test-ui-run2-units: test-ui-run2-units-negctl $(UR2_DIR)/current
	@rc=0; $(UR2_DIR)/current > $(UR2_DIR)/current.log 2>&1 || rc=$$?; \
	 cat $(UR2_DIR)/current.log; test $$rc -eq 0

ci-host: test-ui-run2-units

# ---- guest -----------------------------------------------------------------
ifeq ($(UR2NEG),1)
CFLAGS += -DUI_RUN2_NEGCTL
endif

UR2_CTL_BUILD := $(BUILD)-run2neg
UR2_DISK := $(BUILD)/disk_gallery.img

test-ui-run2-units-guest-negctl: $(BUILD)/gallery.aex
	@$(MAKE) --no-print-directory DISK=$(UR2_DISK) GALLERY_AEX=$(BUILD)/gallery.aex $(UR2_DISK) >/dev/null
	@mkdir -p $(UR2_DIR)
	@$(MAKE) --no-print-directory BUILD=$(UR2_CTL_BUILD) UR2NEG=1 \
		$(UR2_CTL_BUILD)/logit.iso > $(UR2_DIR)/guest_ctl_build.log 2>&1 || { \
		tail -20 $(UR2_DIR)/guest_ctl_build.log; echo "control kernel build failed"; exit 1; }
	@rc=0; python3 tests/qmp/qmp_ui_run2.py --iso $(UR2_CTL_BUILD)/logit.iso \
		--disk $(UR2_DISK) --out $(UR2_DIR)/guest-ctl > $(UR2_DIR)/guest_ctl.log 2>&1 || rc=$$?; \
	cat $(UR2_DIR)/guest_ctl.log; \
	if [ $$rc -ne 1 ]; then echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL text-present ' $(UR2_DIR)/guest_ctl.log || { \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL text-present' line"; exit 1; }; \
	if grep -q '^FAIL harness' $(UR2_DIR)/guest_ctl.log; then \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON: harness"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL text-present ' $(UR2_DIR)/guest_ctl.log)"

test-ui-run2-units-guest: test-ui-run2-units-guest-negctl $(ISO) $(BUILD)/gallery.aex
	@$(MAKE) --no-print-directory DISK=$(UR2_DISK) GALLERY_AEX=$(BUILD)/gallery.aex $(UR2_DISK) >/dev/null
	@python3 tests/qmp/qmp_ui_run2.py --iso $(ISO) --disk $(UR2_DISK) --out $(UR2_DIR)/guest

ci-boot: test-ui-run2-units-guest
