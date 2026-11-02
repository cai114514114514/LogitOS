# Making your first change

This walkthrough adds a regression case for `cut`, follows its host and guest builds, exercises the existing negative controls, and prepares a commit containing the two edited files. Run the commands from the repository root after setting up the [build environment](building.md). Record the results observed in your environment.

## Read the rules and check ownership

Read `CLAUDE.md`, `AGENTS.md`, `CONTRIBUTING.md`, and `TRANSPARENCY.md`. They describe the project, shared workspace rules, contribution policy, and evidence required for capability statements.

Inspect the files you plan to edit:

```sh
git status --short
git diff -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
```

Check their modification times and coordinate with anyone working on them. `AGENTS.md` requires respecting active ownership and preserving other contributors’ changes. Choose an unused build directory for generated outputs.

Follow the authorship and model eligibility policy in `CONTRIBUTING.md`, including its named exceptions. Contributions must identify the model that performed the work and describe its role.

Comments should explain decisions and failure modes that a future reader would struggle to infer. `AGENTS.md` gives examples of the expected standard.

## Trace the behavior

The example adds coverage for a field selection containing reordered and repeated positions.

Read these sources together:

| File | What to follow |
|---|---|
| `c/apps/coreutils/text/cut.c` | Selection parsing and field output |
| `tests/textutils/cases.txt` | Tagged host comparison cases |
| `tests/textutils/guest_cases.txt` | Commands executed inside LogitOS |
| `tests/textutils/run_host.py` | Reference selection and output comparison |
| `tests/textutils.mk` | Host builds, guest packages, and test dependencies |

In `c/apps/coreutils/text/cut.c`, `parse_list()` marks selected positions in an array. Repeating a position sets the same entry again. `cut_line()` walks the input fields in order and emits each selected field once. This representation makes selection order and repetition produce the same result as the corresponding set of positions.

For `-f3,1,1`, fields one and three appear in input order. The first line of `tests/textutils/fixture/words.txt` is `apple 3 red`, so the expected first output line is `apple red`.

`tests/textutils.mk` builds the tool source against both the host C library and LogitOS’s C library. The host suite compares the project tools with the host reference tools. The guest suite compares the packaged programs running inside LogitOS with the project tools built for the host. Together, these comparisons exercise the selection logic and its integration with the LogitOS runtime.

## Establish a baseline

The commands below use `build-first-change` as the build directory. Choose another name if that directory is already in use.

```sh
make BUILD=build-first-change test-textutils-host
python3 tests/textutils/run_host.py build-first-change/textutils-host --only cut
```

The first command builds the host tools and runs their aggregate gate. Its prerequisites include the tools compiled with mutations and the check that their mistakes are detected.

The second command focuses on cases classified as testing `cut`. In `tests/textutils/run_host.py`, `--only` filters by the first command word that matches an executable in the supplied directory. Building the complete tool sets first gives the focused run the same executable inventory as the aggregate gate.

The runner executes each comparison in fresh copies of the fixtures, sets `LC_ALL=C`, and compares standard output and exit status. Read the summary and investigate existing failures before attributing a result to your change.

Pass `BUILD` as a make command-line assignment. `Makefile` defines `BUILD := build`; `AGENTS.md` records that an environment-prefix assignment loses to this definition.

## Add the regression cases

Beside the existing field selection cases in `tests/textutils/cases.txt`, add:

```text
# Reordered and repeated selections emit each field once, in input order.
posix  cut -d ' ' -f3,1,1 words.txt
```

The host runner reads the tag and command, then obtains the expected result by running the host reference tool.

Add the corresponding untagged command to `tests/textutils/guest_cases.txt`:

```text
cut -d ' ' -f3,1,1 words.txt
```

Keep one command per line in the guest file and preserve its final newline. The loops in `tests/boot/run-textutils-test.sh` read each line as a command. The tagged host file has its own parser, which handles comment lines.

The selection behavior is already implemented in `c/apps/coreutils/text/cut.c`; these additions extend its coverage.

## Watch the control fail

Run the new case through the existing mutated host build:

```sh
python3 tests/textutils/run_host.py build-first-change/textutils-host-negctl --only cut
```

Defining `TEXTUTIL_NEGCTL` in `c/apps/coreutils/text/cut.c` removes the delimiters between selected output fields. Expect the new case to appear in a `FAIL [posix:cut]` report with differing output. For the first fixture line, the mutation produces `applered` and the reference produces `apple red`. The runner returns a nonzero status when comparisons fail.

Then run the standard build:

```sh
python3 tests/textutils/run_host.py build-first-change/textutils-host --only cut
```

Record the observed mismatch and the result from the standard build. The mutation checks whether the comparison detects missing separators. The new selection list adds coverage for field ordering and duplicate positions.

The aggregate host gate checks the controls too. In `tests/textutils.mk`, `test-textutils-host-negctl` is a prerequisite of `test-textutils-host`. Its recipe requires a failure report for every tool listed as carrying a mutation.

## Build and exercise the guest

Build the boot image and application disk:

```sh
make BUILD=build-first-change \
    build-first-change/logit.iso \
    build-first-change/disk.img
```

