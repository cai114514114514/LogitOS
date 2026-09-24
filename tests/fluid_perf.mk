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

# The kernel's drawing contexts (c/lib/gfx/include/gfx_ctx.h): one per band
# worker plus the compositor's own. A ring-3 build keeps the default of 1.
CFLAGS += -DGFX_NCTX=4

# ---- P1: the band splitter, host side ----
.PHONY: test-wm-bands test-wm-bands-negctl
test-wm-bands-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DWM_BANDS_NEGCTL_IGNORE_GLASS -Ic/kernel/gui \
		-o $(BUILD)/wm_bands_negctl tests/unit/wm_bands_test.c
	@if ./$(BUILD)/wm_bands_negctl > $(BUILD)/wm_bands_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the splitter passes while cutting through glass"; exit 1; \
	elif grep -q '^FAIL NO-CUT' $(BUILD)/wm_bands_negctl.log; then \
		echo "negative control ok: cutting through glass breaks NO-CUT"; \
	else cat $(BUILD)/wm_bands_negctl.log; echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON"; exit 1; fi
test-wm-bands: test-wm-bands-negctl
	@$(CC) -O2 -Wall -Wextra -Ic/kernel/gui -o $(BUILD)/wm_bands_test tests/unit/wm_bands_test.c
	@./$(BUILD)/wm_bands_test
ci-host: test-wm-bands

# ---- P1: band-parallel composition, on the machine ----
# test-wm-par boots a -DWM_PAR_VERIFY kernel: every rectangle composited in
# parallel is re-rendered serially and compared pixel for pixel, across a drag,
# a dock sweep, typing and a theme flip (which repaints the whole screen). It
# must see parallel frames, worker-rendered bands, and ZERO differing pixels.
# The control builds the same kernel with the splitter allowed to cut through
# glass (-DWM_BANDS_NEGCTL_IGNORE_GLASS) and must see differing pixels -- the
# proof the comparison can catch a wrong band. It runs first.
ifeq ($(PARVERIFY),1)
CFLAGS += -DWM_PAR_VERIFY
endif
ifeq ($(PARCUTGLASS),1)
CFLAGS += -DWM_BANDS_NEGCTL_IGNORE_GLASS
endif
WM_PAR_BUILD := $(BUILD)-parverify
WM_PAR_CTL_BUILD := $(BUILD)-parcutglass
WM_PAR_WORK := drag,dock,type,theme

.PHONY: test-wm-par test-wm-par-negctl
test-wm-par-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(WM_PAR_CTL_BUILD) PARVERIFY=1 PARCUTGLASS=1 \
		$(WM_PAR_CTL_BUILD)/logit.iso > $(BUILD)/wm_par_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_repaint.py --xres 1920 --yres 1200 --reps 1 \
		--only $(WM_PAR_WORK) --assert-par control \
		--iso $(WM_PAR_CTL_BUILD)/logit.iso --disk $(DISK) \
		> $(BUILD)/wm_par_ctl.log 2>&1 || rc=$$?; \
		grep -E "par |FAIL" $(BUILD)/wm_par_ctl.log; \
		test $$rc -eq 0 && echo "negative control ok: a band cut through glass is caught"

test-wm-par: test-wm-bands test-wm-par-negctl $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(WM_PAR_BUILD) PARVERIFY=1 \
		$(WM_PAR_BUILD)/logit.iso > $(BUILD)/wm_par_build.log 2>&1
	@python3 tests/qmp/qmp_repaint.py --xres 1920 --yres 1200 --reps 1 \
		--only $(WM_PAR_WORK) --assert-par verify \
		--iso $(WM_PAR_BUILD)/logit.iso --disk $(DISK) --json $(BUILD)/wm_par.json

ci-boot: test-wm-par
