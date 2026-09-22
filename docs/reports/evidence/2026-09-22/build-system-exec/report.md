# Exec host gate portability

Changed only `tests/exec.mk` and five files under `tests/unit/exechost/`.
Final source hashes: `final-sources.json`.

The original failure is retained at `../build-system-core/exec-gates.log`:
Darwin has no `mremap` declaration/constants, and `aex.c` cannot find `kheap.h`.
Read-only dependency inspection also found the missing SHA-512 implementation
used by Ed25519, located in `c/crypto/hash/sha384.c`.

## Changes

- Add the real malloc/free host heap shim and SHA-512 translation unit.
- Preserve Linux mremap and add Darwin mmap/copy/unmap for single-owner frames.
  This does not claim to implement shared frame aliases. The older PIE wrapper
  keeps its own adapter; its `-Werror` compilation still succeeds.
- Compile Darwin probes and tests as x86_64, shrinking Mach-O PAGEZERO to leave
  the low guest window available. The native 16 KiB page model is not usable
  for independently protected adjacent 4 KiB guest pages.
- Build ordinary, ASan/UBSan, and control binaries before capability checks.
  An absent runtime or explicit MMU capability refusal yields a named SKIP;
  missing files, unexpected probe errors and crashes remain failures.
- Probe actual fixed mapping, independent read/write permissions and RWX
  mapping support without executing generated instructions. The sanitizer gate
  has a probe built with the same sanitizer flags.
- Keep the overflow and W^X controls as prerequisites of all three positive
  host entries. Their checks now reject invocation failures as control success.
  Review correction: the first runner still accepted any nonzero/non-2 fuzz
  result. It now rejects everything except SIGSEGV/SIGBUS, and even those are
  explicitly only an OBSERVATION, not a passing fuzz control: `exec_fuzz.c`
  emits no seed-loaded or named-case marker before completion. The identified
  overflow unit case remains the actual control. No fuzz fixture was changed.

## Executed validation

`make BUILD=build-system-exec` with the nine explicit host binary targets
compiled and linked the ordinary/fuzz, three controls, two sanitizer binaries,
and ordinary/sanitizer probes. The actual architecture is Mach-O x86_64.
Log: `compile.log` (successful compiler commands are silent).

`make BUILD=build-quality-glass test-exec test-exec-fuzz test-exec-asan
test-exec-bases test-mk-wired` completed with exit 0. Final log:
`final-gates.log`.

- Runtime: **SKIP**, including controls. Executing the compiled probe raises
  Darwin `Bad CPU type in executable`; x86_64 execution is unavailable here.
  No loader assertions or runtime negative controls were counted as passing.
- Link bases: **PASS**, 88 AEX files (the final rerun includes the newly built
  legacy `exec_v1.aex` fixture; the first pass covered 87).
- Make wiring: **PASS**, 386 fragments, 385 reachable, one declared wrapper.
- Native probe: **SKIP with exit 77**, measured page size 16384; see
  `native-probe.log`. It stops before attempting mapping operations.
- Existing PIE wrapper: **PASS** for x86_64 compilation with `-Werror`.
- Whitespace and Python syntax checks: **PASS**.
- Review follow-up: eight synthetic return-code classification checks reject
  exit 0/1/2/127 and SIGABRT/SIGKILL; SIGSEGV/SIGBUS produce only an explicitly
  unproven OBSERVATION (`control-classification.log`). These check runner logic,
  not the loader or an actual mutant crash. The real runner still reports the
  unavailable runtime as SKIP (`review-skip.log`).

Two dependency controls were run by changing only scratch compiler argv/header
copies, not production sources. Omitting `sha384.c` fails linking on `_sha512`,
`_sha512_init`, `_sha512_update`, and `_sha512_final` (`missing-sha512.log`).
Omitting the host heap header fails compiling `aex.c` with `kheap.h file not
found` (`missing-heap.log`). Restoring dependencies links all nine targets.

The normal make entry rebuilt `preview.aex` from an already-modified Preview
source according to its existing prerequisites. No disk or ISO was built,
packed or modified by this task, and no VM was launched.

## Remaining boundaries

Linux or an executable Darwin x86_64 runtime must run the full loader and its
runtime controls. Their execution is not proven by successful cross-compilation.
The mutant fuzzer additionally needs a reliable case marker before its crash
can count as control evidence; an expected signal alone does not provide one.
The production loader/VM and boot harness were not modified. In particular,
the known old `run-exec-test.sh` shell/operator issues are still outside this
patch; use individually captured guest commands for current guest evidence.
