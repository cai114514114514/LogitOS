# tests/ascross.mk -- AetherScript compiler crosscheck (unit DX), RETIRED.
#
# test-as-crosscheck and test-as-crosscheck-negctl stood here until the A2
# engine left (2026-09-16, 4fcc6e6cb / d006fdcc2), and were missed by the change
# that tombstoned their siblings in the Makefile. The gate compiled every .as in
# the tree with BOTH compilers -- the C one and the AetherScript-written
# fsroot/as/lib/asc.as -- and diffed the .la bytes; its control perturbed OP_RET
# in a scratch copy of asc.as and required a BYTECODE-DIFFERS divergence.
#
# Both inputs are gone: asc.as was deleted with the engine and `asc` no longer
# emits .la bytecode. From that day both targets failed at their first step
# ("FAIL: no asc.as in fsroot/as/lib", "sed: .../asc.as: No such file") before
# any comparison ran -- red that measured nothing, which a host sweep on the
# macOS dev host reported on 2026-10-26. There is one compiler now, so "the two
# compilers check each other" has no second party. What the gate protected, the
# emitted code being right, is held by the typed fleet in tests/astyped.mk and
# test-as-examples' frozen native outputs (see the A2 tombstones in the
# Makefile). tests/unit/run-as-crosscheck.sh was deleted in the same change.
#
# The fragment stays, empty of targets, so the Makefile's `-include` of it keeps
# pointing at the reason rather than at nothing.
