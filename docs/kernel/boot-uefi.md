# UEFI boot path

LogitOS boots through an x86-64 EFI application that reads the kernel from its boot volume, reserves the kernel’s physical memory, collects firmware information and boot modules, and exits boot services. It then enters the kernel in long mode using the native boot ABI in `include/abi/logit_boot.h`. The loader lives in `c/boot/efi/loader.c`; `tests/uefi.mk` connects it to the ESP image builder and QEMU launch targets.

## The EFI application

`tools/mkesp.py` places the application at `/EFI/BOOT/BOOTX64.EFI`, the removable media boot path. Firmware enters `efi_main()`, which initialises COM1, checks the EFI system table and boot services signatures, and requests watchdog disablement.

`c/boot/efi/efi.h` declares the firmware interfaces. Compile time assertions check structure sizes and member offsets: firmware supplies tables of function pointers, so a misplaced member could call the wrong service.

`c/boot/efi/build.sh` compiles the loader with Clang’s `x86_64-unknown-windows` target and links it with `lld-link`. The target supplies the Microsoft x64 calling convention and 16-bit wide characters expected by the firmware interfaces. The link selects `efi_main` as the entry point and the EFI application subsystem. A Python check verifies the resulting PE signatures, x86-64 machine type, PE32+ format, subsystem, and nonzero entry address.

Early diagnostics go to COM1 and the firmware console. COM1 uses 115200 baud, eight data bits, no parity, and one stop bit. The previous page specified 38400 baud; the current divisor is 1, matching the kernel driver in `c/drivers/char/serial.c`. Direct port access keeps serial diagnostics available across firmware exit (`c/boot/efi/loader.c`).

## Finding and loading the kernel

`load_kernel()` follows the loaded image protocol from its own image handle to `DeviceHandle`, then opens the simple file system protocol on that volume. It tries `\logit.elf`, followed by `\LOGIT.ELF`. This makes the selected boot volume determine which kernel is loaded (`c/boot/efi/loader.c`).

The loader checks:

- The ELF signature, ELF64 class, little endian encoding, executable type, and x86-64 machine type.
- A program header size of 56 bytes and a count between one and 64.
- At least one `PT_LOAD` segment with a nonzero memory size.
- Segment file sizes within their memory sizes and physical extents without address overflow.
- A combined physical span within the first 1 GiB and an entry address inside that span.

Each segment’s `p_paddr` supplies its destination. `read_exact()` reads in requests of at most 1 MiB and continues across short reads. Firmware errors or a zero byte read before completion produce a diagnostic and halt the boot.

`linker.ld` places the kernel at 32 MiB and selects `logit_native_start` as the entry point. Its comment preserves the earlier 1 MiB placement and explains the move: the former image span crossed firmware reservations, including ACPI NVS under OVMF. The current base remains within the initial identity mapping.

## Allocation ownership

The loader rounds the combined segment span to page boundaries and requests it with `AllocatePages(AllocateAddress, EfiLoaderData, ...)`. It verifies the returned address and checks that the running EFI image occupies a disjoint range.

A successful allocation is followed by a memory map readback. `efi_load_range_is_loader_data()` in `c/boot/efi/load_policy.h` validates descriptor framing, target alignment, and descriptor extents, then makes two passes:

1. Reject every descriptor of another type that intersects the target.
2. Prove that descriptors cover the complete target.

The first pass catches a reserved descriptor hidden beneath an overlapping loader data descriptor. The coverage pass accepts descriptors in any order.

After ownership is established, `load_kernel()` clears the entire allocation and reads the segment contents into it. Clearing the full span supplies zeroed BSS and clears alignment gaps between segments. Kernel image writes finish before firmware exit (`c/boot/efi/loader.c`).

The same validator checks allocations for the temporary stack and page tables, boot information block, and boot modules. These ranges are checked again against the final memory map on each `ExitBootServices()` attempt. The copied handoff code uses an `EfiLoaderCode` allocation, described below.

## Boot modules

After loading the kernel, `efi_main()` reserves the temporary stack and page tables before collecting modules. This order protects the four pages immediately below the kernel from subsequent allocations that request any available pages (`c/boot/efi/loader.c`).

