# Command-line tools

LogitOS keeps 57 command-line programs in `c/apps/coreutils/` and its `text/` subdirectory. They cover file operations, text processing, shell execution, system inspection, networking and desktop interaction. Most are installed under `/bin` as AEX packages. The two graphics compiler frontends are installed as ELF executables through the SDK sysroot. `Makefile` and its build fragments define their compilation, linking and installation.

## Execution and installation

`CLI_RULE` in `Makefile` compiles a command’s source, assembles `c/apps/crt0_cli.asm`, links the objects at `0x50000000` and packages the ELF executable with `tools/mkaex.py`. The disk pack list supplies installed paths such as `/bin/echo`.

`c/apps/crt0_cli.asm` reads the initial argument vector, publishes the environment pointers, calls `main()` and passes its return value to the exit syscall. Small commands use the inline helpers in `c/apps/clib.h`. Text utilities link mini-libc through `TEXTUTIL_RULE` in `tests/textutils.mk`, providing allocation, standard I/O and directory interfaces.

For programs listed in `c/apps/agent/catalog.json`, `tests/agent.mk` and `tools/agent_link.py` augment the link with activation startup and SDK/libc archives. `c/apps/crt0_agent.asm` calls the dispatcher in `c/lib/agent/activation.c`, which selects worker execution from the kernel’s application identity. When dispatch returns control to the original startup, the assembly restores the initial stack.

Installation extends beyond the main `CLI` list. `tests/sched.mk` adds `nice` and `renice`; `tests/audio.mk` adds `rec`; and `tests/agent.mk` installs `agentctl`, `free`, `ps`, `readcore` and `uptime`.

## Files and byte streams

| Tool | Description | Source |
|---|---|---|
| `cat` | Copies named files, or standard input, to standard output. | `c/apps/coreutils/cat.c` |
| `cp` | Copies a file or, with `-r`, a directory tree. | `c/apps/coreutils/cp.c` |
| `echo` | Prints arguments separated by spaces and followed by a newline. | `c/apps/coreutils/echo.c` |
| `head` | Prints the first ten lines, or a count selected with `-n`, from a file or standard input. | `c/apps/coreutils/head.c` |
| `ls` | Lists directory entries, with `-a` for dotfiles and `-l` for metadata. | `c/apps/coreutils/ls.c` |
| `mkdir` | Creates each directory named on the command line. | `c/apps/coreutils/mkdir.c` |
| `mv` | Moves or renames a path through the rename syscall. | `c/apps/coreutils/mv.c` |
| `pwd` | Prints the current working directory. | `c/apps/coreutils/pwd.c` |
| `rm` | Removes each path named on the command line. | `c/apps/coreutils/rm.c` |
| `stat` | Prints metadata and provides operations for modes, ownership, umask and links. | `c/apps/coreutils/stat.c` |
| `touch` | Opens each named file with creation enabled, creating it when needed. | `c/apps/coreutils/touch.c` |
| `wc` | Counts newlines, words delimited by spaces, tabs or newlines, and bytes. | `c/apps/coreutils/wc.c` |

`ls -l` obtains metadata through `c/apps/logit_stat.h` and prints numeric ownership, link counts, sizes and modification times. A dot following the mode string marks default permission bits. `stat` exposes the metadata attribute bits directly. `LSTA_MODE_STORED` and `LSTA_MODE_DURABLE` in `include/abi/logit_abi.h` identify recorded ownership and mode information, and whether that record is stored on the medium.

`cat`, `cp` and `wc` use 64 KiB buffers in static storage. This keeps the buffers out of their stack frames, including the recursive copying path in `cp`. `cat` and `head` retry short writes so output can pass through a pipe over several writes. These choices are implemented in their respective command sources.

## Text processing and filesystem queries

These programs use mini-libc in the guest. `tests/textutils.mk` also builds the same sources against the host C library for comparison checks.

