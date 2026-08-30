# anim gates -- owned by the anim-clock wave-1 agent (2026-08-30).
#
# WHAT THIS FILE CLOSES. Until the animation clock landed, `animation` and
# `transition` were approximated to their static END-STATE: nothing drove
# frames, so a 2 s fade-in painted as its final frame from the first repaint
# (css_extra.c's walk_anim). The clock itself is the tick consumer on the
# page's ONE deadline queue (js_page.c: timers, rAF and now the animation
# tick share one monotonic clock and one sleep), the engine is js_anim.c
# part 2, the @keyframes/animation/transition capture is css_extra.c, and
# the per-tick overlay lands on the same cstyle fields the painter already
# reads (opacity, xraw[XR_TRANSFORM]) so browser_paint.c needed no change.
#
# TWO GATES, each with its control as a PREREQUISITE (never a ci- line
# sibling, which would satisfy the audit and run never):
#
#   test-anim-clock (ci-host)  the pure halves: css_interp.c's timing
#       functions (linear/ease*/cubic-bezier/steps) against reference
#       values, and the engine's animation/transition shorthand parsers.
#       Control (test-anim-clock-negctl): the same suite built with
#       -DCSS_ANIM_NEGCTL_EASE_LINEAR, where every easing collapses to
#       identity -- the values at 0 and 1 still agree, so a suite that
#       cannot fail mid-curve is exactly what the control catches.
#
#   test-anim-page (ci-boot)  the clock on the glass: screenshots at
#       t~=0.1/1.0/2.6 s of @keyframes opacity+transform pages and of a
#       class-change transition; the MID frame must differ from BOTH the
#       start render and the END-STATE render (the end-state is what the
#       pre-clock browser showed at every t). The in-run static control
#       (margin-left keyframes must not move; opacity:0 + a MISSING
#       keyframes name must stay visible) runs FIRST inside the driver and
#       kills the run before the positive assertions if it fails.
#       Control (test-anim-page-negctl): the same driver against a browser
#       built with -DLOGIT_ANIM_NO_CLOCK, which is byte-for-byte the
#       pre-clock behaviour -- the two mid!=end assertions must go RED
#       there, and the control also greps for the measurement lines so a
#       plumbing failure cannot masquerade as a passing control.
#
# COST, honestly: the guest control builds a second browser + disk and
# boots a second QEMU, so ci-boot pays two boots for this file. Same trade
# tests/bold.mk already records; if this ever moves to a slower tier, move
# BOTH halves of the pair, never the positive alone.
#
# WHAT STAYS REFUSED, stated here because a gate that cannot see the
# refusal is where it gets silently "fixed": the clock interpolates opacity
# and transform only (paint-time values; no relayout per frame for
# transform, one relayout per changed-opacity tick). Everything else in a
# @keyframes rule keeps the cascade base value, which is the same
# end-state-shaped answer the tree gave before. animation/transition are
# captured from the SHORTHAND only; longhand-only declarations do not
# animate. Per-keyframe easing and animation events are not implemented.

ifeq ($(ANIMNOCLK),1)
CFLAGS += -DLOGIT_ANIM_NO_CLOCK
# ...and the same define through the door that actually reaches this
# package's TU. js_anim.c compiles by the ring-3 jsobj rule with JS_CF,
# which is SIMPLY-EXPANDED at Makefile:888 and then frozen target-specific
# for every BROWSER_JS_OBJ at Makefile:1030 -- both long before this
# fragment is included (line ~5078), so the CFLAGS append above reaches
# only kernel objects (bold.mk's negctl gets away with exactly that
# because its face constant lives in kernel/gui). Without this line the
# "no-clock" control browser is secretly the clocked one: measured, not
# guessed -- the first control run crashed INSIDE ca_adopt, a function
# that cannot exist in the no-clock world.
$(BUILD)/jsobj/c/apps/browser/js_anim.o: JS_CF += -DLOGIT_ANIM_NO_CLOCK
endif

.PHONY: test-anim-clock test-anim-clock-negctl \
        test-anim-page test-anim-page-negctl