Modules are collected in this order:

| Input | Module supplied to the kernel |
|---|---|
| `\LOGIT\NOGPU` or `\LOGIT\NOGPU.TXT` | `nogpu`, containing fixed marker text. |
| Files listed in `\LOGIT\FW\INDEX.TXT` | One module per accepted file, using the name from the index. |
| `\LOGIT\DEVSSH.PUB`, with `LOGIT_HWHUD` | `devssh-authorized-keys`. |
| Firmware PCI ROM buffers for display controllers | Modules named by PCI address, using the `vbios-SSSS:BB:DD.F` format with lowercase hexadecimal digits. |

`load_firmware_modules()` parses index lines containing an ESP short filename, decimal byte count, and module name. It checks each file’s size against the index, reads it into `EfiLoaderData` pages, and clears the allocation’s trailing bytes. Malformed lines, files that fail to open, and size mismatches are reported and skipped. Allocation failures, ownership failures, and incomplete reads halt the boot.

The Polaris firmware consumer tries the root filesystem, then a boot module. Both sources pass through firmware format checks and pinned SHA-256 digest checks in `c/drivers/gpu/amd/polaris/resource/files.c`.

The GPU self-test marker receives the first module slot. Its presence supplies the switch, so an empty marker file works. Lookup or module construction failures cause the loader to send `LOGIT_BOOT_TAG_GPU_OPTOUT`. The kernel records that tag as an assumed opt-out, which the Polaris self-test consumer also checks (`c/boot/efi/gpu_optout_marker.c`, `c/kernel/init/bootmod.c`, `c/drivers/gpu/amd/polaris/selftest/opt_out.c`).

With `LOGIT_HWHUD`, the developer key module reaches the provisioner after a successful root filesystem mount. The provisioner validates the supplied Ed25519 public keys before preparing the developer account and SSH files (`c/kernel/init/kmain.c`, `c/kernel/init/devssh.c`).

`c/boot/efi/vbios_rom.c` copies usable `RomImage` buffers published through the EFI PCI I/O protocol. Saving these bytes before firmware exit gives the driver a persistent ROM source and avoids reading through the active framebuffer aperture. ROM collection failures are skipped. The Polaris consumer checks the module for its PCI function and tries ACPI VFCT when needed (`c/drivers/gpu/amd/polaris/resource/vbios.c`).

## Boot information

The native block starts with the packed, 24-byte `struct logit_boot_header`. It carries the magic, version, header size, identity mapping extent, total block size, and reserved field. `LOGIT_BOOT_MAGIC` is `0x4c425436`; `LOGIT_BOOT_VERSION` is 1 (`include/abi/logit_boot.h`).

Tags begin on eight byte boundaries. Each `struct logit_boot_tag` contains its type and size. The loader’s `ib_take()` reserves capacity and clears the tag storage and alignment padding. The information block is allocated below 1 GiB so early kernel code can access it through the initial mapping (`c/boot/efi/loader.c`).

| Tag | Contents |
|---|---|
| `LOGIT_BOOT_TAG_MEMORY_MAP` | Physical start, byte length, and native memory kind for each EFI descriptor. |
| `LOGIT_BOOT_TAG_FRAMEBUFFER` | Framebuffer address, byte pitch, dimensions, pixel size, and colour channel positions. |
| `LOGIT_BOOT_TAG_ACPI_OLD` | A 20-byte RSDP copy. |
| `LOGIT_BOOT_TAG_ACPI_NEW` | An RSDP copy using the accepted length from the ACPI 2.0 configuration table entry. |
| `LOGIT_BOOT_TAG_SMBIOS` | Copied SMBIOS entry point and structure table bytes. |
| `LOGIT_BOOT_TAG_DISPLAY` | Original firmware dimensions, offered mode report, selection reason, mode change failure flag, and EDID bytes. |
| `LOGIT_BOOT_TAG_MODULE` | A module’s 64-bit physical start and end addresses, followed by its NUL-terminated name. |
| `LOGIT_BOOT_TAG_GPU_OPTOUT` | An eight-byte tag carrying the assumed GPU self-test opt-out. |
| `LOGIT_BOOT_TAG_END` | Terminates the tag list. |