| Tool | Description | Source |
|---|---|---|
| `chmod` | Changes permission modes using octal values or symbolic clauses. | `c/apps/coreutils/text/chmod.c` |
| `cut` | Selects byte positions or delimited fields from input lines. | `c/apps/coreutils/text/cut.c` |
| `df` | Reports filesystem capacity or inode usage using filesystem statistics and the mount table. | `c/apps/coreutils/text/df.c` |
| `diff` | Compares two files and emits normal or unified line differences. | `c/apps/coreutils/text/diff.c` |
| `du` | Walks paths and reports allocated disk space, with summary and size-formatting options. | `c/apps/coreutils/text/du.c` |
| `find` | Walks directory trees and evaluates predicates, Boolean expressions and actions. | `c/apps/coreutils/text/find.c` |
| `grep` | Selects lines using basic regular expressions, extended regular expressions or fixed strings. | `c/apps/coreutils/text/grep.c` |
| `kill` | Sends named or numbered signals to process IDs, defaulting to termination. | `c/apps/coreutils/text/kill.c` |
| `ln` | Creates hard or symbolic links, including links within a destination directory. | `c/apps/coreutils/text/ln.c` |
| `sed` | Applies addressed editing commands, substitutions, hold-space operations and branches. | `c/apps/coreutils/text/sed.c` |
| `sort` | Sorts lines by whole-line or field keys, with numeric, stable and unique modes. | `c/apps/coreutils/text/sort.c` |
| `tail` | Prints the last lines or bytes, or output beginning at a selected line or byte. | `c/apps/coreutils/text/tail.c` |
| `tr` | Translates, deletes or squeezes bytes selected by sets, ranges and character classes. | `c/apps/coreutils/text/tr.c` |
| `uniq` | Groups adjacent equal lines and can print counts, repeated groups or unique groups. | `c/apps/coreutils/text/uniq.c` |

Several implementation choices make script results reproducible. `sort -n` compares decimal strings digit by digit, preserving distinctions between large integers. Its merge sort preserves input order for equal keys under `-s`. `du` derives usage from allocated blocks and tracks device/inode pairs when counting hard links. `find` sorts directory entries by name before visiting them, making traversal order consistent between the host and guest builds. Each mechanism lives in the corresponding tool source.

`grep` and `sed` share the regular-expression engine in `c/apps/coreutils/text/lre.c`. Its Pike VM advances active matching states together while scanning the input. For a fixed pattern, matching time grows linearly with input length. An allocated work stack handles transitions between matching states, avoiding recursion over the input. Whole matches select the earliest starting position and the longest match at that position; `c/apps/coreutils/text/lre.h` describes the matching rules.

## Shell, execution and scheduling

| Tool | Description | Source |
|---|---|---|
| `sh` | Runs commands, pipelines, background jobs, command strings and script files. | `c/apps/coreutils/sh.c` |
| `login` | Enrols accounts, authenticates console sessions and executes an account’s shell. | `c/apps/coreutils/login.c` |
| `nice` | Prints the caller’s nice value or adjusts it before executing a command. | `c/apps/coreutils/nice.c` |
| `renice` | Sets process nice values or displays their current nice values and scheduler weights. | `c/apps/coreutils/renice.c` |
| `sleep` | Waits for a number of seconds while yielding between checks of the wall clock. | `c/apps/coreutils/sleep.c` |
| `true` | Returns exit status zero. | `c/apps/coreutils/true.c` |
| `false` | Returns exit status one. | `c/apps/coreutils/false.c` |

`sh` supports quoting, variable expansion, filename patterns, pipes, input and output redirection, append redirection and background execution. Semicolons and newlines divide sequential commands. `sh -c` executes a command string; script-file invocation supplies positional parameters. For a command name without a slash, lookup first tries the name under `/bin`, then the supplied name. These paths are implemented in `c/apps/coreutils/sh.c`.

The shell provides these builtins:

