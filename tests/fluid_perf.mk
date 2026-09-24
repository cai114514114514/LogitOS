# tests/fluid_perf.mk -- what one interaction costs the compositor, as a
# ceiling that a damage regression has to break (the "Fluid" release, P0).
#
# test-type-damage: a keystroke into TextEdit must recomposite at most 35% of
# the screen per frame at 1920x1200. Measured 2026-09-24: 78.6% before, 18.1%
# after, from two fixes -- aui reports a SET of rectangles instead of their
# bounding box (c/apps/gui/aui_damage.h), and the window manager no longer
# damages the whole window on every key (wm.c, the M8-era "the app repaints").
#
# The control is a kernel built with -DWM_KEY_PREDIRTY, which restores the
# per-key whole-window damage and nothing else; the same boot, workload and
# ceiling must FAIL with it. It runs first.

.PHONY: test-type-damage test-type-damage-negctl

ifeq ($(KEYPREDIRTY),1)
CFLAGS += -DWM_KEY_PREDIRTY
endif
TYPE_DAMAGE_CTL_BUILD := $(BUILD)-keypredirty
TYPE_DAMAGE_MAX := 0.35

test-type-damage-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(TYPE_DAMAGE_CTL_BUILD) KEYPREDIRTY=1 \
		$(TYPE_DAMAGE_CTL_BUILD)/logit.iso > $(BUILD)/type_damage_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_repaint.py --xres 1920 --yres 1200 --reps 1 \
		--only type --assert-max-frac type=$(TYPE_DAMAGE_MAX) \
		--iso $(TYPE_DAMAGE_CTL_BUILD)/logit.iso --disk $(DISK) \
		> $(BUILD)/type_damage_ctl.log 2>&1 || rc=$$?; \
		grep -E "max-frac|type  " $(BUILD)/type_damage_ctl.log; \
		if [ $$rc -eq 1 ] && grep -q '^FAIL max-frac type:' $(BUILD)/type_damage_ctl.log; then \
			echo "negative control ok: per-key whole-window damage breaks the ceiling"; \
		else echo "NEGATIVE CONTROL FAILED: the ceiling holds with per-key damage restored"; exit 1; fi

test-type-damage: test-type-damage-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_repaint.py --xres 1920 --yres 1200 --reps 2 \
		--only type --assert-max-frac type=$(TYPE_DAMAGE_MAX) --iso $(ISO) --disk $(DISK) \
		--json $(BUILD)/type_damage.json

ci-boot: test-type-damage
