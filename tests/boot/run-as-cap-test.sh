#!/usr/bin/env bash
# M28's headline gate, on the real machine: a script that was NOT granted CAP_FS
# provably cannot read /etc, and one that was, can.
#
# =============================================================================
# HISTORY. Written 2026-08-14 while the tree could not build an ISO (the
# page-cache line's fault.c was mid-flight), committed then as a deliverable
# ahead of its own runnability. First actually RUN 2026-08-15 -- and its first
# run earned its keep twice over: (1) it caught its own spawn line spelling
# `as script.as --scope P` (flag AFTER the script), which as.c parsed as
# script args and ran UNNARROWED -- run 2 read /etc identically to run 1;
# (2) chasing that turned the silence into a hard refusal in as.c (a trailing
# --scope anywhere in argv now refuses to run). The reason this file exists at
# all stands: the 4152 host checks in tests/unit/as_cap_test.c all run against
# a held set the test itself installed through as_caps_set(); only this boot
# proves the set a script runs under is the one the KERNEL granted
# (SYS_CAP_QUERY -> install_kernel_grant() in c/apps/as/cli/main.c).
# =============================================================================
#
# WHY TWO RUNS OF THE SAME SCRIPT. A single run showing "denied" is equally
# consistent with the capability check working and with /etc/logit.conf simply
# not existing, or with open() being broken, or with the script never having
# run at all. The evidence is the DIFFERENCE: identical source, identical
# machine, two grants, two answers. The unscoped run must read /etc; the scoped
# run must be refused there and still succeed inside its own subtree -- which
# also rules out "the scoped process just can't open anything".
#
# The scoped child is spawned through ash.as, which is the point of doing it
# this way rather than with a kernel-side fixture: the narrowing travels the
# real path a program would use (SYS_CAP_SPAWN, ceiling-checked by
# proc_cap_subset), not a back door built for the test.

# A3 correction: the old VM --scope launch and mixed serial grep are retired.
# All three grants now run the same installed native artifact. The shared
# oracle requires each child's complete output and its actual exit status.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 "$ROOT/tests/boot/run-as-cap-native.py" "$@"
