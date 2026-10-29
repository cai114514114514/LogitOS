# tests/compositor_lock.mk -- process exits must not repaint an unchanged desk.
#
# The measurement behind this gate is guest-only: it opens the real Terminal,
# runs six real /bin/true processes, and brackets them with wm.c's cumulative
# compositor and graphics-mutex counters.  Host elapsed time is deliberately
# absent; parallel TCG instances make it noise, and the guest already measures
# the exact critical section.
#
# The negative build restores wm_app_exit()'s old unconditional full-repaint
# request.  It runs FIRST and the positive target names it as a prerequisite,
# so the control cannot become a CI decoration that is never executed.  Both
# kernels boot the same immutable disk and run the same six commands.

ifeq ($(WMFULLNEGCTL),1)
CFLAGS += -DWM_SPURIOUS_FULL_NEGCTL
endif

COMPOSITOR_LOCK_DISK ?= $(DISK)
COMPOSITOR_LOCK_CTL_BUILD := $(BUILD)-wm-exit-negctl
COMPOSITOR_LOCK_DRIVER := tests/qmp/qmp_compositor_lock.py

.PHONY: test-compositor-lock test-compositor-lock-negctl

test-compositor-lock-negctl:
	@echo '--- compositor-lock NEGATIVE CONTROL: every process exit requests a full frame ---'
	@$(MAKE) --no-print-directory BUILD=$(COMPOSITOR_LOCK_CTL_BUILD) WMFULLNEGCTL=1 \
		$(COMPOSITOR_LOCK_CTL_BUILD)/logit.iso >/dev/null
	@test -f "$(COMPOSITOR_LOCK_DISK)" || { \
		echo 'FAIL: COMPOSITOR_LOCK_DISK does not exist; build a disk or name an immutable one'; exit 1; }
	@python3 $(COMPOSITOR_LOCK_DRIVER) --mode control --commands 6 \
		--iso $(COMPOSITOR_LOCK_CTL_BUILD)/logit.iso --disk $(COMPOSITOR_LOCK_DISK) \
		--json $(BUILD)/compositor_lock_control.json

test-compositor-lock: test-compositor-lock-negctl $(ISO)
	@echo '--- compositor-lock FIXED: non-GUI exits leave the desktop alone ---'
	@python3 $(COMPOSITOR_LOCK_DRIVER) --mode fixed --commands 6 \
		--iso $(ISO) --disk $(COMPOSITOR_LOCK_DISK) \
		--control-json $(BUILD)/compositor_lock_control.json \
		--json $(BUILD)/compositor_lock_fixed.json
