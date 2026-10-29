# --- the clipboard and notifications ----------------------------------------
#
# In its own .mk rather than in the Makefile, for the reason tests/mem.mk and
# tests/loader.mk both give: several lines share this tree and a whole-file
# Makefile overwrite from a concurrent line has silently deleted other people's
# targets more than once. The only tokens this needs in the main Makefile are
# its `-include` and `clip notify` in the CLI list.
#
#   make test-clip           the whole clipboard gate: host sweep + the
#                            two-process test on the machine
#   make test-clip-host      the host sweep alone (seconds, no emulator)
#   make test-clip-negctl    THE NEGATIVE CONTROL. A kernel whose paste
#                            truncates at the byte asked for, which is the
#                            obvious implementation and the wrong one. Required
#                            to FAIL.
#   make test-notify         the notification overlay: it appears, it does not
#                            steal focus, several stack and queue, a click
#                            dismisses one, and nothing it drew is left behind.
#   make test-notify-negctl  THE OTHER NEGATIVE CONTROL. A kernel whose overlay
#                            reports half the damage it causes. Required to FAIL.
#   make test-notify-cost    what an appearance costs vs. an animated entrance,
#                            from the compositor's own counters.

# --- build knobs, here rather than in the Makefile ---------------------------
# CFLAGS is a simply-expanded variable but recipes are expanded when they RUN,
# so appending from an -include'd fragment at the bottom of the Makefile still
# reaches every compile. That is what keeps this whole feature's footprint in
# the shared Makefile down to two tokens.
#
#   make CLIPNAIVE=1   paste truncates at the byte asked for (splits characters)
#   make NOTIFYLIE=1   the overlay reports half the damage it causes
#   make NOTIFYANIM=1  the 240 ms slide + fade entrance, for the cost comparison
ifeq ($(CLIPNAIVE),1)
CFLAGS += -DCLIP_NAIVE_TRUNC
endif
ifeq ($(NOTIFYLIE),1)
CFLAGS += -DNOTIFY_DAMAGE_LIE
endif
ifeq ($(NOTIFYANIM),1)
CFLAGS += -DNOTIFY_ANIMATE
endif





# --- notifications, in pixels ------------------------------------------------
# A visual feature. A claim about it that is not a screenshot is not a claim, so
# the driver photographs every state it asserts and leaves the frames behind.
# The control below is a PREREQUISITE of this target, not a ci- line:
# tools/audit_tests.py's NOT_CI drops every `test-*-negctl` from CI on the
# assumption the positive runs it, and this is the line that makes that true.
test-notify: test-notify-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_notify.py --iso $(ISO) --disk $(DISK)

# The overlay reports only half the column it draws into. Every pixel of the
# other half then survives the notification, and the round-trip check must fail.
test-notify-negctl:
	$(MAKE) BUILD=$(BUILD)/negnotify NOTIFYLIE=1 \
	        $(BUILD)/negnotify/logit.iso $(BUILD)/negnotify/disk.img
	@echo "--- negative control: an overlay that under-reports its damage ---"
	@if python3 tests/qmp/qmp_notify.py --iso $(BUILD)/negnotify/logit.iso \
	        --disk $(BUILD)/negnotify/disk.img --only stale; then \
	    echo "NEGATIVE CONTROL FAILED: stale pixels went unnoticed"; exit 1; \
	else \
	    echo "negative control ok: half-reported damage leaves pixels nothing repaints"; \
	fi


# --- navigator.clipboard, on the device -------------------------------------
# tests/qmp/qmp_clipboard_js.py boots the real machine, launches the Browser
# from the Dock, loads a page over HTTP and CLICKS IT over QMP, then reads the
# clipboard back FROM ANOTHER PROCESS with /bin/clip. Two processes is the
# whole point and it is the same argument c/kernel/gui/clipboard.c makes for
# /bin/clip existing at all: the claim is that the clipboard outlives the
# process that filled it, and one process cannot test that.
#
# THIS TARGET DID NOT EXIST WHEN THE DRIVER LANDED. The driver was written,
# run by hand, reported "28/28 checks pass" -- and nothing in the tree named
# it, so it was one of CLAUDE.md's 21 DEAD harnesses on the day it arrived.
# A gate nobody runs is a gate that rots, silently: rule 4, and it takes three
# lines to not be an instance of it.
#
# What it covers that no host test can: the ACTIVATION GATE. writeText writes
# the clipboard EVERY PROCESS on this machine reads, so a page must not be
# able to overwrite what the user copied out of Terminal from a timer, in the
# background. The page here calls writeText at load with no click ever having
# happened and must be refused -- and then /bin/clip must show the refusal
# left the real clipboard untouched, because a rejected promise and a
# clipboard that changed anyway are identical from inside the page. The other
# direction is covered too: a writeText DEFERRED out of the click handler must
# still work, because every real copy button that awaits anything calls it
# after its handler returned, and a gate cleared at end-of-dispatch would
# refuse them all while still passing a synchronous-only test.
test-clip-js: $(ISO) $(DISK)
	python3 tests/qmp/qmp_clipboard_js.py --iso $(ISO) --disk $(DISK)

.PHONY: \
        test-clip-js test-notify test-notify-negctl test-notify-cost

# ci-boot takes the JS half of the clipboard: the ACTIVATION GATE is a
# property of the machine (writeText must not fire from a background timer),
# and no host twin can prove a click-gate that only exists between a page and
# the WM. It sat unwired (reachable by nobody) with its sibling above -- the
# debt this package retires.
ci-boot: test-clip-js
