---
name: logitos-write-a-test
description: Add a host or guest test to LogitOS with its negative control and Make wiring. Use whenever a change needs evidence, a test fragment is created, or a test target is renamed.
---

# Write a test with its negative control

1. **Choose the observation**, not the convenient check: a host unit test for logic you can feed inputs
   (`tests/unit/`), a guest test for syscalls, scheduling, interrupts, devices or packaged apps
   (`tests/guest/`, harness in `tests/boot/`), a QMP driver for desktop behaviour (`tests/qmp/`).
   Assert what a caller observes; print the input and the expected value on every failure.
2. **Write the negative control first and watch it fail.** A control introduces one plausible defect
   through one small, named switch (for example `ABORT_NOTIFY_EARLY` in
   `c/apps/gui/browser/js/net/abort.c`) and must fail on exactly the assertion it names. A compile error,
   a crash or an unrelated failure does not count. Share sources, fixtures and packaging with the
   positive case.
3. **Wire it into Make** in a fragment under `tests/`, included from the root `Makefile`:
   declare targets `.PHONY`, make the control a prerequisite of the positive target, and add the
   positive target to `ci-host` (host) or `ci-boot` (guest).
   ```make
   .PHONY: test-thing
   test-thing: test-thing-negctl $(THING_DIR)/current
   ci-host: test-thing
   ```
4. **Check the wiring:** `make test-mk-wired`, `make test-audit`, `make test-negctl-drift`.
5. `tests/` is ignored by `.gitignore`: a new test file only enters a commit with `git add -f`.
   Never delete or skip a test target because nothing seems to call it; such targets are manual
   acceptance gates.
6. Guest durations use guest time (`SYS_MONOTONIC_MS`); QEMU evidence is reported as QEMU evidence.

Details: `docs/contributing/writing-tests.md`, `docs/architecture/testing.md`, `AGENTS.md` §4–§5.
