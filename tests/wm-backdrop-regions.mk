# tests/wm-backdrop-regions.mk -- real backdrop blur behind app windows:
# SYS_GUI_BACKDROP marker-byte regions (c/kernel/gui/wm_backdrop.c) composited
# by wm.c with occlusion, damage and band safety.
#
# HOST: tests/unit/wm_backdrop_test.c drives wm_backdrop.c against a striped
# fake desktop and a fake window (checks listed in its header). The control
# builds the same sources with -DWM_BACKDROP_SELF_ONLY -- the material frosts
# the app's own pixels instead of the desktop -- and must exit 1 with
# 'FAIL desktop-sample' and no other FAIL.
#
# BANDS: tests/unit/wm_bands_backdrop_test.c checks that the band splitter
# never cuts a region's sample extent (wm_backdrop_nocut + wm_bands_split, the
# chain par_nocut uses). Its control is -DWM_BANDS_NEGCTL_IGNORE_GLASS (cut
# anywhere) and must fail 'FAIL NO-CUT region' only.
#
# BUDGET: one region per frame <= 1.2x ol_display_liquid_glass_ex on the same
# rect (tests/unit/wm_backdrop_bench.c).
#
# GUEST: tests/fixtures/wm_backdrop_probe.c (/bin/backdrop-probe) opens a
# window with one sidebar SYS_GUI_BACKDROP and text drawn inside it;
# tests/qmp/qmp_backdrop.py checks the wallpaper's variance under the sidebar
# and that the text is crisp. The control boots a kernel built with
# -DWM_BACKDROP_SELF_ONLY (BACKDROPCTL=1, its own BUILD dir, the SAME probe
# image) and the variance check must fail.

.PHONY: test-wm-backdrop-regions test-wm-backdrop-regions-negctl \
        test-wm-backdrop-bands test-wm-backdrop-bands-negctl \
        test-wm-backdrop-regions-budget \
        test-wm-backdrop-regions-guest test-wm-backdrop-regions-guest-negctl

WMBD_SRC := tests/unit/wm_backdrop_test.c c/kernel/gui/wm_backdrop.c
WMBD_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2

