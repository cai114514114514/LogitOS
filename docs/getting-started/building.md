# Building LogitOS

LogitOS builds an x86-64 kernel, boot loaders and userland programs with Clang, LLD, NASM and Rust. The BIOS ISO contains the kernel and BIOS loaders. A LogitFS disk image contains applications, libraries and runtime files. The build graph in `Makefile` gives these images their own targets, so choose the target that contains the code you changed.

## Host prerequisites

Run build commands from the repository root. The recipes use GNU Make, a POSIX shell, Bash, Python 3 and Unix utilities. Building the userland disk also compiles host programs, so provide the host compiler’s development headers and libraries (`Makefile`, `tests/sysroot.mk`).

| Tool | Role |
|---|---|
| `clang` | Compiles freestanding x86-64 C code and host utilities. |
| `ld.lld` | Links kernel and userland ELF executables. |
| `llvm-ar` | Builds agent archives and the target sysroot’s libraries. |
| `nasm` | Assembles ELF objects and flat BIOS loader binaries. |
| `rustup`, `cargo`, `rustc` | Build the Rust static library used by the kernel and applications. |
| `python3` | Runs configuration tracking, generators and image packers. |
| `lsof` | Checks whether an existing LogitFS image is open before replacement. |
| `qemu-system-x86_64` | Runs the QEMU launch targets. |

Tool defaults are in `Makefile`; archive selection is in `tests/agent.mk` and `tests/sysroot.mk`. Image ownership checks are implemented in `tools/disk_guard.py`.

Install Rust through rustup and add the target:

```sh
rustup target add x86_64-unknown-none
```

The Rust recipe locates Cargo through `rustup which cargo`, selects rustc from the same toolchain and builds with Cargo’s release profile (`Makefile`, `rust/Cargo.toml`).

### macOS

Provide Clang with a working host SDK, LLVM’s ELF linker, NASM, GNU Make, Python 3 and rustup. Guest executables link through `ld.lld`, while host programs link through the host compiler (`Makefile`).

Make LLVM’s tools discoverable through the executable search path or the relevant Make variables. `AGENT_AR` selects the agent archive tool; its default searches for `llvm-ar`, then uses the Apple Silicon Homebrew LLVM location. `SYSROOT_AR` also searches versioned LLVM commands and both Homebrew LLVM locations (`tests/agent.mk`, `tests/sysroot.mk`).

The commands on this page use `make` to mean GNU Make. Existing examples in `docs/BUILDING.md` use `gmake` for a separately installed GNU Make. Automatic parallel scheduling requires GNU Make 4.4 or newer; older versions retain scheduling supplied by the caller (`tools/build/jobs.mk`).

### Linux

Provide Clang, LLD, NASM, Python 3, rustup and native C development headers and libraries. The sysroot build compiles a host TinyCC executable as well as target libraries (`tests/sysroot.mk`, `tests/unit/sysroot_tccsnap.sh`).

The defaults use unversioned `clang` and `ld.lld`. Command-line assignments to `CC` and `LD` select other installations. Use `AGENT_AR` and `SYSROOT_AR` to select the archive tools.

The default BIOS QEMU launch selects CoreAudio. To disable sound on Linux:

```sh
make run QEMU_SND=
```

`QEMU_SND` supplies the sound device arguments for this launch (`Makefile`).

### Windows

Use WSL with the Linux toolchain, following `README.md`. The recipes use Unix shell commands, and LogitFS image locking uses Python’s `fcntl` module (`tools/disk_guard.py`).

## Build targets and images

Build the BIOS ISO:

```sh
make
```

Build the ISO and both userland disk variants:

```sh
make -j12 all release-disk
```

The release image depends on the development image to inherit its build prerequisites. Both images are packed from a shared file specification; release packaging applies exclusions and adds release assets (`Makefile`).

The following commands use the default output directory, `build`.

| Command | Result |
|---|---|
| `make all` | Builds the BIOS ISO, including the kernel and BIOS loaders. |
| `make build/kernel.elf` | Links the kernel and writes its address map. |
| `make build/textedit.aex` | Builds the TextEdit application package from its linked ELF executable. |
| `make build/disk.img` | Builds the development LogitFS image, including userland, runtime files, test programs and fixtures. |
| `make release-disk` | Builds the release LogitFS image with selected test paths excluded. |
| `make sysroot` | Stages target headers, startup objects and libraries for the guest C toolchain. |
| `make esp` | Builds the EFI loader and a FAT boot image containing the loader and kernel. |

