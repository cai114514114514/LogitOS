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

.PHONY: test-audit test-audit-list test-negctl-drift bkl-shared ci ci-boot ci-all ci-here test-simd-os

# cpu_simd_selftest() was written, documented down to its call order, and never
# called from anywhere -- the same shape of dead test the audit above looks
# for, one level down in C rather than in the Makefile. It now runs from
# kernel_main on every boot; this is the target that turns its output into an
# assertion instead of a line nobody reads.
#
# What it proves cannot be proved host-side: that the XMM register file
# survives a real timer interrupt (isr_common's FXSAVE/FXRSTOR) and that the
# selected AES backend agrees with the portable one while interrupts are
# landing inside the instruction stream.
test-simd-os: $(ISO) $(DISK)
	@out=$$(timeout 90 qemu-system-x86_64 -cdrom $(ISO) \
	    -drive file=$(DISK),format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
	    -vga none -device virtio-gpu-pci -m 512M -display none -serial stdio -snapshot 2>&1); \
	echo "$$out" | grep -F '[cpu] simd selftest:' || true; \
	if echo "$$out" | grep -q '^CPU_SIMD_SELFTEST_OK'; then \
	    echo "test-simd-os: PASS"; \
	else \
	    echo "test-simd-os: FAIL -- no CPU_SIMD_SELFTEST_OK on serial"; \
	    echo "$$out" | grep -iE 'simd|xmm' || echo "  (the selftest produced no output at all)"; \
	    exit 1; \
	fi
	@echo "test-simd-os: ok"

# The audit gates. It exits non-zero on any finding, so it can be a
# prerequisite of anything that wants the guarantee.
test-audit:
	@python3 tools/audit_tests.py

test-audit-list:
	@python3 tools/audit_tests.py --list

# The negative-control drift gate. A control is a COPY of the thing it controls
# and nothing in make says "identical except for the -D", so controls drift --
# six had, and all six were found by tripping over them rather than by any
# check. This one is at 0 in both its directions today, which is why it gates
# HARD while test-audit is still advisory in ci: there is nothing here to grant
# a grace period to, and a check that starts clean and is allowed to go dirty
# was never a gate.
test-negctl-drift:
	@python3 tools/negctl_drift.py

# Not a gate: an inventory. Every file-scope mutable static in the subsystems
# the BKL still protects, for the removal work. Prints "at least N" and means it.
bkl-shared:
	@python3 tools/bkl_shared.py

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
