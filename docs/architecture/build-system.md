# Build system

LogitOS uses GNU Make to coordinate compilation, generated interfaces, executable packaging, and image construction. The root `Makefile` defines the product dependency graph; included fragments add programs, assets, and tests. BIOS and UEFI images carry a loader and kernel, while LogitFS images carry applications, libraries, configuration, and guest data.

## Main artifacts

`Makefile` defines the main output variables. `tests/uefi.mk` adds the UEFI image.

| Variable | Request | Contents |
|---|---|---|
| `KERNEL` | Prerequisite of boot images | Kernel ELF linked from C, assembly, and Rust |
| `ISO` | `make` | BIOS preload, BIOS loader, and kernel |
| `DISK` | `make build/disk.img` with the default `BUILD` | Development filesystem, including applications and fixtures |
| `RELEASE_DISK` | `make release-disk` | Filesystem assembled with release exclusions and assets |
| `ESP_IMG` | `make esp` | FAT volume containing the UEFI loader and kernel |

The default goal is `all`, whose prerequisite is `ISO`. Application compilation and packaging feed the filesystem image through `DISK`.

`make run` builds the ISO and selects `RELEASE_DISK` by default. `make run-dev` selects `DISK`. A command-line assignment to `DISK` also supplies the default for `RUN_DISK`; callers can override `RUN_DISK` directly.

Older guidance in `CLAUDE.md` describes the development disk as the interactive default. The current `run` rules in `Makefile` select the release image.

## Makefile structure

The root `Makefile` establishes tools, output locations, compiler flags, and source inventories, then defines compilation, linking, packaging, and image rules. Included fragments extend this graph.

| File | Responsibility |
|---|---|
| `Makefile` | Source inventories, object rules, executable packaging, image recipes, and fragment inclusion |
| `tools/build/jobs.mk` | Automatic parallelism for selected product goals |
| `tools/build/cache.mk` | Compiler-cache discovery and compilation wrapper |
| `tools/build/config.mk` | Configuration groups and rebuild dependencies |
| `tools/build/config.py` | Configuration comparison and stamp updates |
| `tools/build/product.mk` | Product configuration registration and recovery of missing dependency files |
| `tools/build/variants.mk` | Configuration dependencies for options affecting selected objects |
| `tools/build/extras.mk` | Additional program, SDK, and sysroot configuration groups |

Include order affects dependency correctness. Make expands a rule’s prerequisites when reading the rule and expands its recipe variables when executing the recipe. The recursively expanded `DISK_PACK` list incorporates package additions from fragments included later. Each fragment attaches its file prerequisites beside its own definitions.

`tests/sysroot.mk` illustrates this arrangement: it defines the sysroot staging stamp and adds that file as a prerequisite of `DISK`. Using a file stamp gives Make a timestamp for deciding when staging and image construction need work.

Scripts that extract Make rules must join backslash continuations before parsing assignments or prerequisites. `tools/mk_wired.py` follows this rule when examining fragment includes.

## Compilation and linking

`Makefile` selects Clang, LLD, and NASM through `CC`, `LD`, and `ASM`. Kernel and userland C flags target `x86_64-elf`, use freestanding compilation, enable SSE/SSE2, and disable the red zone.

`CFLAGS` includes debug information and preserves frame pointers for kernel backtraces. `UCFLAGS` defines `NDEBUG` and places the userland header directory for conflicting names ahead of the shared include inventory.

`INCDIRS` derives a sorted directory list and filters header namespaces that need qualified paths. The adjacent comments describe failures caused by colliding header names and differences in filesystem traversal order. Include-path ordering is part of the compilation contract.

`C_SRC` recursively discovers C sources beneath the kernel, driver, library, filesystem, networking, and cryptography directories. Filters assign selected libraries and networking code to userland. The uACPI interpreter sources are added from `third_party/uacpi/source`; `ASM_SRC` selects assembly directly beneath `c/boot`.

The interpreter and power-driver objects receive `UACPI_CFLAGS`. `Makefile` also applies those flags to three AML consumers:

- `c/kernel/cpu/power/firmware.c`
- `c/drivers/board/nct_acpi.c`
- `c/kernel/pci/pci_prt_aml.c`

The kernel link combines C and assembly objects with `RUST_LIB`, uses `linker.ld`, and emits a linker map. The Rust recipe invokes Cargo for `x86_64-unknown-none`. `rust/Cargo.toml` defines the static library and its release profile.

Applications compile to objects, link into ELF executables, and pass through `tools/mkaex.py` for AEX packaging. `APP_RULE` supplies this sequence for several GUI programs; `CLI_RULE` supplies it for command-line programs. Larger applications maintain their own object inventories and link rules. The graphics SDK and AetherScript participate through their owning inventories and fragments.

## Configuration and variants

Make variables select tools, output locations, compiler options, and package inputs. Set `BUILD` on Make’s command line to choose another output directory. Command-line assignments take precedence over `BUILD := build` in `Makefile`.

| Variable | Effect |
|---|---|
| `CC`, `LD`, `ASM` | Select compilation, linking, and assembly tools |
| `HWHUD=1` | Adds `LOGIT_HWHUD` for the hardware panel and developer access support |
| `TIMENOCH2=1` | Adds `TIME_TEST_NO_CH2`, causing PIT channel 2 calibration to fail |
| `FPO=1` | Adds frame-pointer omission for the kernel backtrace negative control |
| `UEXTRA` | Appends options to `UCFLAGS` |
| `CODEC_OPT` | Adds optimization options to selected codec objects; defaults to `-O3` |
| `BROWSER_AEX` | Selects the browser package used by disk packaging |
| `GALLERY_AEX` | Supplies an optional Gallery package |
| `SYSROOT_MKFLAGS` | Supplies options to sysroot construction |

These settings are implemented in `Makefile` and `tests/sysroot.mk`. The hardware panel lives in `c/kernel/diag/hwhud.c`; the calibration control in `c/kernel/core/ktime.c` exercises calibration through HPET when that device is available.

For `HWHUD=1`, `tests/uefi.mk` also passes `LOGIT_HWHUD` to the loader built for `esp`. The loader can read `/LOGIT/DEVSSH.PUB` into a boot module consumed by the developer access support in `c/kernel/init/devssh.c`.

Configuration tracking supplements source timestamps. `tools/build/config.mk` registers groups containing a tool command, selected flag values, and affected outputs. `tools/build/config.py` records the command and metadata for resolved tool executables, including their paths, sizes, and modification and change times.

Matching stamp contents retain their timestamp. Changed contents are replaced atomically, and affected outputs gain rebuild dependencies. The helper compares output timestamps at nanosecond precision so unfinished rebuilds remain detectable with Make versions that compare whole seconds.

`tools/build/product.mk` registers compiler domains, kernel assembly, and the kernel link. `tools/build/variants.mk` adds dependencies for options applied to selected objects through target-specific assignments. Tracking both enabled and disabled values lets a later build replace objects compiled with a control option.

## Incremental builds, jobs, and caching

The main C flags include `-MMD -MP`, producing dependency fragments beside objects. `Makefile` includes regular files ending in `.d` beneath `BUILD`, connecting included headers to their consuming objects.

Each object needs a compilation recipe for these dependencies to invoke. The comments beside `APP_RULE` preserve the earlier failure: compilation inside an ELF link recipe produced dependency information naming an object that had no compilation recipe. `tools/build/product.mk` also forces registered C objects through their recipes when their dependency files are missing.

`tools/build/jobs.mk` enables automatic parallelism for top-level invocations whose goals are drawn from `all`, `run`, `ISO`, and `DISK`, including an invocation with no named goal. This requires Make’s `shell-export` feature. The default `JOBS` value is the host CPU count capped at twelve. Existing job options and inherited jobservers retain scheduling control.