Kernel, application and LogitFS image rules are in `Makefile`. TextEdit’s targets come from the application rule macro instantiated there. The sysroot and ESP targets are defined in `tests/sysroot.mk` and `tests/uefi.mk`.

After changing an application, rebuild the disk image used by the next boot. Use `make build/disk.img` for the development disk or `make release-disk` for the release disk. The default `all` goal follows the ISO dependency graph (`Makefile`).

The disk graph also builds host utilities and packaged AetherScript examples through `tests/astyped.mk`.

Both LogitFS variants package Polaris firmware from `third_party/amd-firmware/amdgpu`, along with `third_party/amd-firmware/LICENSE.amdgpu` and `third_party/amd-firmware/MANIFEST`. The firmware reader checks candidates against generated SHA-256 pins (`Makefile`, `c/drivers/gpu/amd/polaris/resource/files.c`).

## Choose the launch target

| Command | Boot image and userland disk |
|---|---|
| `make run` | BIOS ISO and the release disk by default. |
| `make run-dev` | BIOS ISO and the development disk. |
| `make run-uefi` | UEFI ESP and the development disk, using OVMF. |
| `make debug` | BIOS ISO and the development disk, with QEMU halted at startup and a GDB stub enabled. |

`RUN_DISK` selects the disk for `run`. A command-line assignment to `DISK` also makes that disk the default for `RUN_DISK` (`Makefile`).

Earlier instructions in `docs/BUILDING.md` pair a development disk build with `make run`. The current `Makefile` selects the release disk for `run` and provides `run-dev` for development.

For UEFI launches, `OVMF_CODE` selects the firmware code image and `OVMF_VARS_SRC` selects the variables template. The recipe copies the template into the build directory to provide writable guest NVRAM (`tests/uefi.mk`).

See [Running LogitOS in QEMU](running-in-qemu.md) for launch configuration.

## Build directories and options

Pass `BUILD` on Make’s command line to choose an output directory:

```sh
make -j12 BUILD=build-check all release-disk
```

For a filename target, name the output beneath that directory:

```sh
make BUILD=build-check build-check/textedit.aex
```

`Makefile` defines `BUILD := build`. A command-line assignment overrides that definition. Kernel objects, application packages, images, configuration stamps and the staged sysroot follow the selected directory (`Makefile`, `tests/sysroot.mk`).

The development and release disks select packaged content. Compiler rules supply the optimisation settings: kernel C uses `-O2 -g`, userland C uses `-O2 -DNDEBUG`, and Rust uses the release profile. The `debug` target configures QEMU startup (`Makefile`, `rust/Cargo.toml`).

Two diagnostic options are useful during hardware work:

| Option | Effect |
|---|---|
| `HWHUD=1` | Enables the desktop hardware panel. The EFI loader also enables import of an optional developer SSH public-key file from the ESP path `/LOGIT/DEVSSH.PUB`. |
| `TIMENOCH2=1` | Makes PIT channel 2 calibration return failure, allowing the HPET calibration path to be exercised when HPET is available. |

These options are wired in `Makefile` and `tests/uefi.mk`; their implementations are in `c/kernel/diag/hwhud.c`, `c/boot/efi/loader.c` and `c/kernel/core/ktime.c`. Use a fresh build directory when changing `HWHUD`, as described under Limitations.

`UEXTRA` appends diagnostic options to the userland compiler flags. A command-line assignment to `UCFLAGS` replaces that flag variable. Configuration tracking records the resulting values for registered output groups (`Makefile`, `tools/build/product.mk`).

## UEFI image tools

EFI compilation requires `clang` and `lld-link`. The build script emits a PE/COFF EFI application and checks its machine type, subsystem and entry point (`c/boot/efi/build.sh`).

`make esp` produces the default superfloppy layout. The packer selects mtools when `mformat`, `mmd` and `mcopy` are available; otherwise, it uses its internal FAT16 writer (`tests/uefi.mk`, `tools/mkesp.py`).

For custom images, `tools/mkesp.py` accepts:

| Option | Effect |
|---|---|
| `--layout gpt` | Places the FAT volume in an EFI System Partition starting at LBA 2048, with primary and backup GPT metadata. |
| `--size-mb` | Sets the FAT volume size in MiB. GPT wrapping adds 2048 leading sectors and 33 trailing sectors. |
| `--firmware-dir third_party/amd-firmware/amdgpu` | Packages firmware under short names and generates the ESP index `/LOGIT/FW/INDEX.TXT`. |
| `--file SRC:ESP_PATH` | Adds a file; the option can be repeated. |