# The same flags tests/interp.mk gives js_anim.c on the host: js_anim.c is
# one TU whose WAAPI half needs QuickJS, so the engine half pays for the
# prelude's include paths too. -DCONFIG_BIGNUM is load-bearing, not
# cargo-cult (libbf's decimal path is compiled out without it while
# quickjs.c references it anyway).
ANIMCLK_CF   := -O1 -g -w -Ic/apps/browser -Ithird_party/quickjs \
                -Ithird_party/libm -DCONFIG_VERSION='"host"' -DCONFIG_BIGNUM
ANIMCLK_SRC  := c/apps/browser/css_interp.c c/apps/browser/js_anim.c $(QJS_SRC)

# --- host: the timing functions and the shorthand parsers -------------------
test-anim-clock: test-anim-clock-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(ANIMCLK_CF) -o $(BUILD)/anim_clock_test \
	    tests/fixtures/anim/check_anim_clock.c $(ANIMCLK_SRC) -lm -lpthread
	@$(BUILD)/anim_clock_test

test-anim-clock-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(ANIMCLK_CF) -DCSS_ANIM_NEGCTL_EASE_LINEAR \
	    -o $(BUILD)/anim_clock_negctl \
	    tests/fixtures/anim/check_anim_clock.c $(ANIMCLK_SRC) -lm -lpthread
	@if $(BUILD)/anim_clock_negctl > $(BUILD)/anim_clock_negctl.log 2>&1; then \
	    echo "CONTROL FAILED: the suite passed with every easing collapsed to"; \
	    echo "  identity, so it is not measuring the mid-curve values"; \
	    exit 1; fi; \
	if ! grep -q 'FAIL ease-in-out(0.25)' $(BUILD)/anim_clock_negctl.log; then \
	    echo "CONTROL FAILED: exited nonzero but not on an easing value -- a"; \
	    echo "  crash also exits nonzero and would read as a passing control"; \
	    tail -5 $(BUILD)/anim_clock_negctl.log; exit 1; fi; \
	echo "control ok: identity-easing reddens the mid-curve assertions:"; \
	grep 'FAIL' $(BUILD)/anim_clock_negctl.log | head -3

ci-host: test-anim-clock

# --- guest: the clock on the glass -----------------------------------------
test-anim-page: test-anim-page-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_anim_page.py $(ISO) $(DISK)

test-anim-page-negctl:
	$(MAKE) BUILD=$(BUILD)/neganim ANIMNOCLK=1 $(BUILD)/neganim/disk.img \
	    $(BUILD)/neganim/logit.iso
	@echo "--- negative control: the same gate against a browser with no clock ---"
	@if python3 tests/qmp/qmp_anim_page.py $(BUILD)/neganim/logit.iso \
	        $(BUILD)/neganim/disk.img \
	        > $(BUILD)/neganim/ctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: end-state rendered at every t and the"; \
	    echo "  mid!=end assertions did not notice"; \
	    tail -20 $(BUILD)/neganim/ctl.log; exit 1; fi; \
	grep -q '^keyframes: mid-vs-start' $(BUILD)/neganim/ctl.log || { \
	    echo "NEGATIVE CONTROL FAILED: no measurement line -- the page did not"; \
	    echo "  render, which is a plumbing failure, not the control"; \
	    tail -20 $(BUILD)/neganim/ctl.log; exit 1; }; \
	grep -q '^FAIL @keyframes: the MID frame differs from the END-STATE' \
	    $(BUILD)/neganim/ctl.log || { \
	    echo "NEGATIVE CONTROL FAILED: the driver exited nonzero but not on the"; \
	    echo "  end-state assertion -- a plumbing error also exits 1 and would"; \
	    echo "  read as a passing control"; \
	    tail -20 $(BUILD)/neganim/ctl.log; exit 1; }; \
	echo "control ok: with the clock compiled out the mid frame IS the end"; \
	echo "  state, and the gate says so:"; \
	grep -E '^(keyframes:|transition:|FAIL @keyframes: the MID)' \
	    $(BUILD)/neganim/ctl.log | head -4

ci-boot: test-anim-page
