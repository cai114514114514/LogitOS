# Source layout

The tree is organized by execution boundary, then subsystem. Source paths do
not determine executable names or paths inside the guest filesystem.

| Directory | Responsibility |
| --- | --- |
| `c/boot/{bios,efi}` | Native BIOS and UEFI loaders |
| `c/kernel/` | Initialization, CPU, memory, scheduling, process execution, desktop, diagnostics |
| `c/drivers/` | Hardware drivers grouped by device class |
| `c/fs/{vfs,logitfs,cache,procfs,ramfs,ctl}` | Filesystem interfaces, implementations and controls |
| `c/net/` | Network protocols and shared transports |
| `c/crypto/` | Cryptographic primitives and trust policy |
| `c/lib/` | Shared libraries; some also link into the kernel |
| `c/apps/gui/` | Desktop applications, each with its own directory, plus AUI |
| `c/apps/coreutils/` | Shell, commands, login and shared command headers |
| `c/apps/services/{http,ssh,syslog}` | HTTP/HTTPS, SSH/SFTP and logging service programs |
| `c/apps/{as,lm}` | Language toolchain and language-model application |
| `c/apps/gui/browser/` | Browser application, rendering pipeline and web platform |
| `c/apps/{agent,libc}` | Application integration and userspace C library |
| `c/apps/*.{h,asm,inc}` | Shared application ABI wrappers and startup code |
| `include/abi/` | Kernel/userspace and boot protocol contracts |
| `rust/` | Shared Rust implementation code |
| `tests/guest/` | Diagnostic programs executed inside LogitOS, grouped by subsystem |
| `tests/unit/` | Host tests, reference comparisons and test support |
| `tests/{boot,qmp}/` | Guest orchestration and input/screenshot drivers |
| `tests/fixtures/` | Test data with provenance |
| `tools/` | Build generators, audits and development tools |
| `fsroot/` | Files packaged into the guest filesystem |
| `third_party/` | Vendored components with their original licenses |

## Application boundaries

Browser has `app`, `dom`, `html`, `css`, `layout`, `paint`, `net`, `storage`,
`frame` and `js` subsystems. JS is further divided into `runtime`, `dom`,
`style`, `net`, `storage`, `workers`, `media`, `graphics`, `crypto`, `text` and
`wasm`. See its [module map](../c/apps/gui/browser/README.md). Redundant
filename prefixes were removed; public function names remain unchanged.

Clock's event loop, model and rendering modules live together in
`c/apps/gui/clock/`. Studio's window adapter lives in `c/apps/gui/studio/`, with
its independently host-testable application engine in `core/`. Keeping the
engine as a separate library prevents host tests from needing GUI syscall
stubs; its inclusion in the application directory does not merge those layers.

Services use `c/net/` for protocols; daemon entry points are not network stack
implementations. Likewise, guest diagnostics are test consumers of `c/lib/`
and the syscall ABI, not libraries or ordinary commands. Some diagnostics are
still installed on the product disk for existing tests and developer workflows.
Relocation does not change that packaging policy.

`CLI_RULE` in the root Makefile resolves explicit `CLI_SOURCE_*` overrides
before falling back to coreutils. This is necessary because `/bin/httpd` and
`/bin/smptest` keep their installed names even though their source ownership is
different. Custom links for SSH, HTTPS, codecs and libc-based probes remain in
their existing test fragments; merging them would lose their link contracts.

Headers stay with their implementation. Global `INCDIRS` scans `c/` and
`include/`; guest probes are deliberately outside that scan. Host tests use
narrow include paths. When moving a header, update both relative includes and
those explicit host paths: a successful kernel build cannot prove the latter.

## Migration on 2026-09-22

The following former paths remain in dated audit reports as historical
evidence. Use this table to locate their current sources.

| Former location | Current location |
| --- | --- |
| `c/apps/{audio,media,net,video}/` | `tests/guest/{audio,media,net,video}/` |
| `c/apps/coreutils/{smptest,schedtest,thrtest}.c` | `tests/guest/sched/` |
| `c/apps/coreutils/socktest.c` | `tests/guest/net/socktest.c` |
| `c/apps/coreutils/sndtest.c` | `tests/guest/audio/sndtest.c` |
| `c/apps/coreutils/{crash,polltest}.c` | `tests/guest/exec/` |
| `c/apps/coreutils/httpd*`, `https_*` | `c/apps/services/http/` |
| `c/apps/coreutils/sshd*`, `sftpd.c` | `c/apps/services/ssh/` |
| `c/apps/coreutils/syslogd.c` | `c/apps/services/syslog/syslogd.c` |
| `c/apps/clock/` | `c/apps/gui/clock/` (old README is `MODEL.md`) |
| `c/apps/studio/` | `c/apps/gui/studio/core/` |
| `c/apps/browser/` | `c/apps/gui/browser/`, then the subsystem/name map in its README |

The application/service/diagnostic migration moves 56 files; Browser's 173
files are additionally grouped and renamed. These retain their per-path MIT license: both
application and test sources are outer components under `LICENSING.md`.
Kernel and driver directories already had subsystem divisions and retain them.
Generated output, vendored code and frozen evidence directories are not source
ownership boundaries to reorganize along with these programs.

Build from the ordinary entry points. For an independent validation build:

```sh
make BUILD=build-layout -j6 all build-layout/disk.img
make BUILD=build-layout test-mk-wired test-clock-ui test-server-semantics test-sftpd
make BUILD=build-layout test-studio-render test-studio-core
```

Use a command-line `BUILD=build-...` assignment and an explicit disk target.
The default `make` builds only the ISO; it does not rebuild ring-3 programs.

Object paths must follow relocated sources too. Existing `.d` files still name
the former primary `.c`; `-MP` only tolerates removed headers. Clock, Studio,
relocated CLI programs and guest probe variants therefore have new object paths,
so an existing `build/` can rebuild without cleaning it or reusing stale objects.
`make test-build-relocation` checks this with old dependency files present.