The EFI loader reads the firmware index and passes the listed files to the kernel as boot modules (`c/boot/efi/loader.c`).

The BIOS product ISO is written by `tools/mkiso.py`. The `iso-grub` comparison target invokes `GRUB_RESCUE`, whose default is `i686-elf-grub-mkrescue` (`Makefile`).

## Font regeneration

Product builds consume checked-in runtime font subsets. To regenerate and verify them:

```sh
python3 -m pip install -r tools/requirements-fonts.txt
make regen-fonts
make verify-fonts
```

These targets require `sha256sum`. They read the vendored source fonts and write or verify the runtime subsets (`Makefile`, `third_party/fonts/README.md`).

## Parallel compilation and caching

On GNU Make 4.4 or newer, `tools/build/jobs.mk` enables automatic parallelism for a top-level default build or an invocation whose goals all belong to `all`, `run`, the ISO target and the development disk target. The default job count is the host CPU count capped at 12. `JOBS` changes that default; supplied job options and inherited jobservers take precedence.

Invocations containing other goals, including `release-disk` and `esp`, retain the caller’s scheduling. Supply `-j12` to request twelve jobs directly.

When `ccache` is available, product C compilation uses it through `COMPILE`. `CCACHE=` disables caching, and `CCACHE_DIR` selects the cache location. The default cache is local to the repository and survives the cleanup targets (`tools/build/cache.mk`).

## Incremental builds

Product C compilation emits dependency files with `-MMD -MP`. These record included project headers and source fragments. If a registered object survives while its dependency file is missing, Make recompiles that object to recover its dependency information (`Makefile`, `tools/build/product.mk`).

Configuration stamps record compiler options and resolved tool metadata. Registered compiler groups associate these stamps with their outputs, so changing an option rebuilds the affected objects. The helper also detects outputs left stale by an interrupted rebuild (`tools/build/config.mk`, `tools/build/config.py`).

The generated Polaris firmware pin header is a prerequisite of the firmware reader object, including its first compilation. `tools/gpu/amd/fwpin.py` derives the header from `third_party/amd-firmware/MANIFEST`. Individual firmware blobs are also disk prerequisites, so rewriting an existing blob triggers repacking (`Makefile`).

Inspect planned recipes with:

```sh
make -n all release-disk
```

LogitFS repacking imports selected user state through a checked private snapshot, then replaces the image atomically. Each image imports state from its own previous contents. `MKFS_PROFILE` preserves the browser, application state, home and download trees, and merges saved documents and configuration with packaged files (`Makefile`, `tools/mkfs.py`, `tools/disk_profile.py`).

## Cleaning

`make clean` removes the selected build directory, including its images and saved guest state. To remove a chosen output tree:

```sh
make BUILD=build-check clean
```

`make clean-scratch` removes entries matching the literal patterns `build-*`, `wbuild` and `refute-*`. Coordinate its use in a shared checkout because those patterns can include another developer’s outputs (`Makefile`).

## Common failures

| Symptom | Cause and next step |
|---|---|
| An application edit is absent after plain `make`. | Rebuild the chosen userland image with `make build/disk.img` or `make release-disk` (`Makefile`). |
| Outputs appear in the default build directory. | Put `BUILD` on Make’s command line and use filename targets beneath that directory (`Makefile`). |
| Cargo is missing or Rust reports a missing target. | Install rustup and add `x86_64-unknown-none`. The recipe selects Cargo and rustc through rustup (`Makefile`). |
| Linking fails through the host’s default linker. | Check `LD`; guest ELF links use `ld.lld` (`Makefile`). |
| Archive creation rejects `-D` on macOS. | Select LLVM’s archive tool through `SYSROOT_AR`. Sysroot archives use deterministic GNU format (`tests/sysroot.mk`, `tools/mksysroot.py`). |
| Packing reports that `lsof` is required. | Install `lsof` to enable the existing image’s ownership check (`tools/disk_guard.py`). |
| Packing reports an open image or a disk held by a VM. | Close the VM using that image, then rebuild (`tools/disk_guard.py`). |
| A runtime font is missing. | Restore the tracked subset or use the regeneration procedure above (`Makefile`). |
| Firmware pin generation reports a missing commit, missing pins or a repeated filename. | Check the commit and digest entries in `third_party/amd-firmware/MANIFEST` (`tools/gpu/amd/fwpin.py`). |
| EFI compilation cannot find `lld-link`. | Provide the PE/COFF linker selected by the EFI build script (`c/boot/efi/build.sh`). |
| UEFI launch reports missing firmware. | Set `OVMF_CODE` and `OVMF_VARS_SRC` to installed firmware files (`tests/uefi.mk`). |
| QEMU rejects CoreAudio on Linux. | Override `QEMU_SND`, or use `QEMU_SND=` to disable sound (`Makefile`). |
| A macOS host link reports `unknown architecture arm64e.x1-macos`. | Inspect the conditional `SDKROOT` workaround for the Command Line Tools SDK pair in `Makefile`. |

