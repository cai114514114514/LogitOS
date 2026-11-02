# Userland

LogitOS userland comprises desktop applications, command line tools, language runtimes, and system services running as x86-64 processes in ring 3. Programs under `c/apps/` reach kernel services through syscall wrappers and the project C library. The build produces ELF executables, wraps them in AEX packages, and installs them into a LogitFS image alongside configuration and assets from `fsroot/`.

## Building and linking programs

The root `Makefile` defines the common application rules. It compiles C with Clang targeting `x86_64-elf`, assembles startup code with NASM, and links with LLD. `UCFLAGS` selects freestanding C11, disables the red zone, and enables SSE and SSE2. Each link recipe supplies the program’s startup objects and dependencies.

Two rule families provide the basic layouts:

| Rule | Program and runtime | Link layout |
|---|---|---|
| `APP_RULE` | GUI source, `c/apps/crt0.asm`, widget toolkit, and graphics objects | Text address supplied by the application’s rule |
| `CLI_RULE` | Command source, `c/apps/crt0_cli.asm`, and optional additional objects | Text address `0x50000000` |

Processes have their own address spaces, allowing CLI programs to share a link address. Address space creation and executable loading live in `c/kernel/exec/load/exec.c`. Applications with larger dependency sets, including the browser and media applications, have custom link recipes in `Makefile`.

`tools/mkaex.py` packages the ELF executable with a display name, icon, file association, application identity, and stack size hint. The window manager reads package headers to build its application registry. See [Applications and AEX packages](applications-aex.md).

For applications selected by the agent catalogue, `tests/agent.mk` routes the link through `tools/agent_link.py`. The wrapper retains the application’s object list, adds the agent entry object and SDK and libc archives, and selects `_agent_start`. It writes a manifest sidecar containing the ELF digest. `tools/mkaex.py` checks that digest before incorporating the manifest into an AEX version 3 package, so metadata follows the executable that was linked.

The build provides distinct targets for the bootable ISO and application images:

| Make definition or command | Purpose |
|---|---|
| `all`, selected by `make` | Builds the bootable ISO |
| `DISK` | Development image containing applications, assets, and guest test material |
| `make build/disk.img` | Builds the development image with the default `BUILD` value |
| `RELEASE_DISK`, built by `make release-disk` | Image assembled with release exclusions |
| `make run` | Builds and boots the ISO with the release image by default |
| `make run-dev` | Builds and boots the ISO with the development image |

`DISK` and `RELEASE_DISK` are generated targets under `BUILD`. Rebuilding the disk target incorporates application changes into the image. See [Building LogitOS](../getting-started/building.md) and [Build system](build-system.md).

## Syscalls and the C library

`c/apps/logit.h` provides inline syscall wrappers using `int 0x80`. Call numbers and shared argument contracts come from `include/abi/logit_abi.h`.

Small utilities use `c/apps/clib.h`, which supplies inline string helpers and descriptor I/O over those wrappers. For example, `c/apps/coreutils/echo.c` writes through its output helpers. This support layer lets a utility link its CLI startup code and application objects directly.

The C library supplies interfaces for applications and ported software. Public headers live in `c/apps/libc/include/`, and implementations live in `c/apps/libc/src/`.

| Area | Implementation |
|---|---|
| File descriptors and process operations | `c/apps/libc/src/io.c` |
| Formatted and buffered I/O | `c/apps/libc/src/stdio.c` |
| Strings, conversions, and environment | `c/apps/libc/src/string.c`, `c/apps/libc/src/stdlib.c` |
| Allocation and memory mappings | `c/apps/libc/src/malloc.c`, `c/apps/libc/src/mman.c` |
| Socket adapters and address lookup | `c/apps/libc/src/socket.c`, `c/apps/libc/src/netdb.c` |
| Threads and synchronization | `c/apps/libc/src/pthread.c`, `c/apps/libc/src/threads.c` |

The syscall adapters translate kernel results into library return values and `errno`. For example, `read()` in `c/apps/libc/src/io.c` distinguishes interrupted operations from operations that would block.

