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
.PHONY: test-sweep test-sweep-confirm test-sweep-host test-sweep-resume

SWEEP_JOBS ?= 6
# THE BUDGET MUST EXCEED THE LONGEST LEGITIMATE HARNESS, and 900 did not.
#
# It is computable, not a guess: a boot harness bounds each boot itself and runs
# several, so its worst case is boots x per-boot bound.
#
#   run-settings-test.sh   6 boots x 150 s = 900 s   -- exactly the old budget
#   run-statmeta-test.sh   4 boots x 220 s = 880 s   -- plus overhead, over it
#
# Both timed out in the first full sweep, and neither was hanging: each boot
# stopped at its own limit, and the sum reached the sweep's. A perfectly healthy
# slow run collides by construction, and the result reads as TIMEOUT -- the one
# verdict that looks most like a real hang.
#
# 1800 gives the worst known harness 2x headroom. A target that genuinely hangs
# still gets caught; it just costs twice as long to say so, which is the right
# trade when three of the seven timeouts in a 522-target sweep were this.
SWEEP_TMO  ?= 1800
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
	@bad=$$(grep -cvE "^(PASS|NOTARGET|NEEDSARGS|AGGREGATE)	" $(SWEEP_OUT) || true); \
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

# Pick up an interrupted sweep: run only what has no result yet, then confirm.
# An afternoon is long enough for something to interrupt it, and re-running 440
# passing targets to reach the 80 that never ran is an hour spent learning
# nothing. NO VERDICT HERE -- sweep-resume.sh chains into sweep-confirm.sh, and
# the resumed run's judgement belongs to test-sweep-confirm over the same file.
test-sweep-resume:
	@bash tests/boot/sweep-resume.sh $(SWEEP_OUT) $(SWEEP_TMO)

# Host only. The device targets are what make a full sweep an afternoon; this
# half is the one worth running after an ordinary change.
test-sweep-host:
	@mkdir -p $(BUILD)/sweep
	@SWEEP_HOST_ONLY=1 bash tests/boot/sweep-targets.sh $(SWEEP_OUT) $(SWEEP_JOBS) $(SWEEP_TMO)
	$(SWEEP_VERDICT)
