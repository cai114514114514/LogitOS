# Applications and AEX packages

AEX is LogitOS’s native executable container. It wraps an ELF64 image with application metadata, integrity records, and optional signing information. Version 3 adds an agent manifest and an application identity that the kernel binds to the loaded image. The build packs applications into a LogitFS disk image, and the process loader and window manager implement the launch paths.

## Package layout

AEX embeds ELF so applications share the segment validation and loading code in `c/kernel/exec/load/elf.c`. The design rationale in `c/kernel/exec/load/aex.h` names compiler debugging as a benefit: the embedded executable remains readable by ELF inspection tools.

All supported AEX versions use the magic `AEX1`. The version field determines how the remaining bytes are interpreted.

| Version | Interpretation |
|---|---|
| 1 | A 64-byte wrapper followed by ELF |
| 2 | Extended header fields, metadata records, and required ELF CRC-32 |
| 3 | Version 2 layout with a required application ID and agent manifest |

Versions 2 and 3 use this layout, defined in `c/kernel/exec/load/aex.h`:

| Region | Byte range | Contents |
|---|---|---|
| Fixed header | `[0, 64)` | Version, flags, display fields, header length, architecture, ABI, stack hint, and ELF length |
| Metadata | `[64, hdr_size)` | Records containing a tag, length, and value |
| Executable | `[hdr_size, hdr_size + elf_size)` | Embedded ELF64 image |

The fixed header contains a field of 32 bytes for the display name, a field of eight bytes for the extension, an icon glyph and RGB colour, GUI and CLI flags, category, and ordering metadata. Its stack hint is measured in pages; the desktop launcher uses its default when the hint is zero.

The reader in `c/kernel/exec/load/aex.c` accepts bare ELF files, presenting the executable body at offset zero.

## Metadata and validation

Metadata records contain a 32-bit tag, a 32-bit value length, and the value bytes. Each record advances to the next boundary of eight bytes.

| Tag | Value |
|---|---|
| `AEX_T_CRC32` | Four-byte CRC-32 of the embedded ELF |
| `AEX_T_APPID` | Application ID terminated by NUL |
| `AEX_T_TYPES` | Array of 16-bit content type identifiers |
| `AEX_T_SIG` | Ed25519 public key followed by signature |
| `AEX_T_APAD` | Padding used by the packager to align the ELF |
| `AEX_T_AGENT` | Version 3 agent manifest |

For versions 2 and 3, `c/kernel/exec/load/aex.c` validates the architecture, syscall ABI, known flag bits, header bounds, ELF bounds, metadata lengths, and ELF CRC-32. It skips unknown metadata tags and refuses unknown flag bits. This gives descriptive metadata room to grow while preserving checks on executable behaviour.

The total header length must be between 64 bytes and 16 KiB, inclusive, and divisible by eight. `tools/mkaex.py` aligns the embedded ELF to 4096 bytes. For version 2 loads supplied with a page-cache handle, that alignment allows eligible whole pages of segments without write permission to use the page cache through `c/kernel/exec/load/elf.c`.

Version 3 requires exactly one application ID and one agent manifest. The ID contains between one and 63 characters followed by NUL. Accepted characters are lowercase letters, digits, dots, underscores, and hyphens. The parser stores a copy in `struct aex_info`, making the identity available through the memory reader and through the path reader.

The manifest is `struct aex_agent_manifest` in `include/abi/aex_agent.h`. It carries ABI, capability, and state versions; object, action, and context bitmaps; a maximum document size; and a reserved field. The parser checks the supported ABI and bitmap values, requires nonzero capability and state versions, bounds the document size to 1 MiB, and requires the reserved field to be zero.

## Building an application package

`tools/mkaex.py` wraps a linked ELF, computes its CRC-32, adds metadata, and aligns the ELF body. It publishes the completed package through an atomic file replacement, which prevents a concurrent disk-image packer from reading a partially written package. Its `--emit` mode constructs a minimal ELF from code and data blobs before wrapping it.