The allocator in `c/apps/libc/src/malloc.c` uses size classes, free lists, and coalescing of adjacent free blocks. Its guest implementation reserves anonymous virtual memory through the kernel. A configured occupancy bound and periodic checks of free physical memory constrain growth, preserving memory headroom for the kernel and other processes.

GUI link recipes also include the widget toolkit and graphics libraries. [Graphics and windowing](graphics-and-windowing.md) describes their place in the system.

## Process startup

The loader prepares executable mappings and the initial stack. The scheduler in `c/kernel/sched/sched.c` creates a thread carrying the entry address, user stack, address space, and TLS base. Its first dispatch reaches the assembly path in `c/boot/enter_user.asm`, which enters ring 3.

For CLI execution, `c/kernel/exec/load/exec.c` constructs a SysV stack containing the argument count, argument pointers, environment pointers, and auxiliary vector. The auxiliary vector includes page size, program header information, entry addresses, credentials, and pointers to startup data.

The startup paths then adapt this state to the application:

| Startup source | Entry behavior |
|---|---|
| `c/apps/crt0_cli.asm` | Reads arguments, publishes the environment through `environ` and `__libc_environ_hook`, aligns the stack, calls `main()`, and passes its return value to `SYS_EXIT` |
| `c/apps/crt0.asm` | Calls `app_main()` and invokes `SYS_EXIT` with status zero when it returns |
| `c/apps/crt0_agent.asm` | Reads the startup vectors, calls `ag_activation()`, and either exits or restores the initial stack and jumps to the application’s `_start` |

The CLI startup object owns the environment symbols so utilities using inline support and applications linking libc can share that runtime. GUI applications retrieve the window manager’s launch argument through `get_arg()` in `c/apps/logit.h`.

For GUI launches, `c/kernel/gui/wm.c` uses `exec_interpreter_stack()` for AEX version 3 images and images using an ELF interpreter. That helper builds a SysV stack with the executable path and an optional launch argument. Legacy GUI images receive the stack used by `c/apps/crt0.asm`.

Agent worker activation is selected through kernel identity in `c/lib/agent/activation.c`. Returning from activation with the application continuation result transfers execution to the existing startup runtime.

Initial console processes and applications launched by the window manager receive descriptors 0, 1, and 2 backed by the serial terminal. The shell in `c/apps/coreutils/sh.c` builds pipelines with pipes, forks children, redirects descriptors, and executes programs. For a command name containing no slash, it tries `/bin` before trying the supplied name as a path.

## The installed file system

`DISK_PACK` in `Makefile` assembles the installation list, including additions from included make fragments. `tools/mkfs.py` maps host files and directory trees to guest destinations and creates parent directories as needed.

| Guest location | Installed content |
|---|---|
| `/` | Desktop AEX applications, including Browser and Files |
| `/bin` | Shell, utilities, services, language runtimes, and TinyCC |
| `/sbin/greeter.aex` | Desktop login application |
| `/etc` | Configuration, including `fsroot/etc/agent.conf` |
| `/fonts` and `/ime` | Fonts and input method data |
| `/docs` and `/media` | Documents and media assets |
| `/www` | Web server content |
| `/usr/include` and `/usr/lib` | C development headers, archives, and startup objects |
| `/usr/as` | AetherScript libraries, examples, and compiled example programs |
| `/lib/firmware/amdgpu` | AMD Polaris firmware |
| `/licenses` | License texts and provenance records |

Desktop placement has a functional role. `scan_apps()` in `c/kernel/gui/wm_dock.c` scans the root directory, reads AEX headers, and adds applications to the desktop registry. CLI packages appear under command names such as `/bin/sh`.

The packer records executable mode `0755` for files ending in `.aex` and files placed under executable directories such as `/bin`. This supplies the permission bits used by execution checks in `c/kernel/exec/load/exec.c`.

`Makefile` packs firmware from `third_party/amd-firmware/amdgpu/` into both images and installs its license and manifest under `/licenses/amd-firmware`. The reader in `c/drivers/gpu/amd/polaris/resource/files.c` tries the root file system first, then a boot module. It checks the format and SHA-256 pin of each accepted blob.

