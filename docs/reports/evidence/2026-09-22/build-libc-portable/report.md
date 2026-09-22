# Portable search/uio host gates — 2026-09-22

Changed only the two assigned fixtures and five new narrow test/helper files.
No libc production source, Makefile or tests/libc.mk edits by this agent.
Root owns rule wiring and has integrated both ordinary targets and controls.
Both original fixtures were clean with mtime 1789739248 before this task.
Original GNU fixture bodies are byte-for-byte preserved behind LIBC_COMMON_TEST;
the default branch remains the original full GNU differential.

## Host capability evidence

Both original ordinary targets failed while compiling the Darwin reference:
search-before.log and uio-before.log retain the full errors. Actual compile and
runtime probes correct the initial assumption that all these functions are absent:

- tdestroy/hcreate_r/hsearch_r/hdestroy_r: absent declarations and absent runtime symbols.
- preadv/pwritev: present runtime symbols; macOS SDK declares them for macOS 11+
  when _DARWIN_C_SOURCE is enabled. The original fixture's POSIX/XOPEN feature
  macros hid those declarations. They are now exercised against the actual
  host APIs; no stand-in implementation and no skip for these two APIs.
- Darwin O_CREAT=512, O_TRUNC=1024, O_NONBLOCK=4, F_SETFL=4. The original
  fixture hardcoded Linux O_CREAT=64/O_TRUNC=512/F_SETPIPE_SZ=1031.
- Darwin readv(valid_fd, NULL, 0) returns -1/EINVAL; the project deliberately
  follows its documented Linux-style zero-vector validation semantics.

capabilities-final.c/log is a successful runtime probe. hash_probe.c/log checks
real Darwin duplicate ENTER preserving data and FIND-miss leaving errno zero.
The first exploratory capacity probe aborted because it passed stack keys to
Darwin hdestroy. That is a harness ownership error, not a production defect.
The portable case uses allocated keys and frees them only where the real API
leaves ownership with the caller. Apple documents its key ownership:
https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/hcreate.3.html

## Results

| Family | Common assertions | Additional project contracts | Ordinary diff | ASan+UBSan diff |
| --- | ---: | ---: | --- | --- |
| search | 61 | 10 | byte-identical, exit 0 | byte-identical, exit 0 |
| uio | 37 | 9 | byte-identical, exit 0 | byte-identical, exit 0 |

Search compares tree membership/deletion/sorted walk projection, linear search,
queue links, and common hash insertion/find/duplicate behavior. The project-only
contracts test tdestroy callbacks and the independent/reusable hsearch_r tables.
No invented Darwin tdestroy or hsearch_r is linked into the reference.

UIO compares actual vector bytes/counts, short reads, EOF, descriptor/vector
errors, full nonblocking pipe refusal, preadv/pwritev position and content.
Its separately compiled host helper only prepares real files and pipes, using
host header constants. It does not implement any vectored API under test.
Project-only contracts cover the deliberate zero-vector, partial-EFAULT and
safe ioctl rejection paths. The successful FIONREAD/fstat path is excluded
because a host struct stat has a different ABI from the project structure.

Missing GNU reference coverage is explicitly printed to stderr, along with
`make test-libc-search` / `make test-libc-uio` on a Linux glibc host to settle
those original contracts. GNU hash sizing/errno/null-root policy and the Linux
ioctl/zero-vector/pipe specifics are not silently normalized into a fake match.
GNU full differential was not run on this Darwin host. No VM was launched.

## Observed negative controls

libc_portable_control.py copies the actual production source, requires exactly
one mutation anchor, builds each changed copy, and requires exit 1 plus the
named semantic failure. A compiler error or crash cannot pass the control.

- search/find changes a real successful tfind return to NULL: 15 tree lookup failures.
- search/destroy removes the real key callback: the callback assertion fails.
- uio/position removes the real restoring lseek: preadv cursor assertion fails
  (3 common and 2 dependent project assertions fail in total).
- uio/progress loses the actual byte count on a later read error: its named
  project assertion fails, while all 37 common assertions remain green.

Logs/commands/mutated source: libchost/portable-control/{search,uio}/.
These are prerequisites of the corresponding ordinary targets after root wiring.

## Replay and artifacts

`make BUILD=build-libc-portable test-libc-search test-libc-uio`

Integrated ordinary run: integrated.log; final search callback-independence
check: search-final.log. Sanitizer output/stderr: *-sanitize.out / *-sanitize.err.
The sanitizer builds instrumented both the real project sources and, for uio,
the separately compiled host setup helper. Both host reference executables were
also instrumented. Final owned-file hashes and mtimes: final-sources.json.
`git diff --check` and Python control-script byte-compilation passed.

The full original fixture bodies were compared against their frozen pre-edit
copies after removing only the new conditional wrapper and feature macro:
exact equality held for both. This protects the existing GNU gate while making
the Darwin common subset runnable and honest about its remaining boundaries.