Module end addresses point one byte past the payload. Their 64-bit fields accommodate allocations above 4 GiB. The previous page listed module tags as absent; their current wire layout and registry limits are defined in `include/abi/logit_boot.h`.

## Memory map conversion and reservations

The loader walks firmware descriptors using the stride returned by `GetMemoryMap()`. Firmware can return descriptors larger than the declaration in `c/boot/efi/efi.h`.

`boot_memory_type_of()` in `c/boot/efi/loader.c` performs this conversion:

| EFI memory type | Native memory kind |
|---|---|
| Conventional memory, boot services code and data, loader code and data | `LOGIT_BOOT_MEMORY_AVAILABLE` |
| ACPI reclaim memory | `LOGIT_BOOT_MEMORY_ACPI_RECLAIM` |
| ACPI NVS | `LOGIT_BOOT_MEMORY_NVS` |
| Unusable memory | `LOGIT_BOOT_MEMORY_BADRAM` |
| Remaining types, including runtime services and MMIO | `LOGIT_BOOT_MEMORY_RESERVED` |

The kernel reserves allocations it still needs before exposing frames to callers. `pmm_init()` reserves low memory through the kernel end, its allocator metadata, and the normalized boot block. The low reservation also covers the active page tables immediately below the kernel (`c/kernel/mm/phys/pmm.c`).

`kernel_main()` calls `bootmod_init()` before `pmm_init()`. Valid module tags register their memory through `pmm_hold_range()`. Metadata placement avoids those ranges, and the allocator reserves them after releasing available pages in each allocation zone. `bootmod_find()` supplies a module pointer after its RAM mapping is ready (`c/kernel/init/kmain.c`, `c/kernel/init/bootmod.c`).

## Display selection

The earlier policy retained the mode firmware had selected. The current `build_fb_tag()` calls `gop_select_mode()` before describing the framebuffer (`c/boot/efi/loader.c`).

The GOP adapter queries offered modes and obtains EDID from the handle carrying the selected GOP interface. It prefers the active EDID protocol and falls back to the discovered EDID protocol. Matching the GOP interface associates the monitor information with that output (`c/boot/efi/gop_pick_efi.c`).

`gop_pick_mode()` validates the EDID header, version, advertised block coverage, and checksums. It selects among accepted 32-bit modes:

1. Match the preferred progressive detailed timing in the EDID base block.
2. If that timing is usable but its geometry is unavailable, choose the largest mode within 2% of its aspect ratio.
3. Fall back to the largest accepted mode.

This policy addresses the distortion that can occur when firmware offers a larger mode with the wrong aspect ratio (`c/boot/efi/gop_pick.c`).

A failed `SetMode` triggers a request to restore the original mode. The loader then reads the live GOP descriptor, because either call can replace its mode information, framebuffer base, or pitch (`c/boot/efi/gop_pick_efi.c`, `c/boot/efi/loader.c`).

`build_fb_tag()` accepts RGB and BGR layouts and qualifying bitmask layouts. Bitmask checks require three nonoverlapping eight-bit colour channels, disjoint reserved bits, and evidence of storage in the fourth byte. The live descriptor must also provide sufficient framebuffer bytes. The tag records 32-bit pixels and a byte pitch of `PixelsPerScanLine * 4`, preserving firmware scanline padding.

The display report holds up to 48 accepted mode geometries and 256 EDID bytes. `hwhud_capture_boot_info()` retains it for the hardware panel (`include/abi/logit_boot.h`, `c/kernel/diag/hwhud.c`).

In the kernel, `fb_init()` first tries VirtIO GPU initialisation, then consumes the supplied framebuffer tag. For that framebuffer, it requests a mapping with write combining when the PAT configuration supports it, otherwise an uncached mapping, and reports the cache bits read from the page table (`c/kernel/gui/fb/fb.c`).

## ACPI and SMBIOS