C recipes using `COMPILE` receive the wrapper configured by `tools/build/cache.mk`. It discovers `ccache`; an empty `CCACHE` value disables the wrapper. `CCACHE_DIR` selects the cache location.

`make clean` removes the selected `BUILD` directory. `make clean-scratch` removes entries matching `build-*`, `wbuild`, and `refute-*`, as defined in `Makefile`.

## Generated interfaces and data

The build uses generated prerequisites alongside maintained generated files. Their generators connect consumers to shared definitions and data.

| Generator | Inputs and outputs |
|---|---|
| `tools/gen_boot_inc.py` | Extracts integer constants from `include/abi/logit_boot.h` into the NASM include identified by `PRODUCT_BIOS_ABI_INC` |
| `tools/gen_abi.py` | Reads `include/abi/logit_abi.h` and `include/abi/logit_calls.abi`; emits ABI bindings, C layout assertions, and `include/abi/logit_pack.h` |
| `tools/gpu/amd/fwpin.py` | Reads `third_party/amd-firmware/MANIFEST`; emits the digest header identified by `POLARIS_FW_PIN_H` |
| `tools/genroots.py` | Extracts trust-anchor public keys from PEM inputs under `tools/roots` into `c/crypto/trust/roots_bundle.inc` |
| `tools/genpkgroots.py` | Converts signer keys under `tools/pkgroots` into `c/crypto/trust/pkgroots.inc` |

The BIOS include rule depends on the boot header, generator, and `Makefile`. The loader depends on that include, propagating changes in constants or extraction logic through assembly to the ISO.

The Polaris resource object depends on `POLARIS_FW_PIN_H` before its first compilation and receives the generated header directory in its include path. `c/drivers/gpu/amd/polaris/resource/files.c` uses those SHA-256 digests to validate firmware obtained from the root filesystem or boot modules. Generating the table from the manifest ties validation to the record of the vendored files.

For maintained ABI outputs, `make check-abi` regenerates and compares their contents. `python3 tools/gen_abi.py --write` updates them. The root `Makefile` also gives trust consumers dependencies on their generated includes.

Runtime font subsets are checked in under `fsroot/fonts`. `make regen-fonts` verifies the vendored source checksums, invokes `tools/mkfont.py`, and checks the output checksums.

## Boot image construction

The BIOS rules assemble `c/boot/bios/preload.asm` and `c/boot/bios/loader.asm` as flat binaries under `PRODUCT_BIOS_DIR`. The loader consumes the generated boot constants.

`tools/mkiso.py` places the preload, loader, and kernel in an ISO9660 image with an El Torito BIOS boot entry. It patches the loader location into the preload and the kernel location into the loader, giving each stage the location of its next payload.

`make iso-grub` builds the GRUB oracle image. Its kernel link includes boot objects from `tests/fixtures/bootoracle`, and its image recipe invokes the configured GRUB rescue tool.

For UEFI, `tests/uefi.mk` invokes `c/boot/efi/build.sh`. The script compiles the loader for the Windows x86-64 target and links an EFI application with `lld-link`. `tools/mkesp.py` installs the loader and kernel on a FAT volume, using mtools when available or its Python FAT16 writer otherwise.

The default layout is a superfloppy. `--layout gpt` wraps the FAT volume in an ESP beginning at LBA 2048, with a protective MBR and primary and backup GPT structures. `--size-mb` sizes the FAT volume; the GPT layout adds 2,048 leading and 33 trailing sectors.

The writer accepts extra files through `--file`, validating each destination component as an 8.3 name. `--firmware-dir` packages sorted `.bin` files under `/LOGIT/FW` with short stored names and an `INDEX.TXT` map containing stored names, byte counts, and firmware names.

`c/boot/efi/loader.c` reads that index and loads its listed files before leaving UEFI boot services. It describes them with module tags from `include/abi/logit_boot.h`. `c/kernel/init/bootmod.c` validates the tags, reserves the module pages, and registers the files for kernel consumers.

## Application disk construction

