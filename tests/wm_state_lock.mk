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