`build_acpi_tag()` searches the EFI configuration table, selecting the ACPI 2.0 entry when present and otherwise using ACPI 1.0. It checks the RSDP signature and copies the descriptor into the boot block. For an ACPI 2.0 entry, a reported length between 20 and 4096 bytes determines the copy size; other lengths produce a 20-byte copy (`c/boot/efi/loader.c`).

The kernel validates the copied RSDP before using it. `c/kernel/cpu/acpi/acpi_integrity.c` checks the signature and prefix checksum. Revision 2 or later also requires at least 36 bytes, an accepted length, and the extended checksum. The consumer is in `c/kernel/cpu/acpi/acpi.c`.

`build_smbios_tag()` selects a valid SMBIOS 3 entry point or tries SMBIOS 2. It checks entry signatures, lengths, and checksums, then copies the entry point and up to 64 KiB of structure data. Address checks consult firmware memory descriptors and also accept extents within the legacy firmware area from `0xE0000` to `0x100000`.

Copying preserves SMBIOS data stored in memory available for reuse after firmware exit. `smbios_capture_boot_info()` retains the supplied bytes for subsequent board information parsing (`c/boot/efi/loader.c`, `c/drivers/board/smbios.c`).

## Exiting boot services

`efi_main()` completes its allocations and builds the display, framebuffer, ACPI, SMBIOS, module, and GPU opt-out tags before the final memory map loop. The map buffer includes 8192 bytes of slack because allocations following the sizing request can increase the descriptor count (`c/boot/efi/loader.c`).

Each attempt:

1. Obtains the current memory map and map key.
2. Checks ownership of the kernel, temporary stack and tables, boot block, and every registered boot module.
3. Rebuilds the memory map tag, terminating tag, and total block size.
4. Calls `ExitBootServices()` with that key.

The loader disables firmware console output before the loop. Console drivers can allocate memory and invalidate the map key; diagnostics here use direct COM1 access.

A failed exit causes a fresh map read and one retry. A second failure prints a refusal and halts. On success, the loader clears the interrupt flag and proceeds toward the kernel. When a framebuffer tag was built, two yellow squares mark progress after firmware exit and before entry into the handoff stub.

## Entering the kernel

`setup_native_runway()` reserves four 4 KiB pages immediately below the kernel allocation: a temporary stack page, a PML4, a PDPT, and a page directory. The tables identity map the first 1 GiB with writable, executable 2 MiB leaves (`c/boot/efi/loader.c`).

The function also allocates an `EfiLoaderCode` page below 1 GiB and copies the GDT, its pointer, and `logit_handoff_stub` into it. Earlier documentation required the whole EFI image to fit below 1 GiB. The low copy now keeps the transition instructions and GDT reachable when firmware places the EFI image above that boundary. Using a code allocation allows execution under firmware page protections.

The copied stub switches to the temporary stack, loads the copied GDT and new page tables, reloads segment selectors, and jumps to the ELF entry.

| Entry state | Value |
|---|---|
| CPU mode | x86-64 long mode with four level paging |
| Interrupt flag | Cleared |
| `RAX` | `LOGIT_BOOT_MAGIC` |
| `RDI` | Physical address of `struct logit_boot_header` |
| `CS` | `0x08` |
| `DS`, `ES`, `SS` | `0x10` |
| Stack | Aligned temporary stack below the kernel |

`include/abi/logit_boot.h` defines this contract. `logit_native_start` in `c/boot/long.asm` switches to the kernel’s 32 KiB stack before calling C.

`logit_boot_normalize()` validates the entry magic, header, block extent, identity mapping extent, tag framing, and terminating tag. It also reads the final byte of the promised identity mapping. Refusal paths flush queued serial diagnostics before halting (`c/kernel/init/bootinfo.c`).

Normalization rewrites the memory map, framebuffer, ACPI, and end tag identifiers into the internal layout consumed by existing kernel readers. Payloads remain in place, and the returned view begins 16 bytes into the original block. Extension tags retain their native identifiers. The resulting pointer reaches `kernel_main()` through `c/boot/long.asm`.

