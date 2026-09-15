# tests/repaint.mk -- what a REPAINT costs, and whether an ANIMATION stops.
#
# tests/qmp/qmp_repaint.py is the instrument this desktop's performance work was
# done with, and until this fragment existed NO MAKE TARGET RAN IT. Three other
# drivers (qmp_notify, qmp_window, qmp_damage, qmp_css_repaint, qmp_flash)
# import it as a LIBRARY, so it was reachable, exercised in pieces, and never
# run as the thing it is -- which is the shape of rot rule 4 in CLAUDE.md is
# about. The table at the top of that file ("drag a large window 94.0 ms") came
# out of it, by hand, once.
#
# Own fragment rather than lines in the root Makefile, for the reason every
# other fragment in this tree exists: a whole-file Makefile overwrite from a
# concurrent line deletes targets written straight into it. Both targets here
# carry a recipe, so tools/audit_tests.py classifies them and tools/ci.sh finds
# them without anyone maintaining a list.
#
# ---------------------------------------------------------------------------
# THE TRAP THAT ATE AN AFTERNOON, and it is why these recipes look the way they
# do. qmp_repaint.py's settle_pointer() confirms the pointer against the
# guest's own `[wm] ptr X Y` line; with no serial log configured it falls back
# to LOCATING THE ARROW IN A SCREENDUMP, and this machine puts the pointer on
# the display's HARDWARE CURSOR PLANE. The arrow is therefore not in the
# composite, the drag never grabs anything, and the profile that comes back is
# four halted cores -- reported as a result. The driver constructs
# Session(sock, serial=serial) itself, so this is a property to preserve rather
# than a flag to pass, and it is written down here because the next person to
# copy these recipes into a new driver will not otherwise know.
#
# Every number is TCG on a shared host. Read a DELTA between two runs on the
# same machine; the absolute milliseconds are not portable and are not meant to
# be. Do NOT quote `bench-gfx-frame` at any of this -- it builds the gallery
# through aui.c, nm finds zero browser_paint symbols in it, and two runs of the
# same binary have read 19,871 and 12,707 microseconds.

.PHONY: test-repaint test-open-anim test-anim test-anim-negctl test-fb-scale-bl test-fb-scale-bl-negctl

# The animated-window scaler keeps the former four-weight formula in the host
# gate as a pixel oracle. The production loop may use fewer multiplies, but it
# may not move a sample centre or round between axes. The mutation does exactly
# that tempting intermediate round and must be caught by an actual pixel.
FB_SCALE_BL_SRC := tests/unit/fb_scale_bl_bench.c c/kernel/gui/fb/fb.c c/lib/gfx/adapters/openlogit_display.c
FB_SCALE_BL_INC := $(KGUI_INC) -Ic/drivers/virtio $(KMM_INC) -Ic/lib/text $(GFX_INC)

$(BUILD)/fb_scale_bl_bench: $(FB_SCALE_BL_SRC) tests/repaint.mk
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -o $@ $(FB_SCALE_BL_SRC) $(FB_SCALE_BL_INC)

$(BUILD)/fb_scale_bl_negctl: $(FB_SCALE_BL_SRC) tests/repaint.mk
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -DFB_SCALE_BL_NEGCTL_AXIS_ROUND \
	    -o $@ $(FB_SCALE_BL_SRC) $(FB_SCALE_BL_INC)

test-fb-scale-bl-negctl: $(BUILD)/fb_scale_bl_negctl
	@rc=0; (cd $(BUILD) && ./fb_scale_bl_negctl) > $(BUILD)/fb_scale_bl_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'bilinear mismatch 17x13' $(BUILD)/fb_scale_bl_negctl.log

test-fb-scale-bl: test-fb-scale-bl-negctl $(BUILD)/fb_scale_bl_bench
	@cd $(BUILD) && ./fb_scale_bl_bench

ci-host: test-fb-scale-bl

# ---------------------------------------------------------------------------
# THE TABLE. Not a pass/fail -- a measurement, printed. It is a make target so
# that "run the repaint table" is one command with the right ISO and disk on
# it, and so that a before/after pair is reproducible by somebody who did not
# write it.
#
# REPS defaults to 3 inside the driver because a single sample from a host that
# other agents are running QEMU on is how a line reports a regression that was
# its own neighbour's build.
REPAINT_XRES ?= 1920
REPAINT_YRES ?= 1200
REPAINT_REPS ?= 3
REPAINT_JSON ?= $(BUILD)/repaint.json

test-repaint: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_repaint.py --xres $(REPAINT_XRES) --yres $(REPAINT_YRES) \
	    --reps $(REPAINT_REPS) --iso $(ISO) --json $(REPAINT_JSON) $(REPAINT_ONLY)

# Regression gate for a flight whose PIT-driven progress once remained p=0
# indefinitely. It checks the real guest serial stream and gives the animation
# a full second to settle, independent of how many intermediate TCG frames fit.
test-open-anim: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_repaint.py --xres 1280 --yres 800 --reps 1 \
	    --only open --assert-open --iso $(ISO) --disk $(DISK) \
	    --json $(BUILD)/open_anim.json

