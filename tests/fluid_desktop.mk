# tests/fluid_desktop.mk -- the desktop contract of the "Fluid" release
# (docs/superpowers/specs/2026-09-24-experience-release-design.md, P2-P5).

# ---- P2: key-up, and the Cmd+Tab switcher that needs it ----
# The control kernel's keyboard driver drops modifier releases
# (-DKEYUP_NEGCTL) -- the machine as it was before key-up -- so a held Cmd+Tab
# can never commit, and scene 1 must fail at exactly that check.
ifeq ($(KEYUPNEGCTL),1)
CFLAGS += -DKEYUP_NEGCTL
endif
SWITCHER_CTL_BUILD := $(BUILD)-keyupnegctl

.PHONY: test-switcher test-switcher-negctl
test-switcher-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(SWITCHER_CTL_BUILD) KEYUPNEGCTL=1 \
		$(SWITCHER_CTL_BUILD)/logit.iso > $(BUILD)/switcher_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_switcher.py --iso $(SWITCHER_CTL_BUILD)/logit.iso \
		--disk $(DISK) --negctl > $(BUILD)/switcher_ctl.log 2>&1 || rc=$$?; \
		cat $(BUILD)/switcher_ctl.log; \
		if [ $$rc -eq 1 ] && grep -q 'FAIL  releasing Cmd committed the choice' $(BUILD)/switcher_ctl.log; then \
			echo "negative control ok: without the release edge the switcher cannot commit"; \
		else echo "NEGATIVE CONTROL FAILED"; exit 1; fi

test-switcher: test-switcher-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_switcher.py --iso $(ISO) --disk $(DISK)

ci-boot: test-switcher

# ---- P3: snapping and the close animation ----
# test-snap: the guest's own [wm] snap lines must give exactly the half / zoom
# frame for each zone, and leaving a half must restore the size. The control
# kernel (-DWM_SNAP_NEGCTL) shows the preview but does not apply it on release.
# test-close-anim: a launch + Cmd+W round trip must leave the desktop
# pixel-identical and the fade must draw >= 3 positions. The control kernel
# (-DWM_CLOSE_DAMAGE_LIE) skips the fade's final damage report and must leave
# residue.
ifeq ($(SNAPNEGCTL),1)
CFLAGS += -DWM_SNAP_NEGCTL
endif
ifeq ($(CLOSELIE),1)
CFLAGS += -DWM_CLOSE_DAMAGE_LIE
endif
SNAP_CTL_BUILD  := $(BUILD)-snapnegctl
CLOSE_CTL_BUILD := $(BUILD)-closelie

.PHONY: test-snap test-snap-negctl test-close-anim test-close-anim-negctl
test-snap-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(SNAP_CTL_BUILD) SNAPNEGCTL=1 \
		$(SNAP_CTL_BUILD)/logit.iso > $(BUILD)/snap_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_snapclose.py --iso $(SNAP_CTL_BUILD)/logit.iso --disk $(DISK) \
		--only snap > $(BUILD)/snap_ctl.log 2>&1 || rc=$$?; cat $(BUILD)/snap_ctl.log; \
		if [ $$rc -eq 1 ] && grep -q 'FAIL  dragging to the left edge snapped it' $(BUILD)/snap_ctl.log; then \
			echo "negative control ok: a release that does not apply the zone fails the frame checks"; \
		else echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-snap: test-snap-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_snapclose.py --iso $(ISO) --disk $(DISK) --only snap

test-close-anim-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(CLOSE_CTL_BUILD) CLOSELIE=1 \
		$(CLOSE_CTL_BUILD)/logit.iso > $(BUILD)/close_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_snapclose.py --iso $(CLOSE_CTL_BUILD)/logit.iso --disk $(DISK) \
		--only close > $(BUILD)/close_ctl.log 2>&1 || rc=$$?; cat $(BUILD)/close_ctl.log; \
		if [ $$rc -eq 1 ] && grep -q 'FAIL  the desktop is pixel-identical after the close' $(BUILD)/close_ctl.log; then \
			echo "negative control ok: an unreported last frame leaves residue the round trip sees"; \
		else echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-close-anim: test-close-anim-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_snapclose.py --iso $(ISO) --disk $(DISK) --only close

ci-boot: test-snap test-close-anim

