# tests/wm-rounded-content.mk -- app content is clipped to the window's rounded
# shape (wm.c: blit_content / anim_blit_fade through an opacity-255 snapshot
# group), and the opaque-content shortcut never claims a rectangle that touches
# a rounded corner (c/kernel/gui/wm_cover.h).
#
# HOST: tests/unit/wm_cover_test.c drives the pure cover predicate. The control
# builds the same source with -DWM_COVER_NEGCTL_IGNORE_CORNERS (the predicate
# ignores corners) and must fail 'FAIL corner-cover'.
#
# GUEST: tests/fixtures/wm_round_probe.c fills its window red; tests/qmp/
# qmp_round_corner.py boots it over the wallpaper and samples the bottom
# corners against the analytic coverage. The control boots a kernel built with
# -DWM_SQUARE_CONTENT (the old square blit) and the same check must fail.

.PHONY: test-wm-rounded-content test-wm-rounded-content-negctl \
        test-wm-rounded-content-guest test-wm-rounded-content-guest-negctl

WMRC_FLAGS := -O2 -Wall -Wextra -Ic/kernel/gui -Iinclude/abi $(GFX_INC)
WMRC_DEPS := tests/unit/wm_cover_test.c c/kernel/gui/wm_cover.h c/kernel/gui/wm_draw2.h

$(BUILD)/wm_cover_test: $(WMRC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(WMRC_FLAGS) tests/unit/wm_cover_test.c -o $@

$(BUILD)/wm_cover_negctl: $(WMRC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(WMRC_FLAGS) -DWM_COVER_NEGCTL_IGNORE_CORNERS tests/unit/wm_cover_test.c -o $@

test-wm-rounded-content-negctl: $(BUILD)/wm_cover_negctl
	@rc=0; $< > $(BUILD)/wm_cover_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/wm_cover_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL corner-cover' $(BUILD)/wm_cover_negctl.log || { cat $(BUILD)/wm_cover_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL corner-cover' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/wm_cover_negctl.log | grep -qv '^FAIL corner-cover'; then \
		cat $(BUILD)/wm_cover_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL corner-cover' $(BUILD)/wm_cover_negctl.log)"

test-wm-rounded-content: test-wm-rounded-content-negctl $(BUILD)/wm_cover_test
	@$(BUILD)/wm_cover_test

ci-host: test-wm-rounded-content

# ---- guest -----------------------------------------------------------------
# The probe is a plain logit.h client (crt0 + fixture + $(GFX_OBJ)), installed
# as /bin/round-probe into the isolated test image only under WM_ROUND_GUEST=1;
# FS_SPECS is untouched. The control kernel is a second BUILD dir compiled with
# -DWM_SQUARE_CONTENT (ROUNDCTL=1): the old square content blit. It boots the
# SAME probe image and the corner check must fail.
ifeq ($(ROUNDCTL),1)
CFLAGS += -DWM_SQUARE_CONTENT
endif

WMRC_CTL_BUILD := $(BUILD)-wmrc-negctl

$(BUILD)/wmrc/fixture.o: tests/fixtures/wm_round_probe.c c/apps/logit.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/wmrc/crt0.o: c/apps/crt0.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(BUILD)/wmrc/round.elf: $(BUILD)/wmrc/crt0.o $(BUILD)/wmrc/fixture.o $(AUI_OBJ) $(GFX_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(BUILD)/wmrc/installed.stamp: $(BUILD)/wmrc/round.elf $(BUILD)/sdk/installed.stamp
	@mkdir -p $(SYSROOT)/bin
	cp $(BUILD)/wmrc/round.elf $(SYSROOT)/bin/round-probe
	@touch $@
ifeq ($(WM_ROUND_GUEST),1)
$(DISK): $(BUILD)/wmrc/installed.stamp
endif

test-wm-rounded-content-guest-negctl:
	@$(MAKE) --no-print-directory BUILD=$(BUILD) WM_ROUND_GUEST=1 $(DISK) >/dev/null
	@$(MAKE) --no-print-directory BUILD=$(WMRC_CTL_BUILD) ROUNDCTL=1 \
		$(WMRC_CTL_BUILD)/logit.iso > $(BUILD)/wmrc_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_round_corner.py --iso $(WMRC_CTL_BUILD)/logit.iso \
		--disk $(DISK) --out $(BUILD)/wmrc-guest-ctl > $(BUILD)/wmrc_ctl.log 2>&1 || rc=$$?; \
	cat $(BUILD)/wmrc_ctl.log; \
	if [ $$rc -ne 1 ]; then echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL corner-outside' $(BUILD)/wmrc_ctl.log || { \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL corner-outside' line"; exit 1; }; \
	if grep -q '^FAIL harness' $(BUILD)/wmrc_ctl.log; then \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON: harness"; exit 1; fi; \
	echo "negative control ok: square content fails the corner check"

test-wm-rounded-content-guest: test-wm-rounded-content-guest-negctl
	@$(MAKE) --no-print-directory BUILD=$(BUILD) WM_ROUND_GUEST=1 $(DISK) $(ISO) >/dev/null
	@python3 tests/qmp/qmp_round_corner.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/wmrc-guest

ci-boot: test-wm-rounded-content-guest