# ---------------------------------------------------------------------------
# THE ANIMATION GATE. Three assertions; the second and third are what make it a
# control rather than a thermometer. assert_anim() in the driver states each
# one and prints which is failing.
#
#   1. POSITIVE -- a Settings toggle flipped ANIM_FLIPS times must produce at
#      least 4 composites per flip. AUI_T_BASE is 180 ms and the toolkit derives
#      a ~21 ms cadence from that window's canvas, so the real number is nearer
#      9; four is the floor at which motion is motion rather than a jump cut,
#      which is the number the vocabulary was designed around.
#
#   2. NEGATIVE -- and it MUST BE WATCHED FAILING, which is what test-anim-negctl
#      below does. -DAUI_ANIM_OFF makes aui_anim() return its target instantly.
#      Every widget still calls it, every path above and below is unchanged, the
#      picture at rest is identical -- the value simply arrives in one frame, so
#      nothing is live, no deadline is registered, and the same interaction
#      produces one composite instead of nine. IF THE OFF BUILD ALSO READS NINE,
#      the harness is counting the compositor's own idle repaints and assertion
#      1 means nothing. That is not hypothetical here: CLAUDE.md records
#      test-ime-os passing green while the feature was unusable, by construction.
#
#   3. STOP -- 1.2 s of nothing, starting well after the last flip must have
#      landed, must produce <= 3 composites. This is the only assertion that can
#      see the failure the whole design exists to avoid: a slot that never
#      latches keeps registering deadlines forever. MORE COMPOSITES LOOK LIKE
#      MORE ANIMATION, so assertion 1 would pass while the machine burned a core
#      -- and re-introducing that is the worst possible outcome of this work,
#      because deleting it is what took 3,283,157 syscalls a boot down to 1,234.
#
# It also prints the SAME interaction with the window pushed down over the dock.
# That is not asserted, deliberately: it is a property of where the user left
# the window, not of the code. It is published because the compositor grows any
# damage touching a glass panel to the whole panel (dmg_expand), so ~77,000
# glass pixels at ~207 ns each is about +16 ms PER FRAME -- roughly 1.8x for the
# identical widget -- and nothing inside aui can see it.
#
# THE NEGATIVE CONTROL RUNS FIRST and hands its number to the positive run,
# because a control that has to be remembered is a control that is skipped. The
# flag has to be on the make that BUILDS THE DISK, not on the one that runs the
# harness -- tests/desktop.mk's greeter control documents the same trap, and it
# is the reason this is a recipe that re-invokes make rather than a two-step in
# a README.
test-anim: test-anim-negctl
	@echo "--- animation gate: the real build ---"
	@$(MAKE) --no-print-directory $(DISK) >/dev/null
	@python3 tests/qmp/qmp_repaint.py --iso $(ISO) --only anim --reps 1 \
	    --assert --expect-off $$(cat $(BUILD)/anim_negctl.count)

# aui.o is shared by every GUI app, so the OFF build has to invalidate it and
# put it back afterwards -- the rm is what makes the flag take, because make
# cannot see a -D that is not in a prerequisite. The flag is passed by
# OVERRIDING UCFLAGS from this recipe rather than by adding a hook variable to
# the root Makefile, so this fragment needs no edit in a file three other lines
# of work are also editing today.
test-anim-negctl: $(ISO) $(DISK)
	@echo "--- animation NEGATIVE CONTROL: aui_anim() returns its target instantly ---"
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/apps/aui.o
	@$(MAKE) --no-print-directory $(DISK) UCFLAGS="$(UCFLAGS) -DAUI_ANIM_OFF" >/dev/null
	@python3 tests/qmp/qmp_repaint.py --iso $(ISO) --only anim --reps 1 \
	    --json $(BUILD)/anim_negctl.json > $(BUILD)/anim_negctl.log 2>&1 || true
	@python3 -c "import json,sys; \
d=json.load(open('$(BUILD)/anim_negctl.json')).get('workloads',{}); \
a=d.get('anim'); \
sys.stdout.write(str(a['composites'][0]) if a else '0'); \
sys.stderr.write('' if a else 'CONTROL DID NOT AIM: the OFF run never produced an anim row.\n')" \
	    > $(BUILD)/anim_negctl.count
	@echo "    OFF build composites: $$(cat $(BUILD)/anim_negctl.count)"
	@if [ "$$(cat $(BUILD)/anim_negctl.count)" = "0" ]; then \
	    echo "CONTROL FAILED: the -DAUI_ANIM_OFF run produced no measurement at"; \
	    echo "                all, so it cannot be compared against. A control"; \
	    echo "                that could not aim is not a control that read zero."; \
	    tail -20 $(BUILD)/anim_negctl.log; \
	    rm -f $(BUILD)/apps/aui.o; \
	    $(MAKE) --no-print-directory $(DISK) >/dev/null 2>&1; exit 1; \
	 fi
	@# Put the REAL toolkit back in the image. Without this the positive run
	@# above measures the accept-nothing build and reports it as the result.
	@rm -f $(BUILD)/apps/aui.o
	@$(MAKE) --no-print-directory $(DISK) >/dev/null

# test-anim names its own control as a prerequisite, which is the ONE line that
# stops it joining the 61 stranded controls tests/audit-stranded.baseline
# records. Naming it on a ci-boot: line instead would satisfy the audit and
# still run it never, which is worse because it looks fixed.
ci-boot: test-anim
