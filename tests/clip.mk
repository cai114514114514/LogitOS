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

# --- host: the clipboard store, every boundary, no emulator ------------------
# clipboard.c has no dependency on the process table (syscall.c looks the pid up
# and passes it in) and none on the window manager, so it compiles against the
# host libc with six stubs. That is not an accident of the code -- it is why the
# pid is a parameter.
test-clip-host:
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -o $(BUILD)/clipboard_test \
	      tests/unit/clipboard_test.c c/kernel/gui/clipboard.c $(HOST_INCDIRS)
	@$(BUILD)/clipboard_test

# The same sweep against a kernel that cuts a paste at the byte it was asked
# for. Every other assertion in clipboard_test still passes; the cut sweep does
# not, which is the point -- the control has to fail for ONE reason.
test-clip-host-negctl:
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -DCLIP_NAIVE_TRUNC \
	      -o $(BUILD)/clipboard_negctl \
	      tests/unit/clipboard_test.c c/kernel/gui/clipboard.c $(HOST_INCDIRS)
	@if $(BUILD)/clipboard_negctl > $(BUILD)/clipboard_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: a naive truncation passed the boundary sweep"; \
	    exit 1; \
	else \
	    echo "negative control ok: a naive truncation splits characters and the sweep sees it"; \
	    grep -c 'SPLIT a character' $(BUILD)/clipboard_negctl.log \
	      | sed 's/^/     cuts that split a character: /'; \
	fi

# --- on the machine: two processes, one clipboard ----------------------------
test-clip: test-clip-host test-clip-host-negctl $(ISO) $(DISK)
	bash tests/boot/run-clip-test.sh $(ISO) $(DISK)

# THE NEGATIVE CONTROL, on the machine. Same disk, same script, a kernel built
# with -DCLIP_NAIVE_TRUNC. `clip trunc` must report a split character and the
# harness must fail. An assertion nobody has watched fail is not a known-failing
# assertion, so this target exists to be run, not to be described.
test-clip-negctl:
	$(MAKE) BUILD=$(BUILD)/negclip CLIPNAIVE=1 \
	        $(BUILD)/negclip/logit.iso $(BUILD)/negclip/disk.img
	@echo "--- negative control: the SAME test against a naive truncation ---"
	@if bash tests/boot/run-clip-test.sh $(BUILD)/negclip/logit.iso $(BUILD)/negclip/disk.img; then \
	    echo "NEGATIVE CONTROL FAILED: a paste that splits characters passed the test"; exit 1; \
	else \
	    echo "negative control ok: the boundary assertion fails without the boundary rule"; \
	fi

# --- notifications, in pixels ------------------------------------------------
# A visual feature. A claim about it that is not a screenshot is not a claim, so
# the driver photographs every state it asserts and leaves the frames behind.
test-notify: $(ISO) $(DISK)
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

# --- what it costs -----------------------------------------------------------
# The instruction was to measure the animation before building it, so both were
# built and both were measured. This target raises three notifications on an
# idle desktop and reads the compositor's own counters
# (`[wm] perf ... composites= cpx= fpx=`) before and after.
#
# It measures against a CONTROL -- the same three shell commands with `true`
# instead of `notify` -- because fork+execve is itself worth a full-screen frame
# on this compositor, and the first version of this measurement was about to
# credit three of those to the notification.
#
# MEASURED 2026-08-08, 1920x1200 @ scale 150, TCG, three notifications raised
# and left to expire. Figures are load minus control:
#
#                      composites   px recomposited   full-screen frames
#   appear/disappear        +5           945,760              0
#   240 ms slide + fade    +26         5,176,224              0
#
# 5.2x the frames and 5.5x the pixels for two thirds of a second of decoration,
# which is why the shipped build does not animate. See the long comment above
# notify_compose() in c/kernel/gui/notify.c for why the shape of that answer
# does not depend on the host.
test-notify-cost: $(ISO) $(DISK)
	@echo "=== shipped: a notification appears ==="
	python3 tests/qmp/qmp_notify.py --iso $(ISO) --disk $(DISK) --only cost
	$(MAKE) BUILD=$(BUILD)/animnotify NOTIFYANIM=1 \
	        $(BUILD)/animnotify/logit.iso $(BUILD)/animnotify/disk.img
	@echo "=== the losing variant: a 240 ms slide + fade ==="
	python3 tests/qmp/qmp_notify.py --iso $(BUILD)/animnotify/logit.iso \
	        --disk $(BUILD)/animnotify/disk.img --only cost

.PHONY: test-clip test-clip-host test-clip-host-negctl test-clip-negctl \
        test-notify test-notify-negctl test-notify-cost