The application catalogue in `c/apps/agent/catalog.json` records application IDs, installed paths, source locations, and agent interface descriptions. `tools/agent_catalog.py` derives the manifests used by the build. Rules in `tests/agent.mk` route catalogue application links through `tools/agent_link.py`, which retains each target’s existing objects and startup code, adds the activation entry, and selects `_agent_start`.

After linking, the wrapper writes a sidecar containing the application ID, manifest, and ELF SHA-256. `tools/mkaex.py` checks that digest before consuming the sidecar and producing version 3. Its `--agent` option can supply a manifest. Packaging without agent metadata produces version 2; `--v1` selects the legacy wrapper.

At runtime, `_agent_start` in `c/apps/crt0_agent.asm` calls `ag_activation()` in `c/lib/agent/activation.c`. That function selects worker activation using the kernel identity and handles application information options. A regular invocation continues through the application’s original `_start`. For example, the catalogue copy utility supports:

```sh
/bin/cp --aex-capabilities
```

## Capabilities and image identity

`struct proc` in `c/kernel/exec/proc.h` stores the process capability bitmap and a filesystem prefix used in grant comparisons. The capability bits come from `include/abi/logit_abi.h`. The launch path supplies the process grant; the agent manifest describes the application interface.

The classifier in `c/kernel/exec/syscall.c` requires `CAP_FS` for its listed operations that access the filesystem by path, and `CAP_NET` for its listed networking operations. Reading and writing through file descriptors use authority represented by an existing descriptor.

A fork copies the parent’s grant, and executing another image carries that grant forward. `SYS_CAP_SPAWN` creates a child with a requested grant after `proc_cap_subset()` checks its bitmap and prefix against the caller’s grant. The child receives the caller’s working directory and a cloned descriptor table. `SYS_CAP_QUERY` returns the current bitmap and can copy out the stored prefix. These mechanisms are implemented in `c/kernel/exec/proc.c` and `c/kernel/exec/load/exec.c`.

For version 3, the AEX parser hashes the complete container with SHA-256. `proc_agent_bind_image()` in `c/kernel/exec/load/exec.c` binds the application ID and image hash to the PID, parent PID, credentials, activation mode, and a new generation number.

`SYS_AGENT_SPAWN` requires the caller to hold `CAP_PROC` and `CAP_FS`. It captures the requested application, validates version 3, and compares its ID and hash with the request. The worker receives a zero capability bitmap and one authenticated channel endpoint at descriptor 3.

Worker syscalls pass through the additional policy in `include/abi/agent_policy.h`. It permits a defined set of memory, timing, identity, waiting, and synchronisation operations, and restricts channel read, write, close, nonblocking mode, and peer operations to descriptor 3. `c/kernel/exec/syscall.c` evaluates this policy before dispatching the syscall.

## Signing and verification

`tools/mkaex.py` supports signing through `--sign-seed` and `--aexsign-bin`. It passes the ELF bytes to the host tool implemented in `tools/aexsign.c`, which calls the signing code shared with the kernel verifier.

The signature record contains a 32-byte public key followed by a 64-byte Ed25519 signature. Ed25519 signs the SHA-256 digest of this concatenation:

```text
"LOGITOS-aex-v1" || ELF length as eight little-endian bytes || SHA-256(ELF)
```

The domain string is included without its terminating NUL. This construction binds the ELF length and digest while keeping the Ed25519 message at 32 bytes. `c/crypto/trust/aexsig.c` implements the construction and verification.

The loader records these verdicts in `struct aex_info`:

| Verdict | Meaning |
|---|---|
| `AEX_SIG_ABSENT` | Signature record absent |
| `AEX_SIG_INVALID` | Signature verification failed |
| `AEX_SIG_UNTRUSTED` | Signature valid under a key outside the compiled trust set |
| `AEX_SIG_OK` | Signature valid under a compiled trust root |