`Makefile` makes the ISO the dependency of the default `all` target. The development disk depends on application packages. `tests/textutils.mk` adds the text tool packages and fixture mappings to that disk’s dependency graph. Naming both artifacts exercises the kernel and userland build paths.

Run the integrated gate and the fragment wiring check:

```sh
make BUILD=build-first-change test-textutils
make BUILD=build-first-change test-mk-wired
```

`test-textutils` depends on the host suite and guest negative control. Its positive guest recipe invokes `tests/boot/run-textutils-test.sh`, which:

1. Copies the development disk.
2. Runs the guest command list against the project tools built for the host to create expected outputs.
3. Boots QEMU and executes the same commands inside LogitOS.
4. Checks the serial completion marker and compares files extracted from the copied disk byte for byte.

Each output file receives a trailing marker so that a command producing empty output still leaves an extractable file.

Inspect `textutils_negctl.log` and `textutils_guest_negctl.log` in the chosen build directory. These destinations are defined in `tests/textutils.mk`. For the guest control, inspect the new command’s output diff and confirm that the harness’s completion check succeeded.

`test-mk-wired` runs `tools/mk_wired.py`, which audits test fragment reachability through literal Makefile includes. Changes adding a test fragment should connect it to this graph. `AGENTS.md` also requires making a negative control a prerequisite of its positive gate.

## Check the command interactively

Start QEMU with the development disk:

```sh
make BUILD=build-first-change run-dev
```

Inside LogitOS, run:

```sh
cut -d ' ' -f3,1,1 /tu/words.txt
```

The fixture mapping in `tests/textutils.mk` supplies this guest path. Check that the first output line is `apple red`.

Earlier guidance associated `make run` with the development disk. The current `Makefile` defaults `run` to the release disk and provides `run-dev` to select the development disk used here.

## Review and prepare the commit

Review the intended changes and the surrounding working tree:

```sh
git diff --check -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
git diff -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
git status --short
```

Check that both cases express the same behavior and that the comment explains the property being protected. Account for every changed line in the two files.

Stage and inspect the intended paths:

```sh
git add -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
git diff --cached -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
```

Create a commit naming those paths:

```sh
git commit -- tests/textutils/cases.txt tests/textutils/guest_cases.txt
```

A suitable subject is “tests: cover cut field ordering and duplicate selections”. In the body, explain the behavior being protected and record:

- Exact validation commands, host environment, and observed results.
- The output mismatch observed with the negative control.
- Failed, skipped, and unrun checks.
- The writing model and its role, with the `Co-Authored-By:` trailer and session link described in `CONTRIBUTING.md`.

For new source files, follow the notice requirements in `LICENSING.md`. For imported material, record provenance and redistribution terms in `THIRD_PARTY.md` and preserve the required notices. `CONTRIBUTING.md` requires updating `TRANSPARENCY.md` when authorship, provenance, supported environments, or public capability statements change.

## Verify the committed revision

After committing, run:

```sh
make verify-commit
```

`tests/fullsystem.mk` delegates to `tools/verify-commit.sh`. By default, the script clones the repository into a temporary directory, checks out the committed revision, builds the ISO and disk image, and invokes `test-fullsystem`.

The clean clone checks whether the committed files supply the dependencies needed by the build and integration test. Report its result alongside the focused regression results.

## Limitations

- The host runner reports missing GNU reference tools as skips and returns success when its failure count is zero. Record skip counts when assessing coverage.
- The guest harness compares output files and checks completion; individual command exit statuses are outside the comparison.
- The guest negative-control wrapper accepts any nonzero harness exit. A boot failure can satisfy that wrapper, so its log must show completed commands and output differences caused by the mutations.
- Focused host comparisons classify cases using the executables present in the supplied directory. Use the complete tool sets built by the aggregate host gate.

## Source map

| File | What it contains |
|---|---|
| `CLAUDE.md` | Project map and dated development findings |
| `AGENTS.md` | Ownership, comments, controls, and completion rules |
| `CONTRIBUTING.md` | Authorship, provenance, validation, and contribution policy |
| `TRANSPARENCY.md` | Evidence and capability statement rules |
| `LICENSING.md` | Licensing boundaries and source notice requirements |
| `THIRD_PARTY.md` | Imported material provenance and redistribution records |
| `Makefile` | Default build, images, QEMU entry points, and test includes |
| `c/apps/coreutils/text/cut.c` | Selection behavior and separator mutation |
| `tests/textutils.mk` | Host and guest builds, fixture mappings, and control dependencies |
| `tests/textutils/cases.txt` | Tagged host comparison cases |
| `tests/textutils/guest_cases.txt` | Guest command cases |
| `tests/textutils/fixture/words.txt` | Input used by this regression |
| `tests/textutils/run_host.py` | Reference selection, filtering, and host comparisons |
| `tests/boot/run-textutils-test.sh` | Guest execution and extracted output comparisons |
| `tools/mk_wired.py` | Test fragment reachability audit |
| `tests/fullsystem.mk` | Full system and commit verification targets |
| `tools/verify-commit.sh` | Clean clone build and integration check |

## See also

- [Building LogitOS](building.md)
- [Running LogitOS in QEMU](running-in-qemu.md)
- [Contribution workflow](../contributing/workflow.md)
- [Writing tests](../contributing/writing-tests.md)
- [Testing](../architecture/testing.md)