The historical loader synthesized Multiboot2 information and passed through 32-bit mode. The retirement note in `c/boot/efi/loader.c` dates that path’s removal to 2026-09-15.

## Building the ESP image

From the repository root:

```sh
make esp
```

`Makefile` includes `tests/uefi.mk`, which connects the EFI application, kernel, and image builder.

| Variable | Default output | Producer |
|---|---|---|
| `BOOTX64_EFI` | `build/efi/BOOTX64.EFI` | `c/boot/efi/build.sh`, with `EFI_OUT` set by the recipe. |
| `KERNEL` | `build/kernel.elf` | Kernel link rule in `Makefile`, using `linker.ld`. |
| `ESP_IMG` | `build/esp.img` | `tools/mkesp.py`. |

`BUILD` defaults to `build`; `EFI_DIR` selects its `efi` subdirectory. Inside the image, the loader occupies `/EFI/BOOT/BOOTX64.EFI` and the kernel occupies `/LOGIT.ELF`.

The image defaults to a superfloppy layout, with the FAT volume starting at the beginning of the image. The earlier description that the builder always omitted a partition table has been superseded by the optional GPT layout (`tools/mkesp.py`).

The builder uses `mformat`, `mmd`, and `mcopy` when all three are available. Otherwise, its Python FAT16 writer constructs the boot sector, two FAT copies, directories, and file data. `MKESP_FORCE_PYFAT=1` selects the Python writer. Automatic sizing accounts for all input file bytes and adds filesystem space, with a minimum volume size of 16 MiB.

| Builder option | Effect |
|---|---|
| `--layout gpt` | Places the FAT volume in one EFI System Partition at LBA 2048, with a protective MBR and primary and backup GPT structures. |
| `--size-mb` | Selects the FAT volume size in MiB. GPT adds 2048 leading and 33 trailing sectors. |
| `--file SRC:ESP_PATH` | Copies an extra host file to an ESP path. The option is repeatable; path components must fit 8.3 names. |
| `--firmware-dir` | Copies sorted `.bin` files as `/LOGIT/FW/FWnn.BIN` and generates `/LOGIT/FW/INDEX.TXT`. |

The generated index records each short filename, byte count, and original firmware name. The vendored Polaris files are in `third_party/amd-firmware/amdgpu`. Both FAT construction paths support extra files and the GPT wrapper (`tools/mkesp.py`).

The `make esp` recipe supplies the loader and kernel arguments. Invoke `tools/mkesp.py` with the corresponding options to select GPT or include firmware and extra files.

For `HWHUD=1`, the EFI recipe adds `-DLOGIT_HWHUD`, enabling developer key collection. Use a distinct `BUILD` directory for this configuration: the recipe’s dependencies track source files, while changing `HWHUD` alone leaves an existing loader target up to date (`tests/uefi.mk`).

`make run-uefi` attaches the ESP through AHCI and the LogitFS data disk through VirtIO block. `OVMF_CODE` and `OVMF_VARS_SRC` select the firmware code and variables template. The `UEFI_VARS` rule creates a writable copy of that template beneath `BUILD` (`tests/uefi.mk`).

## Verification targets

`make test-uefi-load-policy-host` exercises the production ownership validator with constructed descriptor maps. Its negative control is a prerequisite. The runner removes the descriptor type check and requires the fixture to fail with `FAIL: ACPI NVS overlap rejected`. Other fixtures cover holes, overlapping reservations, descriptor stride, alignment, and overflow (`tests/unit/efi_load_policy_test.c`, `tests/unit/efi_load_policy_run.py`).

`make test-uefi-native` invokes the policy checks, protocol controls, firmware comparison, and an OVMF boot that checks for `LOGIT_BOOT_OK`. The controls supply a bad protocol version, a boot block above the initial mapping, and a block left incomplete before firmware exit. Expected diagnostics and retained serial logs are defined in `tests/uefi.mk`.

## Limitations