It records the public key supplied by the signature record and the matching root index for a trusted signature. `c/kernel/exec/load/aex.c` logs the verdict.

`CLAUDE.md` carries the older claim that `c/kernel/exec/load/aex.c` “checks only CRC32”. The current reader also calls `aex_sig_verify()` when a signature record is present.

AEX verification and distribution-package verification share the root accessors in `c/crypto/trust/pkgsig.c`. `tools/genpkgroots.py` generates `c/crypto/trust/pkgroots.inc` from public-key files under `tools/pkgroots/`. The kernel verifier and package verification utility compile those keys into their binaries.

## Distribution envelopes

The `.lpk` format provides a signed envelope around a payload, which can itself be an AEX file. `c/crypto/trust/pkgsig.h` defines a 224-byte fixed header followed by the payload. The header contains an intended filename, payload length, SHA-256 digest, signer public key, and Ed25519 signature.

`lpk_verify()` in `c/crypto/trust/pkgsig.c` validates the header fields, payload bounds, payload digest, signature, and signer trust. The signature authenticates a SHA-256 digest of the `LOGITOS-lpk-v1` domain followed by the 160 header bytes preceding the signature field.

The utility in `c/apps/coreutils/pkgverify.c` exposes this verifier. Its default mode requires a trusted signer; `--any` accepts a valid signature from any key. `--roots` lists the utility’s compiled trust set. The extraction branch verifies the envelope before opening its output.

## Installation and discovery

The standard build installs application files by packing them into LogitFS. `Makefile` defines the mappings from host files to guest paths and passes them to `tools/mkfs.py` when building the disk image:

```sh
make build/disk.img
```

Desktop applications commonly retain `.aex` filenames in the filesystem root. Command-line applications commonly appear under `/bin` without the suffix. Their installed contents retain the AEX container.

`tools/mkfs.py` assigns mode `0755` to files whose destinations end in `.aex` or fall under `/bin/`, `/sbin/`, `/usr/bin/`, or `/usr/sbin/`. `c/kernel/exec/load/exec.c` checks execute permission for `proc_execve()`, capability spawning, and worker spawning.

Desktop discovery uses `scan_apps()` in `c/kernel/gui/wm_dock.c`. It scans root-directory names ending in `.aex`, reads their fixed headers, and registers display names, extension associations, icons, and colours. An `AEX_CAT_TEST` entry keeps its slot and painted tile while hover and click handling skip it. Container integrity and ELF validation run during loading.

## Launch paths

`proc_execve()` in `c/kernel/exec/load/exec.c` copies the path, arguments, and environment into kernel memory, checks execute permission, and builds the replacement image and initial stack in a new address space. After loading and stack construction succeed, it commits the replacement and rewrites the syscall return frame to enter the program.

`include/abi/logit_exec.h` sets limits of 256 entries in each argument and environment vector and 16 KiB of combined string storage, including terminating NUL bytes.

`aex_load_path()` streams version 1, version 2, and bare ELF inputs through the container and ELF readers. For version 3, it copies the file into a memory snapshot, hashes that snapshot, and loads the ELF from those bytes. Version 3 mappings use copied image contents, which keeps the bound hash and loaded executable tied to the captured container.

Desktop launch enters `wm_launch()` and `wm_launch_locked()` in `c/kernel/gui/wm.c`. The launcher reads the application into memory and focuses an existing application with the same display name, or creates an address space and loads through `aex_load_image_ex()`. It supplies the stack, serial standard descriptors, and `CAP_ALL`. Version 3 desktop applications receive the kernel identity binding and an argument stack suitable for their activation entry.

At startup, `wm_run()` selects the greeter when `/etc/passwd` has a positive size and starts the desktop otherwise. Builds selected with `HWHUD=1` use `devssh_desktop_locked()` in `c/kernel/init/devssh.c`. That helper also permits desktop startup for a readable store containing one or more `dev` account rows with password hash `!`, alongside blank or comment lines. This supports hardware setup on a board whose keyboard is unavailable.

