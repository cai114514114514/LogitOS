# --- test-sweep: run EVERY test target in the tree ---------------------------
#
# `make test-audit` reports 352 test- targets that no suite reaches, after
# already excluding the ones deliberately out of CI (benchmarks, negative
# controls run by their positive counterpart, manual drivers). That number is
# the real story of this tree's test coverage: `make test` reaches 22 targets
# out of 522.
#
# This does not redefine what `make test` is for -- that is a decision, and it
# belongs to whoever owns the CI budget. It makes "run everything" ONE COMMAND,
# so the 352 stop being unreachable in practice:
#
#   make test-sweep              everything: host targets in parallel, device
#                                targets one at a time, then every failure
#                                re-run alone before it is believed
#   make test-sweep-confirm      just the re-run pass, over an existing result
#                                file (for a sweep that predates the rule)
#   make test-sweep-host         the 353 host targets only -- minutes, not hours
#
# SWEEP_JOBS bounds the parallel phase; SWEEP_TMO is the per-target timeout.
# Results land in $(BUILD)/sweep/all.res as "STATUS<TAB>seconds<TAB>target",
# with each target's output in $(BUILD)/sweep/sweeplogs/.
#
# It is NOT wired into `test` or `ci`: the device half boots QEMU 169 times and
# takes hours, and a gate nobody can afford to run is a gate nobody runs.
.PHONY: test-sweep test-sweep-confirm test-sweep-host

SWEEP_JOBS ?= 6
SWEEP_TMO  ?= 900
SWEEP_OUT  ?= $(BUILD)/sweep/all.res

test-sweep: $(ISO) $(DISK)
	@mkdir -p $(BUILD)/sweep
	@bash tests/boot/sweep-targets.sh $(SWEEP_OUT) $(SWEEP_JOBS) $(SWEEP_TMO)

test-sweep-confirm:
	@bash tests/boot/sweep-confirm.sh $(SWEEP_OUT) $(SWEEP_TMO)

# Host only. The device targets are what make a full sweep an afternoon; this
# half is the one worth running after an ordinary change.
test-sweep-host:
	@mkdir -p $(BUILD)/sweep
	@SWEEP_HOST_ONLY=1 bash tests/boot/sweep-targets.sh $(SWEEP_OUT) $(SWEEP_JOBS) $(SWEEP_TMO)