| Builtin | Purpose |
|---|---|
| `cd` | Changes the shell’s current directory. |
| `pwd` | Prints the shell’s current directory. |
| `exit` | Exits with the supplied status or the previous command’s status. |
| `export` | Stores a supplied assignment in the environment passed to commands. |
| `unset` | Removes a named environment entry. |
| `set` | Prints the stored environment. |
| `history` | Prints retained command history. |
| `jobs` | Lists tracked jobs and their state. |
| `fg` | Waits for a selected job, or all tracked jobs, in the foreground. |
| `wait` | Uses the same job-waiting path as `fg`. |
| `help` | Prints command, syntax and editing help. |

A command with one pipeline stage reaches the builtin dispatcher before process creation. This lets `cd` change the shell’s own directory. The executable in `c/apps/coreutils/pwd.c` supplies working-directory output for pipelines. A standalone assignment also updates the shell’s stored environment.

In GUI Terminal mode, the shell handles line editing, completion and history. A control pipe carries input events to the shell, which forwards typed text into the foreground job’s input pipe. Interrupt handling sends SIGINT to the pipeline’s processes; an end-of-input event closes the job’s input writer. Keeping control events on their own channel lets the shell process them while a child reads standard input. See `c/apps/coreutils/sh.c`.

`nice` adds an increment to its own nice value before executing the requested program. `renice` sets an absolute value and prints the value installed by the kernel together with its scheduler weight. Their implementations are in `c/apps/coreutils/nice.c` and `c/apps/coreutils/renice.c`.

## System inspection and diagnostics

| Tool | Description | Source |
|---|---|---|
| `entropy` | Exercises random-byte generation, reports seed strength and emits samples across a fork. | `c/apps/coreutils/entropy.c` |
| `execinfo` | Prints and checks the initial argument, environment and auxiliary-vector layout. | `c/apps/coreutils/execinfo.c` |
| `free` | Summarizes physical memory and kernel heap figures from `/proc/meminfo`. | `c/apps/coreutils/free.c` |
| `ps` | Lists processes from `/proc`, adding thread identifiers and GUI flags under `-l`. | `c/apps/coreutils/ps.c` |
| `readcore` | Reads a LogitOS ELF core dump and prints registers, fault details and region information. | `c/apps/coreutils/readcore.c` |
| `uname` | Prints `LogitOS x86_64`. | `c/apps/coreutils/uname.c` |
| `uptime` | Prints elapsed uptime, raw seconds or a diagnostic for descriptor freshness. | `c/apps/coreutils/uptime.c` |

`ps`, `free` and `uptime` consume files published by `c/fs/procfs/procfs.c`, using file interfaces also available to other commands. `ps` skips unreadable or malformed process records during enumeration. `free` displays a question mark for a missing field. `uptime -d` holds a descriptor open across a delay and compares its result with a fresh open.

`readcore` uses `c/apps/coreutils/corefmt.h` to parse ELF headers and notes through byte loads and bounds checks. Its output includes dump truncation flags and reasons for captured or omitted region contents. This lets a reader distinguish memory present in the dump from regions described by metadata.

## Networking, settings and task control

| Tool | Description | Source |
|---|---|---|
| `agentctl` | Inspects and controls tasks, documents, proposals, source slices, file references and application memory. | `c/apps/coreutils/agentctl.c` |
| `net` | Reports network configuration, probes connectivity, resolves names, fetches downloads and computes or verifies digests. | `c/apps/coreutils/net.c` |
| `ping` | Sends IPv4 ICMP echo requests through a raw socket and reports replies and timing statistics. | `c/apps/coreutils/ping.c` |
| `pkgverify` | Verifies signed LPK packages, lists trusted signers and can extract a verified payload. | `c/apps/coreutils/pkgverify.c` |
| `pref` | Reads, writes and lists values in the kernel settings store. | `c/apps/coreutils/pref.c` |
| `poweroff` | Requests shutdown and reports an error if the syscall returns. | `c/apps/coreutils/poweroff.c` |
| `reboot` | Requests a machine reset and reports an error if the syscall returns. | `c/apps/coreutils/reboot.c` |