The shared ELF loader supplies segment mapping, entry points, and startup information. See [Program loading](../kernel/program-loading.md).

## Limitations

- AEX signature verdicts are advisory: unsigned images, invalid signatures, and untrusted signers proceed to loading. Malformed signature records cause container refusal. The signature covers the ELF; the fixed header and agent manifest lie outside its coverage.
- The compiled trust set contains the development key whose private seed is published in `tools/pkgroots/DEV-SIGNING-KEY.txt`. Anyone with that seed can produce signatures accepted under that root.
- Filesystem prefixes are recorded and compared during spawning but are not enforced against resolved paths. The legacy syscall classifier has no general `CAP_PROC` or `CAP_GUI` checks. `CAP_RAW` enforcement belongs to AetherScript.
- Version 3 path launches and worker images are capped at 16 MiB. Version 3 loading rejects an ELF requesting `PT_INTERP`.
- Version 1 wrappers and bare ELF inputs carry no AEX integrity record.
- Distribution verification provides no dependency resolution, installation scripts, or package version management. `pkgverify` buffers at most 1 MiB. Its current `--extract` parser selects the same operand for input and output, opens output without truncation, and can return success after a write failure leaves extraction incomplete.

## See also

- [Building LogitOS](../getting-started/building.md)
- [Program loading](../kernel/program-loading.md)
- [Security model](security-model.md)
- [Trust roots and package signing](../crypto/trust-and-signing.md)
- [Agent runtime](../userland/agent-runtime.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/load/aex.h` | Fixed header, metadata tags, version rules, and parser results |
| `c/kernel/exec/load/aex.c` | Container validation, CRC checks, signature reporting, and loading |
| `c/kernel/exec/load/elf.c` | ELF validation, segment loading, and page-cache eligibility |
| `c/kernel/exec/load/exec.c` | Execution, capability spawning, worker spawning, and image identity binding |
| `c/kernel/exec/proc.h`, `c/kernel/exec/proc.c` | Process grants, inheritance, subset checks, and grant queries |
| `c/kernel/exec/syscall.c` | Capability classification and worker policy dispatch |
| `include/abi/logit_exec.h` | Argument and environment limits |
| `include/abi/logit_abi.h` | Capability bits and syscall contracts |
| `include/abi/aex_agent.h`, `include/abi/agent_policy.h` | Agent manifest, identity, spawning contract, and worker policy |
| `tools/mkaex.py` | Container generation, alignment, signing integration, and sidecar validation |
| `tools/agent_catalog.py`, `tools/agent_link.py` | Catalogue processing, activation linking, and sidecar generation |
| `c/apps/agent/catalog.json` | Application IDs, installed paths, and agent interface descriptions |
| `c/apps/crt0_agent.asm`, `c/lib/agent/activation.c` | Activation entry and dispatch |
| `c/crypto/trust/aexsig.c`, `c/crypto/trust/aexsig.h` | AEX signature construction and verification |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/pkgsig.h` | Distribution envelopes and shared trust accessors |
| `c/crypto/trust/pkgroots.inc`, `tools/pkgroots/DEV-SIGNING-KEY.txt` | Compiled development root and published signing seed |
| `tools/aexsign.c`, `tools/genpkgroots.py` | Host signing and trust-set generation |
| `c/apps/coreutils/pkgverify.c` | Distribution verification and extraction implementation |
| `Makefile`, `tests/agent.mk`, `tools/mkfs.py` | Build integration, installed paths, and executable modes |
| `c/kernel/gui/wm_dock.c`, `c/kernel/gui/wm.c` | Desktop discovery, session startup, and application launch |
| `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h` | Developer account provisioning and hardware-build desktop startup |
| `CLAUDE.md` | Historical CRC-only claim corrected by the current verifier |
