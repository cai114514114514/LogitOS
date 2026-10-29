# The WM state mutex must not cover compositor pixels.  QEMU host time is not
# measured: the kernel reports wall/on-CPU hold time and exact causal counts.
# The negative build restores the old lock boundary and runs first; the positive
# target is not allowed to exist without watching that control go red.

ifeq ($(WMLOCKNEGCTL),1)
CFLAGS += -DWM_STATE_RENDER_NEGCTL
endif

WM_STATE_LOCK_DISK ?= $(DISK)
WM_STATE_LOCK_CTL_BUILD := $(BUILD)-wm-state-negctl
WM_STATE_LOCK_DRIVER := tests/qmp/qmp_wm_state_lock.py

.PHONY: test-wm-state-lock test-wm-state-lock-negctl

test-wm-state-lock-negctl:
	@echo '--- wm-state-lock NEGATIVE CONTROL: compositor pixels hold wm_lock ---'
	@$(MAKE) --no-print-directory BUILD=$(WM_STATE_LOCK_CTL_BUILD) WMLOCKNEGCTL=1 \
		$(WM_STATE_LOCK_CTL_BUILD)/logit.iso >/dev/null
	@test -f "$(WM_STATE_LOCK_DISK)" || { \
		echo 'FAIL: WM_STATE_LOCK_DISK does not exist; build a disk or name an immutable one'; exit 1; }
	@python3 $(WM_STATE_LOCK_DRIVER) --mode control --commands 6 \
		--iso $(WM_STATE_LOCK_CTL_BUILD)/logit.iso --disk $(WM_STATE_LOCK_DISK) \
		--json $(BUILD)/wm_state_lock_control.json

test-wm-state-lock: test-wm-state-lock-negctl $(ISO)
	@echo '--- wm-state-lock FIXED: compositor pixels run outside wm_lock ---'
	@python3 $(WM_STATE_LOCK_DRIVER) --mode fixed --commands 6 \
		--iso $(ISO) --disk $(WM_STATE_LOCK_DISK) \
		--control-json $(BUILD)/wm_state_lock_control.json \
		--json $(BUILD)/wm_state_lock_fixed.json