`net` shares digest selection and incremental hashing through `c/apps/coreutils/net_digest.inc`. It supports SHA-256, SHA-512, BLAKE2b with a digest of 512 bits and BLAKE3 with a digest of 256 bits. `c/apps/coreutils/net_fetch.inc` handles HTTP and HTTPS requests, response completion, redirects and download destinations. Saving follows response validation and any requested checksum comparison. The saved content is then read back and hashed. Automatic downloads use a staging directory before renaming into the download directory.

`pkgverify` uses the signer set compiled into its executable through its link rule in `Makefile`. The verifier in `c/crypto/trust/pkgsig.c` checks the payload digest, signature and signer trust. `--any` accepts a valid signature from an otherwise untrusted signer and prints that signer’s key. Payload extraction follows successful verification.

`pref` assembles result lines in buffers before writing them. The reason is recorded in `c/apps/coreutils/pref.c`: kernel diagnostics share the serial output stream, so fragmented writes can interleave with unrelated log messages.

Service executables live under `c/apps/services/`; see [System services](services.md).

## Desktop and media commands

| Tool | Description | Source |
|---|---|---|
| `chart` | Reads labels and values from standard input and emits a bar chart or text bars. | `c/apps/coreutils/chart.c` |
| `clear` | Clears rich Terminal scrollback or emits console clear-screen escapes. | `c/apps/coreutils/clear.c` |
| `clip` | Copies, pastes and inspects clipboard content, and provides diagnostic subcommands. | `c/apps/coreutils/clip.c` |
| `dir` | Emits a directory table with clickable file names or a plain text listing. | `c/apps/coreutils/dir.c` |
| `notify` | Posts a system notification or a numbered burst of notifications. | `c/apps/coreutils/notify.c` |
| `prog` | Demonstrates a progress widget by updating a bar or printing percentage lines. | `c/apps/coreutils/prog.c` |
| `rec` | Captures audio into a PCM WAV file and reports captured frames and peak amplitude. | `c/apps/coreutils/rec.c` |
| `show` | Inspects file contents and selects text output, a media frame or a format summary. | `c/apps/coreutils/show.c` |

Rich commands use LRT/1, defined in `c/apps/coreutils/logit_rich.h`. Text travels through standard output, while structured frames travel through another pipe. The shell makes the rich channel available to the final foreground pipeline stage when its output targets Terminal. Other stages, background commands and commands with output redirection receive the environment for text output. This routing keeps structured payloads out of pipeline data and redirected files.

A producer calls `rt_init()` to discover the channel and `rt_send()` to emit frames. Text written through `rt_out()` or `rt_outn()` advances the producer’s byte count, which frames carry as an ordering anchor. `c/apps/gui/terminal/terminal.c` uses that count when deciding when to display a frame.

`show` identifies content through `c/apps/coreutils/logit_sniff.h`. Its media frames name a file for Terminal to open and decode. During text output, a byte guard continues checking the stream after the initial prefix. `dir` includes file paths as link targets in its table frames.

## Graphics compiler frontends

| Tool | Description | Source |
|---|---|---|
| `olscc` | Compiles OLS-IR shader source into bytecode for OpenLogit. | `c/apps/coreutils/olscc.c` |
| `lslcc` | Compiles typed LSL shader source into bytecode for OpenLogit. | `c/apps/coreutils/lslcc.c` |

`tests/openlogit_sdk.mk` builds these commands. `tools/mkopenlogit_sdk.py` installs their ELF executables as `/bin/olscc` and `/bin/lslcc` within the sysroot.

## Shared helpers

