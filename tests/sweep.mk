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

# THE GATE IS HERE, NOT IN THE SCRIPTS, AND UNTIL NOW THERE WAS NO GATE AT ALL.
#
# `make test-sweep` exited 0 no matter what the sweep found. sweep-confirm.sh
# ends with `grep -v -E "^PASS\t" | sort | sed ...`, and a pipeline's status is
# its LAST command's, so sed's 0 was the answer -- a sweep of 525 targets that
# could not fail. tools/audit_tests.py caught it under MUTE, which is exactly
# the shape it was written to find, and its opening line is the argument: a test
# that cannot fail is worse than no test, because it is counted.
#
# The scripts stay reporters -- they print a verdict per target and write the
# result file, which is what they are for. The TARGET is the gate, and it judges
# the artifact rather than a exit status threaded through two levels of shell.
# NEEDSARGS and NOTARGET are not failures: one was never callable bare, the
# other is a build-system finding already reported as itself.
define SWEEP_VERDICT
	@bad=$$(grep -cvE "^(PASS|NOTARGET|NEEDSARGS)	" $(SWEEP_OUT) || true); \
	if [ "$$bad" != "0" ]; then \
	    echo "test-sweep: $$bad target(s) not passing -- see $(SWEEP_OUT)"; \
	    exit 1; \
	fi; \
	echo "test-sweep: $$(grep -c . $(SWEEP_OUT)) target(s), all accounted for"
endef

test-sweep: $(ISO) $(DISK)
	@mkdir -p $(BUILD)/sweep
	@bash tests/boot/sweep-targets.sh $(SWEEP_OUT) $(SWEEP_JOBS) $(SWEEP_TMO)
	$(SWEEP_VERDICT)

test-sweep-confirm:
	@bash tests/boot/sweep-confirm.sh $(SWEEP_OUT) $(SWEEP_TMO)
	$(SWEEP_VERDICT)

# Host only. The device targets are what make a full sweep an afternoon; this
# half is the one worth running after an ordinary change.
test-sweep-host:
	@mkdir -p $(BUILD)/sweep
	@SWEEP_HOST_ONLY=1 bash tests/boot/sweep-targets.sh $(SWEEP_OUT) $(SWEEP_JOBS) $(SWEEP_TMO)
	$(SWEEP_VERDICT)
