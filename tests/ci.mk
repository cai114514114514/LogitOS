# tests/ci.mk -- the thing this repository did not have.
#
# Until now there was no CI here at all, and it was the structural cause of a
# day's worth of breakage: 14 of 30 commits did not build from a clean clone of
# themselves, `test-durability` (five real boots proving the disk keeps data)
# belonged to no suite and had apparently never been run, and `qmp_term.py`
# computed a verdict, printed PASS or FAIL, and exited 0 either way.
#
# Two targets, because there are two distinct failures to catch:
#
#   test-audit   finds tests that CANNOT FAIL -- a harness no target runs, a
#                harness that prints FAIL and exits 0, a target no aggregate
#                reaches. A test that cannot fail is worse than no test,
#                because it is counted. This is a lint: seconds, no build.
#
#   ci           builds from a CLEAN CLONE OF HEAD and runs the suites. The
#                clean clone is the whole point; every one of those 14 commits
#                built fine in the working tree.
#
# Own fragment rather than lines in the Makefile for the reason the other
# fragments exist: a whole-file Makefile overwrite from a concurrent line
# deletes targets written straight into it, which is how two of these
# fragments came to exist in the first place.

.PHONY: test-audit test-audit-list ci ci-boot ci-all ci-here

# The audit gates. It exits non-zero on any finding, so it can be a
# prerequisite of anything that wants the guarantee.
test-audit:
	@python3 tools/audit_tests.py

test-audit-list:
	@python3 tools/audit_tests.py --list

# ci: audit + clean-clone build + every host suite the Makefile declares.
# The suite list is DERIVED from the Makefile (tools/audit_tests.py
# --suites=host), not hand-written -- a hand-written list is exactly what
# rotted into 217 unwired targets.
ci:
	@bash tools/ci.sh

# ci-boot: the QEMU boot suites as well. Long. This is the one that would have
# run test-durability.
ci-boot:
	@bash tools/ci.sh --boot-only

ci-all:
	@bash tools/ci.sh --boot

# ci-here: same suites, working tree, no clone. Faster, and strictly weaker --
# it cannot see a missing-from-git file, which is the failure ci exists for.
ci-here:
	@bash tools/ci.sh --here