- Kernel selection uses the fixed filename pair. Configuration menus, fallback kernels, initrd handoff, and kernel signature verification are absent (`c/boot/efi/loader.c`).
- Boot requires allocation of the kernel’s fixed physical span and the four pages below it. The boot block and copied handoff page must fit below 1 GiB (`c/boot/efi/loader.c`).
- Missing GOP, `PixelBltOnly`, unsupported pixel layouts, or an unusable live descriptor produce no framebuffer tag. EDID preference selection uses the progressive detailed timing in the base block (`c/boot/efi/loader.c`, `c/boot/efi/gop_pick.c`).
- The module registry holds at most 32 modules. The firmware index is capped at 4096 bytes, each indexed file at 16 MiB, and the developer key file at 4096 bytes (`include/abi/logit_boot.h`, `c/boot/efi/loader.c`).
- GOP selection and GPU opt-out source comments record model or OVMF evidence, with X79-VG4 hardware verification outstanding (`c/boot/efi/gop_pick_efi.c`, `c/boot/efi/gpu_optout_marker.h`).

## Source map

| File | What it contains |
|---|---|
| `c/boot/efi/loader.c` | Firmware entry, kernel and module loading, tag construction, ownership checks, firmware exit, and handoff code. |
| `c/boot/efi/efi.h` | EFI types, protocol declarations, GUIDs, and layout assertions. |
| `c/boot/efi/load_policy.h` | Allocation coverage and descriptor type validation. |
| `c/boot/efi/gop_pick.c` | EDID validation and mode selection policy. |
| `c/boot/efi/gop_pick_efi.c` | GOP queries, associated EDID lookup, mode changes, and restoration. |
| `c/boot/efi/gpu_optout_marker.c` | Marker lookup and module construction. |
| `c/boot/efi/gpu_optout_marker.h` | Marker paths, module contract, and failure policy. |
| `c/boot/efi/vbios_rom.c` | Display controller PCI ROM collection and copying. |
| `c/boot/efi/build.sh` | EFI compilation, linking, and PE checks. |
| `include/abi/logit_boot.h` | Native boot ABI, tag layouts, limits, and entry contract. |
| `linker.ld` | Kernel placement and entry selection. |
| `c/boot/long.asm` | Kernel stack setup, normalization call, and C entry. |
| `c/kernel/init/bootinfo.c` | Native block validation and normalization. |
| `c/kernel/init/bootmod.c` | Module validation, registry, held ranges, and assumed GPU opt-out. |
| `c/kernel/init/devssh.c` | Developer key validation and account provisioning. |
| `c/kernel/init/kmain.c` | Ordering of boot information consumers and provisioning. |
| `c/kernel/mm/phys/pmm.c` | Memory map consumption, metadata placement, reservations, and physical mappings. |
| `c/kernel/gui/fb/fb.c` | Display selection and framebuffer mapping. |
| `c/kernel/diag/hwhud.c` | Display report capture and hardware panel. |
| `c/kernel/cpu/acpi/acpi.c` | Copied RSDP consumption and ACPI discovery. |
| `c/kernel/cpu/acpi/acpi_integrity.c` | RSDP and ACPI table integrity checks. |
| `c/drivers/board/smbios.c` | SMBIOS copy capture and parsing. |
| `c/drivers/char/serial.c` | Kernel COM1 configuration. |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware source selection, format validation, and digest checks. |
| `c/drivers/gpu/amd/polaris/resource/vbios.c` | VBIOS module validation and VFCT fallback. |
| `c/drivers/gpu/amd/polaris/selftest/opt_out.c` | GPU self-test opt-out consumption. |
| `tools/mkesp.py` | FAT construction, GPT wrapping, firmware index generation, and extra files. |
| `tests/uefi.mk` | ESP recipes, QEMU launch, and UEFI verification targets. |
| `tests/unit/efi_load_policy_test.c` | Allocation policy fixtures. |
| `tests/unit/efi_load_policy_run.py` | Fixture compilation and mutation control. |
| `Makefile` | Kernel build and inclusion of the UEFI rules. |

## See also

- [Building LogitOS](../getting-started/building.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
- [Running on real hardware](../getting-started/real-hardware.md)
- [BIOS boot path](boot-bios.md)
- [Kernel initialisation](init.md)
- [Physical memory](physical-memory.md)
- [ACPI](acpi.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)