| Helper | Responsibility |
|---|---|
| `c/apps/clib.h` | Inline string operations, integer conversion, path joining, descriptor output and line reading. |
| `c/apps/logit.h` | Userland syscall wrappers and shared application interfaces. |
| `c/apps/logit_stat.h` | Metadata syscall wrappers, directory enumeration and mode formatting. |
| `c/apps/hidden.h` | Visibility predicates, including the dotfile predicate used by `ls`. |
| `c/apps/coreutils/logit_rich.h` | LRT/1 framing, encoding, parsing, channel discovery and text byte accounting. |
| `c/apps/coreutils/logit_cells.h` | UTF-8 navigation and conversion between byte offsets and display cells. |
| `c/apps/coreutils/logit_sniff.h` | Content identification and the binary-output guard shared with Terminal. |
| `c/apps/coreutils/accounts.h` | Account parsing, lookup, serialization and password checking. |
| `c/apps/coreutils/boot_services.h` | Launching enabled services at login with input and log descriptors. |
| `c/apps/coreutils/corefmt.h` | Parsing LogitOS ELF core dumps. |
| `c/apps/coreutils/net_digest.inc` | Digest selection, incremental hashing and file verification for `net`. |
| `c/apps/coreutils/net_fetch.inc` | HTTP fetching and download destination handling for `net`. |
| `c/apps/coreutils/text/lre.c` and `c/apps/coreutils/text/lre.h` | Regular-expression compilation and execution shared by `grep` and `sed`. |

The account format has consumers beyond console login. The graphical greeter, SSH service and mini-libc account lookup also include `c/apps/coreutils/accounts.h`. Password checking calls the implementation in `c/crypto/kdf/pbkdf2.c`. Sharing the parser keeps these consumers aligned on the stored account format.

## Adding a new tool

1. Choose the runtime and a nearby example. `c/apps/coreutils/echo.c` demonstrates a small command using `c/apps/clib.h`. For standard I/O and allocation, follow a mini-libc tool such as `c/apps/coreutils/text/uniq.c`.

2. Register the source and link dependencies. A command directly under `c/apps/coreutils/` can join `CLI` before its `CLI_RULE` expansion in `Makefile`. Follow `CLI_EXTRA_net` for additional objects. A text utility joins `TEXTUTILS` in `tests/textutils.mk`; its rule supports additional guest objects and host sources, as the `grep` and `sed` rules demonstrate.

3. Connect installation to rebuilding. The initial `CLI` list feeds package prerequisites and the disk pack list. A later build fragment needs both an installation entry and a prerequisite on the disk image. `TEXTUTIL_RULE` demonstrates this arrangement. For activation support, follow `c/apps/agent/catalog.json` and `tests/agent.mk`.

4. Define input, output and error behavior. Use the short-write retry loop in `c/apps/coreutils/cat.c` as an output example and the read-error handling in `c/apps/coreutils/net_digest.inc` as an input example. A rich command can follow `c/apps/coreutils/dir.c`, using the shared encoder and a text fallback.

5. Add behavioral checks. Text-tool host cases belong in `tests/textutils/cases.txt`; guest cases belong in `tests/textutils/guest_cases.txt`. When adding a mutation under `TEXTUTIL_NEGCTL`, register the tool in `TEXTUTILS_MUTATED` so the host control requires a failing case for it.

`tests/textutils/run_host.py` executes each host case under the C locale and compares its standard output and exit status with the reference run. The host cases for `diff` include applying generated patches and comparing the reconstructed files with their targets. This checks the effect of a patch while allowing valid edit scripts to differ.

`tests/boot/run-textutils-test.sh` compares output files recovered from the guest disk with files produced by the host builds. Its guest cases include direct comparisons of `diff` output for the selected fixtures.

Use these commands when validating a change:

| Command | Purpose |
|---|---|
| `make test-textutils-host` | Runs host comparisons and their prerequisite negative control. |
| `make test-textutils` | Runs host checks, guest output comparisons and the guest negative control. |
| `make test-mk-wired` | Checks that test fragments are reachable. |
| `make build/disk.img` | Rebuilds the application disk containing installed commands. |

These targets are defined in `tests/textutils.mk` and `Makefile`. The default `make` target builds the kernel ISO; application changes are rebuilt through the disk-image target.