`tools/mkfs.py` constructs a LogitFS v4 image from host-to-guest path specifications. A specification installs one file or recursively installs a directory tree. Tree traversal sorts entries and validates destination lengths.

`Makefile` divides package descriptions and build dependencies across several variables:

| Variable | Purpose |
|---|---|
| `FS_SPECS` | Base filesystem inputs and destination mappings |
| `FS_BASE_HOSTS` | Host paths derived from those mappings for prerequisites |
| `FS_FILES` | Packaging inputs extended by fragments |
| `ROOT_AEX_PACK` | Application packages installed at the filesystem root |
| `DISK_PACK` | Shared package list for development and release images |
| `DEV_ONLY_PACK` | Additions to the development image |
| `RELEASE_EXCLUDE` | Destination patterns filtered during release packaging |

`FS_BASE_HOSTS` derives host paths from `FS_SPECS` because colons have syntactic meaning in Make prerequisite lists. The package mappings remain authoritative while the dependency graph receives usable host paths.

Both images install `third_party/amd-firmware/amdgpu` at `/lib/firmware/amdgpu`, with its license and manifest under `/licenses/amd-firmware`. `AMD_FIRMWARE_FILES` adds the firmware binaries as prerequisites of `DISK`, so rewriting a blob triggers repacking even when the directory timestamp stays the same.

`tests/sysroot.mk` stages headers, archives, and startup objects through `tools/mksysroot.py`. Its staging stamp connects those inputs to disk construction.

Both image recipes pass `MKFS_PROFILE`. It preserves `/browser`, `/state`, `/home`, and `/download`, and merges existing `/docs` and `/etc` contents with packaged defaults. During a merge, existing file bytes and inode metadata take precedence, while newly packaged siblings remain available. `tools/disk_profile.py` implements this policy.

Before importing preserved content, `tools/lfs_snapshot.c` recovers the journal and checks a private filesystem snapshot. The writer creates a temporary image, flushes it, and replaces the destination atomically. `tools/disk_guard.py` coordinates builders and cooperating launchers through a lock, with `lsof` checks for open images.

The geometry in `tools/mkfs.py` is 512 MiB with 8,192 inodes. Construction writes the allocated prefix and extends the file to its logical size, leaving a sparse free tail.

The release image depends on the development image to inherit dependencies attached by subsystem fragments. Its recipe repacks `DISK_PACK`, applies `RELEASE_EXCLUDE`, and adds release assets. Every exclusion must match packaged content, making renamed inputs visible as packaging errors.

## Adding a component

1. Choose its compilation domain. A kernel source beneath the discovered directories enters `C_SRC` unless a filter excludes it. Userland sources need an application or library inventory. Update the kernel filter when adding userland code beneath a scanned directory; `RING3_NET` provides an example.

2. Give each compiled object a target. Use `APP_RULE` or `CLI_RULE` when their source layout and dependencies fit. The matching `APPS` or `CLI` inventory connects these programs to packaging. Larger components can follow the existing object, ELF, and AEX rules.

3. Register configuration dependencies. Extend the owning group in `tools/build/product.mk` or `tools/build/extras.mk`. Use `product_c` for a new C compiler domain. Options applied to selected objects need corresponding dependencies in `tools/build/variants.mk`, including transitions back to their default value.

4. Connect packaging and prerequisites. Add the installation mapping to the owning package list and attach its host inputs to the image target. Follow `tests/sysroot.mk` when a fragment owns the inputs. Adding dependencies beside their definitions avoids include-order errors and lets the release image inherit development-image dependencies.

5. Connect tests to the include graph. `make test-mk-wired` checks fragment reachability through `tools/mk_wired.py`. Use literal test-fragment paths so the checker can follow them. Make a negative control a prerequisite of its positive gate, following `tests/build_config.mk` and `tests/build_incremental.mk`.

The build has focused gates named `test-build-incremental`, `test-build-config`, `test-build-jobs`, and `test-build-dir`. Their owning fragments connect controls to checks for dependency behavior, configuration changes, scheduling, and fresh output directories.

