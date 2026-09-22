# Product builds share one jobserver. Tests keep their existing scheduling: many
# of their harnesses still write common paths, so `all test-foo` is also excluded.
BUILD_PRODUCT_GOALS := all run $(ISO) $(DISK)
JOBS ?= $(shell python3 -c 'import os; print(min(12, os.cpu_count() or 1))')

# GNU Make only exposed nontrivial options in parse-time MAKEFLAGS from 4.4.
# Before that, `make -j1` looks exactly like plain `make`; adding -j here silently
# overrides the user's serial request (observed with Apple's 3.81). shell-export
# was introduced in the same 4.4 release and is a feature-based version floor.
# Older make retains its original scheduling, including any explicit -j option.
# https://lists.gnu.org/archive/html/info-gnu/2022-10/msg00008.html
ifneq ($(filter shell-export,$(.FEATURES)),)
ifeq ($(MAKELEVEL),0)
ifeq ($(filter-out $(BUILD_PRODUCT_GOALS),$(MAKECMDGOALS)),)
# Respect both direct -j/-j1 and an inherited jobserver, including a jobserver
# supplied by a caller whose MAKELEVEL is zero. Replacing either creates a new
# pool and can multiply the number of jobs across recursive builds.
ifeq ($(filter -j% --jobs% --jobserver%,$(MAKEFLAGS)),)
BUILD_AUTO_JOBS := $(strip $(JOBS))
ifeq ($(BUILD_AUTO_JOBS),)
$(error JOBS must be a positive integer)
endif
MAKEFLAGS += -j$(BUILD_AUTO_JOBS)
endif
endif
endif
endif