The release recipe applies `RELEASE_EXCLUDE` to the shared installation list. Both image recipes use `MKFS_PROFILE` to preserve `/browser`, `/state`, `/home`, and `/download` from the image being rebuilt. For `/docs` and `/etc`, `tools/disk_profile.py` merges existing contents with packaged entries: existing file bytes and metadata take precedence, while newly packaged siblings remain available.

After mounting LogitFS as the root file system, `c/kernel/init/kmain.c` creates the `/proc` mount point and mounts procfs there.

## C development inside the guest

`tests/sysroot.mk` defines `SYSROOT` as a staging tree under `BUILD` and constructs it through `tools/mksysroot.py`. The disk installation list maps this tree onto the guest root.

The staged environment contains libc headers under `/usr/include`, `libc.a` under `/usr/lib`, and TinyCC headers and compiler support under `/usr/lib/tcc`. The startup object assembled from `c/apps/crt0_cli.asm` is installed as `/usr/lib/crt1.o`, matching TinyCC’s expected name. The accompanying `crti.o` and `crtn.o` are empty objects.

The libc archive combines libc and math objects. By default, `tests/sysroot.mk` also includes TinyCC’s support objects in that archive. `tools/mksysroot.py` explains the dependency: compiler support routines can introduce references to libc after TinyCC has scanned its libc archive. Including those objects in `libc.a` lets its archive scan resolve the dependency chain.

AetherScript is another installed language runtime, built from `c/apps/as/` and installed as `/bin/as`.

## Login and system services

The window manager starts `/bin/login` on the serial console. `c/apps/coreutils/login.c` authenticates against `/etc/passwd`, establishes the session, changes to the account’s home directory, and executes its configured shell with `HOME` and `USER` in the environment. An absent account file selects a root shell and prints the account enrollment message.

For standard builds, a nonempty `/etc/passwd` selects the locked desktop and launches `/sbin/greeter.aex`. A session change to a nonzero user ID unlocks the desktop and starts Files. Console authentication can establish that session.

The draft’s earlier wording restricted greeter startup to builds without `HWHUD=1`. Both build modes launch it when their account check selects a locked desktop; the hardware bring-up mode adds a developer-account exception.

For `HWHUD=1` builds, `c/boot/efi/loader.c` can pass public-key records from `\LOGIT\DEVSSH.PUB` to the kernel. After the root file system mounts, `c/kernel/init/devssh.c` validates the Ed25519 record format, provisions the `dev` account with a locked password marker, writes `/home/dev/.ssh/authorized_keys`, and creates `/etc/sshd.enabled` after the account and keys are ready. These files persist on the root file system. In this build mode, the window manager starts the desktop without authentication when the account store contains just matching `dev` account rows with that locked marker. Other account rows select the greeter.

Service startup is distributed across console login, the window manager, and command execution:

| Service | Role and startup |
|---|---|
| `/bin/httpd` | Serves `/www` by default; console login checks `/etc/httpd.enabled` |
| `/bin/httpsd` | Serves HTTP over TLS; console login checks `/etc/httpsd.enabled` |
| `/bin/sshd` | Authenticates remote sessions and launches commands or shells; console login checks `/etc/sshd.enabled` |
| `/bin/sftpd` | Runs as an SSH subsystem child over standard input and output |
| `/bin/syslogd` | Receives local log datagrams when invoked |
| `/bin/agentd` | Brokers tasks; the window manager checks its process state and restarts it |

The helper in `c/apps/coreutils/boot_services.h` runs before console authentication. It accepts enable files that are regular files, owned by root, and protected against group and other writes. Each service child pins root credentials, receives an input pipe at EOF, redirects output to its log under `/var/log`, and executes the service. Giving daemons their own streams keeps the serial console available for login.

HTTP and HTTPS share `c/apps/services/http/httpd.c`; `tests/servers.mk` builds the TLS variant. The parent polls the listener and tracks up to four forked workers, giving each request its own process state.