## Limitations

- `BUILD` relocates many product outputs, but `RUST_LIB` retains a fixed Cargo output location. Builds with different `BUILD` values share that archive.
- Configuration tracking covers registered groups and values. The ACPI group in `tools/build/product.mk` tracks interpreter and power-driver objects; the three additional AML consumers listed above fall outside its `UACPI_CFLAGS` output list.
- Changing `HWHUD` within an existing output directory can reuse a UEFI loader built with the previous setting. Use a distinct `BUILD` directory for that variant.
- The `esp` recipe passes loader and kernel inputs. Extra files, firmware, and GPT layout selection require invoking `tools/mkesp.py` with the corresponding options.
- `tools/mkfs.py` rejects symlink inputs. Release exclusions apply to packaged inputs before preserved guest state is imported.
- `TRANSPARENCY.md` records outstanding release reproducibility requirements. Fresh LogitFS timestamps use the current time unless `SOURCE_DATE_EPOCH` is supplied.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Product graph, compiler flags, source inventories, application rules, and image recipes |
| `linker.ld` | Kernel entry point and section layout |
| `rust/Cargo.toml` | Rust static-library definition and build profiles |
| `tools/build/jobs.mk`, `tools/build/cache.mk` | Job scheduling and compiler-cache configuration |
| `tools/build/config.mk`, `tools/build/config.py` | Configuration dependencies, tool identity, and stamp handling |
| `tools/build/product.mk` | Product compiler groups and recovery of missing dependency files |
| `tools/build/variants.mk`, `tools/build/extras.mk` | Variant dependencies and additional product groups |
| `c/kernel/cpu/power/firmware.c`, `c/drivers/board/nct_acpi.c`, `c/kernel/pci/pci_prt_aml.c` | Additional AML consumers receiving uACPI flags |
| `c/kernel/diag/hwhud.c`, `c/kernel/core/ktime.c` | Hardware panel and calibration control |
| `tools/gen_boot_inc.py`, `include/abi/logit_boot.h` | Boot constants shared with NASM and boot-module definitions |
| `tools/gen_abi.py` | ABI generation and consistency checks |
| `tools/gpu/amd/fwpin.py`, `third_party/amd-firmware/MANIFEST` | Firmware digest generation and provenance records |
| `third_party/amd-firmware/LICENSE.amdgpu` | Firmware redistribution terms |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading and digest validation |
| `tools/genroots.py`, `tools/genpkgroots.py` | Compiled trust-input generation |
| `tools/mkfont.py`, `tools/mkaex.py` | Font generation and executable packaging |
| `tools/mkiso.py`, `tools/mkesp.py` | BIOS ISO and UEFI FAT image construction |
| `tests/uefi.mk`, `c/boot/efi/build.sh` | UEFI build rules and loader compilation |
| `c/boot/efi/loader.c`, `c/kernel/init/bootmod.c` | ESP file loading and kernel module registration |
| `c/kernel/init/devssh.c` | Developer access provisioning from a boot module |
| `tools/mkfs.py` | LogitFS construction and package specifications |
| `tools/disk_profile.py`, `tools/disk_guard.py` | State preservation, atomic replacement, and image ownership checks |
| `tools/lfs_snapshot.c` | Journal recovery and filesystem validation for preserved state |
| `tests/sysroot.mk`, `tools/mksysroot.py` | Sysroot dependencies and staging |
| `tests/build_incremental.mk`, `tests/build_config.mk` | Dependency and configuration gates |
| `tests/build_jobs.mk`, `tests/build_dir.mk` | Scheduling and output-directory gates |
| `tools/mk_wired.py` | Test-fragment reachability audit |
| `TRANSPARENCY.md` | Claim rules and release reproducibility requirements |

## See also

- [Building LogitOS](../getting-started/building.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
- [Boot sequence](boot-sequence.md)
- [Applications and AEX packages](applications-aex.md)
- [Make targets](../reference/make-targets.md)
- [Writing tests](../contributing/writing-tests.md)