## Limitations

- The shell accepts 4,095 bytes per command, eight pipeline stages and eight tracked jobs. Script files have a 64 KiB limit. Argument budgets come from `include/abi/logit_exec.h`. Conditional command lists and subshells are unsupported. Builtin dispatch applies to a single stage and bypasses the redirection setup used for external commands. See `c/apps/coreutils/sh.c`.
- Regular expressions and `cut -c` operate on bytes. The regex engine rejects backreferences inside patterns, and its capture selection differs from full POSIX subexpression rules. `tail`, `sort` and `diff` retain input in memory. See `c/apps/coreutils/text/lre.h` and the corresponding tool sources.
- `touch` provides file creation without timestamp-setting options. `pref delete` reports an unsupported operation. `pkgverify` provides verification and extraction without package installation policy. See their sources under `c/apps/coreutils/`.
- `pkgverify` reads at most 1 MiB into its package buffer, and `readcore` reads at most 256 KiB. `rec` clamps recording duration to five seconds, and `chart` accepts up to 24 entries. These bounds are defined in the respective command sources.
- `login` starts a root shell when the account store is absent. See `c/apps/coreutils/login.c`.

## See also

- [The C library](libc.md)
- [System services](services.md)
- [Agent runtime](agent-runtime.md)
- [Userland](../architecture/userland.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [Making your first change](../getting-started/first-change.md)
- [Writing tests](../contributing/writing-tests.md)

## Source map

| File or directory | What it contains |
|---|---|
| `c/apps/coreutils/` | Shell, file commands, diagnostics, desktop commands and helper headers. |
| `c/apps/coreutils/text/` | Text utilities, filesystem queries and the shared regex engine. |
| `c/apps/clib.h` | Inline support for small command implementations. |
| `c/apps/logit.h` | Application syscall wrappers. |
| `c/apps/logit_stat.h` | Metadata and directory interfaces. |
| `c/apps/hidden.h` | Directory-entry visibility predicates. |
| `c/apps/crt0_cli.asm` | CLI startup, environment publication and exit handling. |
| `c/apps/crt0_agent.asm` | Activation startup and transfer to the original program startup. |
| `include/abi/logit_exec.h` | Argument counts, string budgets and execution error constants. |
| `include/abi/logit_abi.h` | Shared syscall ABI and metadata attributes. |
| `Makefile` | CLI compilation, linking, AEX packaging and disk installation. |
| `tests/textutils.mk` | Text-tool builds, installation, comparisons and negative controls. |
| `tests/textutils/run_host.py` | Host reference selection and comparison of case output and status. |
| `tests/textutils/cases.txt` | Host comparison cases, including patch application checks for `diff`. |
| `tests/textutils/guest_cases.txt` | Guest comparison commands. |
| `tests/boot/run-textutils-test.sh` | Guest execution and comparison of files recovered from the disk image. |
| `tests/sched.mk` | Build and installation rules for priority tools. |
| `tests/audio.mk` | Build and installation rules for recording. |
| `tests/agent.mk` | Activation link integration and additional command installation. |
| `c/apps/agent/catalog.json` | Application identities and declared actions. |
| `tools/agent_link.py` | Catalogue-driven link additions. |
| `c/lib/agent/activation.c` | Activation dispatch and shared command options. |
| `tools/mkaex.py` | AEX package construction. |
| `c/fs/procfs/procfs.c` | Process, memory and uptime files consumed by inspection commands. |
| `c/crypto/trust/pkgsig.c` | Package signature verification and compiled signer access. |
| `c/crypto/kdf/pbkdf2.c` | Password hashing and checking. |
| `c/apps/gui/terminal/terminal.c` | Rich-frame consumption and Terminal display. |
| `tests/openlogit_sdk.mk` | Graphics compiler frontend builds. |
| `tools/mkopenlogit_sdk.py` | Graphics compiler installation into the sysroot. |