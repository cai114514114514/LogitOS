# --- test-dock-map: the Dock's slot order, read from the machine that has it -
#
# In its own fragment for the reason tests/webapi_platform.mk gives at its top:
# several agents edit the top-level Makefile at once, and a fragment is the only
# way to add targets without a commit sweeping up somebody else's half-finished
# work.
#
#   make test-dock-map      boot the shipped disk, then a disk with one app
#                           fewer, and require the helper to follow
#
# WHAT IS BEING GATED. wm.c's dock_publish() prints the app registry the Dock is
# laid out from -- `[wm] dock N apps: 0=clock.aex,shown ...` -- and
# tests/qmp/qmp_ui.py reads it (parse_dock / dock_slot / dock_icon_of). Before
# that line existed the same fact was spelled in ten places in tests/qmp/ as
# hand-maintained constants, which is CLAUDE.md rule 3 at ten doors; NAPPS had
# already gone stale once, the day settings.aex was packed.
#
# THE CONTROL IS THE SECOND DISK AND IT IS THE POINT. A helper that reads its
# own constant returns the same slot no matter what booted, and every driver
# stays green while clicking the wrong app -- the shape
# tools/check-test-liveness.py exists to find. So this gate boots a SECOND image
# built from the same tree with one app removed from the pack list, and requires
# the helper to answer a different slot and a different pixel for the same app
# NAME, with no Python edited; then it clicks the FIRST machine's coordinate on
# the SECOND machine and requires that nothing launches. See the driver's header.
#
# The removed app is derived (`filter-out`) rather than restated: a second
# hand-written app list inside the gate against the door it is closing would be
# funny in the wrong way. It filters out `textedit`, not `widgets` -- Task E
# (2026-09-02) dropped Widgets from $(APPS) entirely (see the Makefile), so
# `$(filter-out widgets,$(APPS))` would silently have become a no-op, running
# the SAME disk twice and passing for the wrong reason (rule 5: a control that
# cannot be watched failing is worse than none). textedit has no other coupling
# in the Makefile (unlike monitor, which MONITOR_AEX substitutes for) and is
# not the app either half of this gate's OWN assertions launch, so removing it
# changes only the thing this gate exists to measure: the count.
#
# TASK E ALSO REQUIRES A SECOND, DIFFERENT CONTROL: that the Dock's published
# COUNT can be independently caught lying, not just that a hand-written Python
# constant can. That is test-dock-map-negctl, below -- a real build-time defect
# (dock_publish() under-reporting nreg by one) rather than a stale-vs-fresh disk
# diff, which is why it is its own target and its own negctl kernel rather than
# a third arm of the two-disk comparison above.
.PHONY: test-dock-map test-dock-map-negctl

# The control is a PREREQUISITE of the positive gate -- CLAUDE.md's fix for the
# 61 stranded controls it found, "test-X: test-X-negctl". Naming it on a
# ci-boot: line of its own instead would satisfy tools/audit_tests.py's wiring
# check and still run it never, which is worse because it looks fixed: it runs
# first, so a broken apparatus is reported before the positive run can be
# believed at all.
test-dock-map: test-dock-map-negctl $(ISO) $(DISK)
	@$(MAKE) --no-print-directory DISK=$(DOCK_ALT_DISK) APPS="$(DOCK_ALT_APPS)" $(DOCK_ALT_DISK)
	@python3 tests/qmp/qmp_dock_map.py $(ISO) $(DISK) $(DOCK_ALT_DISK); \
	 rc=$$?; rm -f $(DOCK_ALT_DISK); exit $$rc

# ci-boot: it boots QEMU (test-dock-map twice for the two-disk comparison,
# test-dock-map-negctl once more for the lying build).
ci-boot: test-dock-map

DOCK_ALT_DISK := $(BUILD)/disk_dockalt.img
DOCK_ALT_APPS := $(filter-out textedit,$(APPS))

# The alt image is built through the ORDINARY $(DISK) rule with APPS overridden,
# not by a rule of its own: a second recipe that packs a root would be a second
# statement of what the root contains, and it would drift. It is removed again
# afterwards (pass or fail) because it is a 512 MiB artifact that rebuilds in
# under a second once the .aex files exist -- see the "110 scratch trees and
# 22 GB" commit for why an extra half-gigabyte per build tree is not free.

# --- test-dock-map-negctl: can a LYING published count be caught? -----------
#
# A SECOND ISO, into a sub-BUILD, because the control is a build-time define
# (DOCKNEGCTL=1 -> -DDOCK_NEGCTL_PUBLISH_STALE, see the Makefile's CFLAGS
# block). The disk is NOT rebuilt -- ring 3 and the pack list are identical in
# both arms, and the defect is entirely inside wm.c's dock_publish(), so the
# control shares $(DISK) and costs one kernel + one ISO, exactly the pattern
# tests/net.mk's test-netlock-negctl already uses.
#
# THE DEFECT: dock_publish() prints nreg-1 apps instead of nreg, and the line
# is still perfectly well-formed -- self-consistent, parseable, no red flag in
# its own shape. scan_apps() is untouched, so the REAL dock still draws and
# hit-tests every app it found; only the report is short by one. A harness
# that trusts the header number and clicks every slot IT names is fooled the
# same way NAPPS used to fool every driver before dock_publish() existed.
# qmp_dock_negctl.py is the harness built to NOT be fooled: it computes where a
# tile one slot past the published count WOULD be if the true count were
# published+1, clicks there, and requires the guest to report a real launch --
# proving there is a real, unpublished tile, i.e. proving the count the
# positive gate trusts can in fact be a lie and this apparatus catches it.
#
# WATCHED, NOT ASSUMED: qmp_dock_negctl.py's own exit code is the pass/fail of
# THIS make target. If the click at the hypothesised extra slot finds nothing,
# that does not mean the machine is honest -- it means the negative control
# itself is broken (wrong build, macro not threaded, arithmetic wrong) and this
# target fails LOUDLY rather than let a mute control stand in for one that
# works, per CLAUDE.md rule 5.
# $(DISK) is an explicit prerequisite HERE, not left to test-dock-map's own
# prerequisite list to provide -- this recipe references it directly (the
# negctl ISO is booted paired with the ORDINARY disk) and make does not
# promise to walk test-dock-map's prerequisites in the order they are
# written, so a bare reliance on "it'll already exist by the time we get
# here" would be exactly the kind of apparatus fragility rule 1 warns about.
test-dock-map-negctl: $(DISK)
	@$(MAKE) --no-print-directory $(BUILD)/dock-negctl/logit.iso \
		BUILD=$(BUILD)/dock-negctl DOCKNEGCTL=1
	@python3 tests/qmp/qmp_dock_negctl.py $(BUILD)/dock-negctl/logit.iso $(DISK)