# ---- P4: Spotlight ----
# /sbin/spotlight.aex: a panel app (WINS_SET_PANEL) the window manager starts
# on the first Cmd+Space. In /sbin so it is not in the dock. Linked against
# mini-libc (opendir/stdio for the file index), like Terminal.
SPOTLIGHT_DIR := c/apps/gui/spotlight
ifeq ($(SPOTLIGHTNOFILES),1)
SPOTLIGHT_CFLAGS := -DSPOTLIGHT_NEGCTL_NO_FILES
endif
$(BUILD)/spotlight/spotlight.o: $(SPOTLIGHT_DIR)/spotlight.c $(SPOTLIGHT_DIR)/sl_rank.h c/apps/gui/aui.h
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) $(SPOTLIGHT_CFLAGS) -c $< -o $@
$(BUILD)/spotlight.elf: $(BUILD)/spotlight/spotlight.o $(BUILD)/apps/crt0.o $(BUILD)/apps/aui.o \
  $(GFX_OBJ) $(LIBC_OBJS)
	$(LD) -nostdlib -e _start -Ttext=0x4E000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(BUILD)/spotlight/spotlight.o $(BUILD)/apps/aui.o $(GFX_OBJ) \
	    $(LIBC_OBJS) --end-group
$(BUILD)/spotlight.aex: $(BUILD)/spotlight.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Spotlight - S 90 90 110
FS_FILES += $(BUILD)/spotlight.aex:/sbin/spotlight.aex
$(DISK): $(BUILD)/spotlight.aex tests/fluid_desktop.mk

# The ranking, on the host: orderings a person would call obviously right. The
# control ranks by "appears anywhere" alone and must misorder at least one.
.PHONY: test-spotlight-rank test-spotlight-rank-negctl
test-spotlight-rank-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DSL_RANK_NEGCTL -I$(SPOTLIGHT_DIR) -o $(BUILD)/sl_rank_negctl tests/unit/sl_rank_test.c
	@if ./$(BUILD)/sl_rank_negctl > $(BUILD)/sl_rank_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: substring-only ranking passes"; exit 1; \
	else echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/sl_rank_negctl.log) orderings wrong without the prefix/word tiers"; fi
test-spotlight-rank: test-spotlight-rank-negctl
	@$(CC) -O2 -Wall -Wextra -I$(SPOTLIGHT_DIR) -o $(BUILD)/sl_rank_test tests/unit/sl_rank_test.c
	@./$(BUILD)/sl_rank_test
ci-host: test-spotlight-rank

# test-spotlight (QMP): Cmd+Space starts the panel; "term" launches Terminal;
# a second Cmd+Space shows the same process; a file from the index opens in
# TextEdit; a query nothing matches asks the assistant; Escape hides it; and the
# typed-into panel is pixel-identical to a full composite of it (watched failing
# at 43 px with aui's old px/4 text padding). The control is a whole disk whose
# Spotlight has no file source (-DSPOTLIGHT_NEGCTL_NO_FILES).
SPOTLIGHT_CTL_BUILD := $(BUILD)-spotnofiles
.PHONY: test-spotlight test-spotlight-negctl
test-spotlight-negctl:
	@$(MAKE) --no-print-directory BUILD=$(SPOTLIGHT_CTL_BUILD) SPOTLIGHTNOFILES=1 \
		$(SPOTLIGHT_CTL_BUILD)/logit.iso $(SPOTLIGHT_CTL_BUILD)/disk.img > $(BUILD)/spotlight_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_spotlight.py --iso $(SPOTLIGHT_CTL_BUILD)/logit.iso \
		--disk $(SPOTLIGHT_CTL_BUILD)/disk.img --negctl > $(BUILD)/spotlight_ctl.log 2>&1 || rc=$$?; \
		cat $(BUILD)/spotlight_ctl.log; \
		if [ $$rc -eq 1 ] && grep -q 'FAIL  "shaping" + Enter opened /docs/shaping.txt' $(BUILD)/spotlight_ctl.log; then \
			echo "negative control ok: without the file source the file case fails"; \
		else echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-spotlight: test-spotlight-rank test-spotlight-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_spotlight.py --iso $(ISO) --disk $(DISK)
ci-boot: test-spotlight