SSH connection handling lives in `c/apps/services/ssh/sshd.c`. Child setup in `c/apps/services/ssh/sshd_channels.h` connects pipes or a PTY, adopts the authenticated account’s credentials, and executes its shell or `/bin/sftpd`.

`c/apps/services/syslog/syslogd.c` receives Unix datagrams at `/var/log/sock`, defined by `LOGIT_PATH_LOG` in `include/abi/logit_abi.h`. It timestamps records with seconds since boot, writes them to standard output, and appends them to `/var/log/messages` when that file opens successfully. Datagram boundaries preserve incoming records.

The broker in `c/apps/agent/agentd.c` serves a local Unix socket and persists task state under `/state/agents`. Its event loop handles client requests, task controls, and model operations. See [Agent runtime](../userland/agent-runtime.md).

## Limitations

- C and POSIX compatibility is partial. For example, `fchdir()` and `getppid()` return `ENOSYS` in `c/apps/libc/src/io.c`.
- `errno` has shared storage within a process, as declared in `c/apps/libc/include/errno.h` and defined in `c/apps/libc/src/io.c`. Locale selection in `c/apps/libc/src/locale.c` supports the C locale.
- Returning from `main()` or `app_main()` reaches `SYS_EXIT` directly through startup assembly. Programs requiring registered exit handlers and buffered stream cleanup must call libc’s `exit()`, implemented in `c/apps/libc/src/stdlib.c`.
- `waitpid()` exposes the kernel’s unshifted exit status. `c/apps/libc/src/io.c` documents its incompatibility with POSIX wait status encoding.

## Source map

| File or directory | What it contains |
|---|---|
| `Makefile` | Application compilation, linking, packaging, installation lists, and image selection |
| `c/apps/logit.h`, `include/abi/logit_abi.h` | Syscall wrappers and shared kernel ABI |
| `c/apps/clib.h`, `c/apps/coreutils/echo.c` | Inline utility support and a small consumer |
| `c/apps/libc/include/`, `c/apps/libc/src/` | C library interfaces and implementations |
| `c/apps/crt0.asm`, `c/apps/crt0_cli.asm`, `c/apps/crt0_agent.asm` | GUI, CLI, and agent startup assembly |
| `c/kernel/exec/load/exec.c` | Executable loading, address space creation, and startup stacks |
| `c/kernel/sched/sched.c`, `c/boot/enter_user.asm` | User thread creation and entry into ring 3 |
| `c/kernel/gui/wm.c` | GUI launches, desktop login, console startup, and broker supervision |
| `c/kernel/gui/wm_dock.c` | Discovery of desktop applications |
| `tools/mkaex.py` | AEX packaging and metadata validation |
| `tools/agent_link.py`, `tests/agent.mk` | Agent integration into application links |
| `c/lib/agent/activation.c` | Dispatch to agent workers or application startup |
| `tools/mkfs.py`, `tools/disk_profile.py`, `fsroot/` | Image construction, state preservation, and packaged content |
| `third_party/amd-firmware/`, `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware payloads and validation during loading |
| `tools/mksysroot.py`, `tests/sysroot.mk` | Guest C development environment |
| `c/kernel/init/kmain.c` | Root and procfs mounting, and developer SSH provisioning hook |
| `c/boot/efi/loader.c`, `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h` | Developer SSH key handoff, account provisioning, and desktop lock policy |
| `c/apps/coreutils/sh.c`, `c/apps/coreutils/login.c` | Shell execution and console authentication |
| `c/apps/coreutils/boot_services.h` | Service enable checks and launch setup |
| `c/apps/services/`, `tests/servers.mk` | HTTP, HTTPS, SSH, SFTP, and logging services |
| `c/apps/agent/agentd.c` | Task broker and persistent task handling |

## See also

- [System calls](system-calls.md)
- [Processes and threads](processes-and-threads.md)
- [Applications and AEX packages](applications-aex.md)
- [The C library](../userland/libc.md)
- [System services](../userland/services.md)