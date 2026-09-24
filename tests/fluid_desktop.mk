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