test-wm-backdrop-regions-negctl:
	@mkdir -p $(BUILD)/wmbd
	@$(CC) $(WMBD_FLAGS) -DWM_BACKDROP_SELF_ONLY -o $(BUILD)/wmbd/wm_backdrop_negctl $(WMBD_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@rc=0; $(BUILD)/wmbd/wm_backdrop_negctl > $(BUILD)/wmbd/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/wmbd/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL desktop-sample' $(BUILD)/wmbd/negctl.log || { cat $(BUILD)/wmbd/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL desktop-sample'"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wmbd/negctl.log | grep -qv '^FAIL desktop-sample'; then \
		cat $(BUILD)/wmbd/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than desktop-sample failed"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL desktop-sample' $(BUILD)/wmbd/negctl.log)"

test-wm-backdrop-regions: test-wm-backdrop-regions-negctl test-wm-backdrop-bands
	@mkdir -p $(BUILD)/wmbd
	@$(CC) $(WMBD_FLAGS) -o $(BUILD)/wmbd/wm_backdrop_test $(WMBD_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/wmbd/wm_backdrop_test

WMBB_SRC := tests/unit/wm_bands_backdrop_test.c c/kernel/gui/wm_backdrop.c
WMBB_FLAGS := -O2 -Wall -Wextra

test-wm-backdrop-bands-negctl:
	@mkdir -p $(BUILD)/wmbd
	@$(CC) $(WMBB_FLAGS) -DWM_BANDS_NEGCTL_IGNORE_GLASS -o $(BUILD)/wmbd/wm_bands_backdrop_negctl $(WMBB_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@rc=0; $(BUILD)/wmbd/wm_bands_backdrop_negctl > $(BUILD)/wmbd/bands_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/wmbd/bands_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL NO-CUT region' $(BUILD)/wmbd/bands_negctl.log || { cat $(BUILD)/wmbd/bands_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL NO-CUT region'"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wmbd/bands_negctl.log | grep -qv '^FAIL NO-CUT region'; then \
		cat $(BUILD)/wmbd/bands_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: another check failed"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL NO-CUT region' $(BUILD)/wmbd/bands_negctl.log)"

test-wm-backdrop-bands: test-wm-backdrop-bands-negctl
	@mkdir -p $(BUILD)/wmbd
	@$(CC) $(WMBB_FLAGS) -o $(BUILD)/wmbd/wm_bands_backdrop_test $(WMBB_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/wmbd/wm_bands_backdrop_test

ci-host: test-wm-backdrop-regions

test-wm-backdrop-regions-budget:
	@mkdir -p $(BUILD)/wmbd
	@$(CC) -O2 -g -Wall -Wextra -DGFX_NCTX=2 -Itests/unit -o $(BUILD)/wmbd/wm_backdrop_bench \
		tests/unit/wm_backdrop_bench.c c/kernel/gui/wm_backdrop.c $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/wmbd/wm_backdrop_bench

test-gfx-budget: test-wm-backdrop-regions-budget

# ---- guest -----------------------------------------------------------------
# The probe is a plain logit.h client (crt0 + fixture + $(GFX_OBJ)), installed
# as /bin/backdrop-probe into the test image only under WM_BACKDROP_GUEST=1;
# FS_SPECS is untouched.
ifeq ($(BACKDROPCTL),1)
CFLAGS += -DWM_BACKDROP_SELF_ONLY
endif

WMBD_CTL_BUILD := $(BUILD)-wmbd-negctl

$(BUILD)/wmbd/fixture.o: tests/fixtures/wm_backdrop_probe.c c/apps/logit.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/wmbd/crt0.o: c/apps/crt0.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(BUILD)/wmbd/backdrop.elf: $(BUILD)/wmbd/crt0.o $(BUILD)/wmbd/fixture.o $(AUI_OBJ) $(GFX_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(BUILD)/wmbd/installed.stamp: $(BUILD)/wmbd/backdrop.elf $(BUILD)/sdk/installed.stamp
	@mkdir -p $(SYSROOT)/bin
	cp $(BUILD)/wmbd/backdrop.elf $(SYSROOT)/bin/backdrop-probe
	@touch $@
ifeq ($(WM_BACKDROP_GUEST),1)
$(DISK): $(BUILD)/wmbd/installed.stamp
endif

test-wm-backdrop-regions-guest-negctl:
	@$(MAKE) --no-print-directory BUILD=$(BUILD) WM_BACKDROP_GUEST=1 $(DISK) >/dev/null
	@$(MAKE) --no-print-directory BUILD=$(WMBD_CTL_BUILD) BACKDROPCTL=1 \
		$(WMBD_CTL_BUILD)/logit.iso > $(BUILD)/wmbd_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_backdrop.py --iso $(WMBD_CTL_BUILD)/logit.iso \
		--disk $(DISK) --out $(BUILD)/wmbd-guest-ctl > $(BUILD)/wmbd_ctl.log 2>&1 || rc=$$?; \
	cat $(BUILD)/wmbd_ctl.log; \
	if [ $$rc -ne 1 ]; then echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL backdrop-variance' $(BUILD)/wmbd_ctl.log || { \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL backdrop-variance' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wmbd_ctl.log | grep -qv '^FAIL backdrop-variance'; then \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: frosting the app's own pixels fails the variance check"

test-wm-backdrop-regions-guest: test-wm-backdrop-regions-guest-negctl
	@$(MAKE) --no-print-directory BUILD=$(BUILD) WM_BACKDROP_GUEST=1 $(DISK) $(ISO) >/dev/null
	@python3 tests/qmp/qmp_backdrop.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/wmbd-guest

ci-boot: test-wm-backdrop-regions-guest