The `test` target builds host cryptography and networking prerequisites before invoking its guest boot harness (`Makefile`). Consult [Testing](../architecture/testing.md) for behavioural verification.

## Limitations

- Rust archive outputs named by `RUST_LIB` and `RUST_LIB_HOST` use shared Cargo directories outside `BUILD` (`Makefile`).
- The EFI loader rule does not track changes to `HWHUD`. Use a fresh build directory for a different setting (`tests/uefi.mk`).
- `make esp` passes the loader and kernel without extra files or a layout override. Invoke the packer directly for firmware files or GPT. Extra ESP path components must fit 8.3 short names with either writer (`tests/uefi.mk`, `tools/mkesp.py`).
- `make -n` creates and removes configuration argument files during Make parsing. Configuration stamps are updated by build recipes (`tools/build/config.mk`).
- Disk repacking preserves the roots configured by `MKFS_PROFILE`. Guest files elsewhere can be replaced or omitted during reconstruction (`Makefile`, `tools/disk_profile.py`).

## See also

- [Running LogitOS in QEMU](running-in-qemu.md)
- [Making your first change](first-change.md)
- [Build system](../architecture/build-system.md)
- [Make targets](../reference/make-targets.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Tool defaults, compiler flags, kernel and application rules, disk contents, launch targets and cleanup. |
| `tools/build/jobs.mk` | Automatic job selection and jobserver handling. |
| `tools/build/cache.mk` | Compiler cache selection and configuration. |
| `tools/build/config.mk` | Configuration stamps and checks during parsing. |
| `tools/build/config.py` | Tool metadata, configuration comparison and detection of stale outputs. |
| `tools/build/product.mk` | Product compiler groups and recovery of missing dependency files. |
| `rust/Cargo.toml` | Rust static library configuration and build profiles. |
| `tests/agent.mk` | Agent archive selection and launch dependencies. |
| `tests/astyped.mk` | Native AetherScript runtime and packaged example rules. |
| `tests/sysroot.mk` | Target sysroot construction and archive tool selection. |
| `tests/unit/sysroot_tccsnap.sh` | Host TinyCC compilation and source snapshot selection. |
| `tools/mksysroot.py` | Sysroot staging and deterministic GNU archives. |
| `tests/uefi.mk` | EFI loader, ESP and OVMF launch rules. |
| `c/boot/efi/build.sh` | EFI compilation, linking and PE header checks. |
| `c/boot/efi/loader.c` | Firmware boot modules and optional developer SSH key import. |
| `c/kernel/diag/hwhud.c` | Desktop hardware diagnostic panel. |
| `c/kernel/core/ktime.c` | PIT and HPET calibration paths and the diagnostic hook. |
| `tools/mkiso.py` | BIOS ISO construction. |
| `tools/mkesp.py` | FAT image construction, GPT wrapping and extra files. |
| `tools/gpu/amd/fwpin.py` | Firmware digest header generation from the manifest. |
| `third_party/amd-firmware/MANIFEST` | Firmware source revision and SHA-256 digests. |
| `third_party/amd-firmware/LICENSE.amdgpu` | Firmware redistribution licence. |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading, format checks and digest checks. |
| `tools/mkfs.py` | LogitFS packing, exclusions and preservation options. |
| `tools/disk_guard.py` | Image ownership checks and lifecycle locking. |
| `tools/disk_profile.py` | Preserved state import and atomic image replacement. |
| `tools/requirements-fonts.txt` | Pinned font generation dependency. |
| `third_party/fonts/README.md` | Vendored fonts and regeneration procedure. |
| `README.md` | Host tool overview and WSL guidance. |
| `docs/BUILDING.md` | Earlier build instructions, including GNU Make invocation examples. |