# tests/wm-shadow-adopt.mk -- WM shadows are two ol_display_shadow_ex layers
# (key + ambient, c/kernel/gui/wm_shadow.h), no 72 px mask clamp at 200%.
#
# HOST: tests/unit/wm_shadow_extent_test.c includes wm_shadow.h and links the
# real shadow_ex: every pixel the presets write lies inside wm_shadow_extent,
# and the 200% falloff is the 100% one scaled by 2. The control builds the same
# sources with -DWM_SHADOW_EXTENT_NEGCTL (extent = blur only) and must exit 1
# with 'FAIL extent' and nothing else.
#
# GUEST: tests/qmp/qmp_wm_shadow.py boots the desktop, measures the falloff
# beside the Finder window against the host oracle (the test's --profile mode)
# at 100% (1280x800) and 200% (2560x1600), tolerance 6/255. The control boots a
# kernel built with -DWM_SHADOW_LEGACY (SHADOWCTL=1, its own BUILD dir, legacy
# clamped fb_shadow) at 200% and must fail 'FAIL falloff-200' only.

.PHONY: test-wm-shadow-adopt test-wm-shadow-adopt-negctl \
        test-wm-shadow-adopt-guest test-wm-shadow-adopt-guest-negctl

WMSH_SRC := tests/unit/wm_shadow_extent_test.c $(GFX_SRC)
WMSH_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC) $(HOST_INCDIRS) -Ic/kernel/gui
WMSH_DEPS := $(WMSH_SRC) $(GFX_HEADERS) c/kernel/gui/wm_shadow.h

$(BUILD)/wmsh/wm_shadow_extent_test: $(WMSH_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WMSH_FLAGS) $(WMSH_SRC) -lm -o $@

$(BUILD)/wmsh/wm_shadow_extent_negctl: $(WMSH_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WMSH_FLAGS) -DWM_SHADOW_EXTENT_NEGCTL $(WMSH_SRC) -lm -o $@

test-wm-shadow-adopt-negctl: $(BUILD)/wmsh/wm_shadow_extent_negctl
	@rc=0; $(BUILD)/wmsh/wm_shadow_extent_negctl > $(BUILD)/wmsh/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/wmsh/negctl.log; echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL extent' $(BUILD)/wmsh/negctl.log || { cat $(BUILD)/wmsh/negctl.log; echo "NEGATIVE CONTROL FAILED: no 'FAIL extent' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wmsh/negctl.log | grep -qv '^FAIL extent'; then cat $(BUILD)/wmsh/negctl.log; echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL extent' $(BUILD)/wmsh/negctl.log)"

test-wm-shadow-adopt: test-wm-shadow-adopt-negctl $(BUILD)/wmsh/wm_shadow_extent_test
	$(BUILD)/wmsh/wm_shadow_extent_test

ci-host: test-wm-shadow-adopt

# ---- guest -----------------------------------------------------------------
ifeq ($(SHADOWCTL),1)
CFLAGS += -DWM_SHADOW_LEGACY
endif

WMSH_CTL_BUILD := $(BUILD)-wmsh-negctl

test-wm-shadow-adopt-guest-negctl: $(BUILD)/wmsh/wm_shadow_extent_test
	@$(MAKE) --no-print-directory BUILD=$(BUILD) $(DISK) >/dev/null
	@$(MAKE) --no-print-directory BUILD=$(WMSH_CTL_BUILD) SHADOWCTL=1 \
		$(WMSH_CTL_BUILD)/logit.iso > $(BUILD)/wmsh_ctl_build.log 2>&1 || { tail -20 $(BUILD)/wmsh_ctl_build.log; exit 1; }
	@rc=0; python3 tests/qmp/qmp_wm_shadow.py --iso $(WMSH_CTL_BUILD)/logit.iso \
		--disk $(DISK) --oracle $(BUILD)/wmsh/wm_shadow_extent_test \
		--xres 2560 --yres 1600 --out $(BUILD)/wmsh-guest-ctl > $(BUILD)/wmsh_ctl.log 2>&1 || rc=$$?; \
	cat $(BUILD)/wmsh_ctl.log; \
	if [ $$rc -ne 1 ]; then echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL falloff-200' $(BUILD)/wmsh_ctl.log || { echo "NEGATIVE CONTROL FAILED: no 'FAIL falloff-200' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wmsh_ctl.log | grep -qv '^FAIL falloff-200'; then echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the legacy clamped shadow fails the 200% falloff"

test-wm-shadow-adopt-guest: test-wm-shadow-adopt-guest-negctl
	@$(MAKE) --no-print-directory BUILD=$(BUILD) $(DISK) $(ISO) >/dev/null
	@python3 tests/qmp/qmp_wm_shadow.py --iso $(ISO) --disk $(DISK) --oracle $(BUILD)/wmsh/wm_shadow_extent_test \
		--xres 1280 --yres 800 --out $(BUILD)/wmsh-guest-100
	@python3 tests/qmp/qmp_wm_shadow.py --iso $(ISO) --disk $(DISK) --oracle $(BUILD)/wmsh/wm_shadow_extent_test \
		--xres 2560 --yres 1600 --out $(BUILD)/wmsh-guest-200

ci-boot: test-wm-shadow-adopt-guest
