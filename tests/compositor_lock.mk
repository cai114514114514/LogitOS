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



