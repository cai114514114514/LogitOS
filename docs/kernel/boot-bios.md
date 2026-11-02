# BIOS boot path

LogitOS boots from a BIOS El Torito CD image through a preload at physical address `0x7c00` and a second stage loader at `0x10000`. The loader gathers firmware information, loads the kernel’s ELF segments, creates the initial page tables and transfers control in 64-bit long mode. `c/boot/bios/preload.asm` and `c/boot/bios/loader.asm` implement the stages; `tools/mkiso.py` places their payloads in the ISO and patches their disk locations.

## Image layout

`Makefile` assembles both BIOS stages as flat binaries and passes them with the kernel ELF to `tools/mkiso.py`. The writer creates an ISO9660 image with one BIOS El Torito catalog entry in no emulation mode.

| Component | Location | Purpose |
|---|---|---|
| Boot catalog | ISO LBA 49 | Describe the firmware preload |
| Preload | ISO LBA 217, loaded at `0x7c00` | Read and enter the second stage |
| Second stage loader | Immediately after the padded preload, loaded at `0x10000` | Discover firmware data and load the kernel |
| Kernel ELF | Immediately after the padded loader | Supply segment locations and the entry address |

The preload occupies one CD block of 2,048 bytes. Its catalog entry requests four 512-byte units at segment `0x07c0`. Subsequent BIOS disk reads use native CD blocks of 2,048 bytes. The mix of units explains the differing counts in the boot catalog and the loaders.

Both payload records have the same ten-byte layout:

| Field | Encoding |
|---|---|
| Marker | Four bytes, `L2P!` |
| Payload LBA | Little-endian 32-bit value in native CD blocks |
| Payload block count | Little-endian 16-bit value |

The preload’s loader record starts at byte offset `0x1f0`; the loader’s kernel record starts at `0x1ff0`. `tools/mkiso.py` requires exactly one marker at each expected offset before patching. This check catches assembly layout changes that could turn a payload-location update into an overwrite of executable bytes.

## Preload entry

The first instruction in `c/boot/bios/preload.asm` jumps to `0000:entry`. Firmware can express physical address `0x7c00` as either `0000:7c00` or `07c0:0000`. Setting the code segment to zero makes absolute labels agree with the assembly origin.

The preload sets `DS` and `SS` to zero, places the stack pointer at `0x7c00`, enables interrupts, clears the direction flag and saves the boot drive supplied in `DL`. It configures COM1 for 115,200 baud, eight data bits, no parity and one stop bit, then prints `LOGIT_BIOS_PRELOAD_OK`.

The previous page specified 38,400 baud. Both BIOS stages now program divisor 1, matching the setting in `c/drivers/char/serial.c`.

The preload checks BIOS extended disk services with interrupt `0x13`, function `0x41`, then reads the loader with function `0x42`. The disk address packet supplies the patched LBA, block count and destination `1000:0000`, physical address `0x10000`. On success, the preload jumps there. Failure of the extension check or the read prints a diagnostic and enters a halt loop with interrupts disabled.

## Firmware discovery

The second stage starts in 16-bit real mode. `loader_entry` sets its data segment to its code segment, establishes a stack with its top at `0x7c00`, saves the boot drive and initializes serial output. It then verifies A20, collects the E820 map, searches for ACPI information, probes VBE and constructs the boot block. These routines are in `c/boot/bios/loader.asm`.

### A20

`a20_is_enabled` tests whether physical address `0x100000` aliases address zero. With interrupts disabled, it saves the four bytes at each address, writes a test value at `0x100000`, checks address zero and restores both locations.

If the test detects wrapping, the loader tries the BIOS A20 service, the keyboard controller and port `0x92`, testing again after each method. Keyboard controller waits have bounded iteration counts so execution can reach the next method. Verification matters because firmware can report success while wrapping remains active, causing later copies above 1 MiB to corrupt memory. Failure after all methods produces a diagnostic and halts.

### E820 memory map

`collect_e820` calls BIOS interrupt `0x15`, function `0xe820`, following the firmware continuation value. It checks the returned signature and accepts descriptors of at least 20 bytes.

The loader retains descriptor order, base addresses, lengths and memory types, including reserved regions and entries of zero length. It stores each descriptor with a stride of 24 bytes and clears the trailing reserved field. `include/abi/logit_boot.h` defines this layout as `struct logit_boot_mmap_entry`.

The physical memory manager in `c/kernel/mm/phys/pmm.c` interprets the map and decides which ranges become allocatable. Both boot paths feed that kernel policy.

### ACPI and framebuffer information

`find_rsdp` searches the first KiB of the Extended BIOS Data Area, followed by physical addresses `0xe0000` through `0xfffff`, at boundaries of 16 bytes. It checks the legacy RSDP checksum and, for revision 2 or later, the extended length and checksum. The block builder copies an accepted RSDP into an ACPI tag.

`probe_vbe` searches for a supported graphics mode with a linear framebuffer, 1024 × 768 pixels, 32 bits per pixel and direct colour. After setting the mode, it queries the mode information again. A successful probe supplies the framebuffer address, pitch, dimensions and colour masks for a framebuffer tag. ACPI and framebuffer tags are conditional on successful discovery.

## Boot-information block

`build_boot_info` writes the native boot block at physical address `0x8000`. Its 24-byte `struct logit_boot_header` contains:

| Field | Purpose |
|---|---|
| Magic and version | Identify the native protocol |
| Header size | Locate the first tag |
| Identity-map extent | State the mapping supplied at entry |
| Total size | Bound the complete block |
| Reserved field | Written as zero |

A memory map tag follows the header; discovered ACPI and framebuffer tags come next, followed by an end tag. Tag starts are aligned to eight bytes.

`include/abi/logit_boot.h` defines the wire constants, offsets and packed structures. `tools/gen_boot_inc.py` generates the NASM definitions consumed by the loader, while assertions at compile time in the C header check structure sizes and offsets. This gives assembly writes and C reads a shared layout definition.

## Early physical memory layout

The loader uses fixed addresses for firmware buffers and transition state. Range ends below are exclusive.

| Physical address or range | Use |
|---|---|
| `0x3000`–`0x5000` | Saved ELF program-header table |
| `0x5000` | E820 descriptor buffer |
| `0x6000` | VBE controller-information buffer |
| `0x6200` | VBE mode-information buffer |
| `0x7c00` | Preload address and temporary stack top |
| `0x8000`–`0xa000` | Boot-information storage |
| `0x10000` | Second-stage loader |
| `0x70000`–`0x78000` | Disk-read bounce buffer |
| `0x01ffc000`–`0x01ffd000` | Extra page-table workspace for a negative control |
| `0x01ffd000` | PML4 |
| `0x01ffe000` | Page-directory-pointer table |
| `0x01fff000` | Page directory |
| `0x02000000` | Kernel load floor |

`protected_native_entry` clears all four pages of the paging workspace. The three active tables sit immediately below the kernel load floor, within the region that `pmm_init` reserves from address zero through the kernel image.

`linker.ld` places the kernel at 32 MiB. Its comment records the former placement at 1 MiB and the reason for moving it: the old image span crossed firmware workspace on UEFI machines. Both loaders consume the resulting ELF layout.

## Loading the kernel ELF

`read_kernel_headers` reads the first CD block into the bounce buffer and checks the ELF magic, 64-bit class, little-endian encoding, identification version and x86-64 machine identifier. It obtains the entry address and program-header layout from the ELF, reads enough blocks to cover the table and copies the table to `0x3000`. Later segment reads reuse the bounce buffer.

For each `PT_LOAD` entry, `load_kernel_and_enter` reads the file offset, physical destination, file size and memory size. It checks that memory size covers file size, the destination meets the kernel load floor, address calculations stay within 32 bits and file data fits within the padded extent of the patched payload. Successful loading requires at least one loadable segment.

`load_segment_file` transfers file data in chunks that fit the 32 KiB bounce buffer. It accounts for offsets within a CD block, allowing segment boundaries to fall between block boundaries. After each read, `protected_memory_operation` enters 32-bit protected mode, copies the bytes to their physical destination and returns to real mode. The same mechanism zeroes the memory beyond each segment’s file data, including BSS.

The repeated transitions establish the segment limits needed to reach the kernel at 32 MiB after each BIOS read. The source records the reason: BIOS calls may change hidden segment state, so keeping an unreal mode segment cached is unsafe.

## Entering long mode

After segment loading, `load_kernel_and_enter` disables interrupts and enters 32-bit protected mode. `protected_native_entry` then:

1. Clears the paging workspace and connects the PML4, page-directory-pointer table and page directory.
2. Creates 512 writable, executable supervisor mappings with 2 MiB pages, identity-mapping `[0, 0x40000000)`.
3. Loads `CR3`, clears `CR4.LA57` and sets `CR4.PAE`.
4. Sets `EFER.LME` and enables paging with `CR0.PG`.
5. Loads the native GDT and jumps into 64-bit code.

The native GDT uses selector `0x08` for code and `0x10` for writable data. `include/abi/logit_boot.h` explains why those indices match the kernel GDT: interrupt returns must retain valid selectors after the kernel replaces the table.

An older comment above `load_kernel_and_enter` calls the entry contract “32-bit protected mode.” The final transition and the protocol header specify entry in 64-bit long mode.

## Kernel handoff

`native_long_entry` loads the data selectors, clears the `FS` and `GS` selectors, sets the temporary stack and jumps to the ELF entry with this state:

| State | Value |
|---|---|
| `RAX` | `LOGIT_BOOT_MAGIC`, `0x4c425436` |
| `RDI` | Boot-block physical address, `0x8000` |
| `RSP` | `0x7c00` |
| `CS` | `0x08` |
| `DS`, `ES`, `SS` | `0x10` |
| Interrupt flag | Clear |
| Paging | Four levels, with the first 1 GiB identity-mapped |

`linker.ld` selects `logit_native_start` in `c/boot/long.asm` as the entry. It switches to a 32 KiB stack owned by the kernel before making calls. The common entry code prepares x87 and SSE state, enables NX and SMEP when supported, and calls `logit_boot_normalize`.

The normalizer in `c/kernel/init/bootinfo.c` checks the entry magic, boot header, version, extent and tag framing. It also reads the final byte of the promised identity range. It converts the memory map, framebuffer, ACPI and end tag numbers in place, adjusts the total size and returns a pointer to the internal boot view at offset 16 in the native block. This adaptation lets existing kernel consumers share their tag parsers across both boot paths.

Refusal paths call `serial_flush_blocking` before halting with interrupts disabled. The kernel serial driver queues output, and draining driven by the timer starts later in initialization. The bounded flush in `c/drivers/char/serial.c` drains queued diagnostics and waits for the UART transmitter to empty.

Assembly then initializes CPU feature selection and calls `kernel_main` in `c/kernel/init/kmain.c`.

## Continuing in the kernel

Early C initialization installs interrupt and descriptor tables, configures the PIT and calls `bootmod_init` before `pmm_init`. The module registry in `c/kernel/init/bootmod.c` validates module tags and uses `pmm_hold_range` to retain their physical ranges. This shared path preserves files supplied in memory by the UEFI loader.

`pmm_init` keeps allocator metadata outside held ranges and reserves those ranges after releasing available memory. It also reserves the kernel and boot data and builds mappings of physical memory from the firmware map. The initial 1 GiB identity map provides the mapping needed to reach this setup.

After physical memory setup, `lapic_virtual_wire` in `c/kernel/cpu/irq/lapic.c` configures LINT0 to deliver interrupts from the legacy PIC when LAPIC initialization succeeds. `board_early` in `c/drivers/board/board.c` attempts to halt the PCH TCO watchdog before storage probing. `time_platform_init` in `c/kernel/core/ktime.c` discovers HPET and can use it to calibrate the TSC or provide the clock source.

Successful HPET initialization in `c/drivers/timer/hpet.c` disarms comparator interrupts and clears legacy replacement routing, returning IRQ0 to the PIT. The following PIT tick probe enables interrupts and waits until the monotonic clock advances by 50 ms or the loop reaches 200 million iterations.

## Boot progress messages

These messages identify stages reached by the loader and kernel entry code.

| Message | Emission point |
|---|---|
| `LOGIT_BIOS_PRELOAD_OK` | Preload entry, before the loader read |
| `LOGIT_BIOS_LOADER_NATIVE` | Second-stage entry |
| `LOADER A20 VERIFY PASS` | Successful A20 verification |
| `LOADER KERNEL LOAD OK` | Completion of ELF segment processing |
| `LOADER ENTER KERNEL` | Before the final mode transition |
| `LOGIT_BOOT_NATIVE_OK version=0001` | Successful normalization of the boot block |
| `[logitos] long mode, C kernel running` | Early in `kernel_main` |

The first five strings are in the BIOS assembly sources. The normalization marker comes from `c/kernel/init/bootinfo.c`; the C entry message comes from `c/kernel/init/kmain.c`.

Kernel progress also appears as numbered `[mark]` log lines, starting with `[mark] 3` at C entry. `c/kernel/diag/bootmark.c` draws progress squares and boot console text when it obtains a usable framebuffer mapping. It removes any temporary mapping before `pmm_init`. After framebuffer initialization, drawing resumes when the kernel reports the same boot framebuffer address. `make HWHUD=1` enables the hardware panel implemented in `c/kernel/diag/hwhud.c`.

## Limitations

- The preload requires BIOS extended disk services and reads the second stage in one transfer. `tools/mkiso.py` caps that payload at 127 native CD blocks.
- E820 storage holds 128 entries, and boot information must fit within `[0x8000, 0xa000)`. Overflow stops boot with a serial diagnostic.
- The ELF entry address, program-header offset and consumed segment fields must fit in 32 bits. Program headers must have a stride of 56 bytes, occupy at most 8 KiB and end within the first 32 KiB of the payload.
- ELF validation checks neither segment overlap nor placement in available firmware RAM or the initial identity map. It also skips the check that the entry point lies in a loaded segment.
- VBE probing examines at most 256 mode-list entries and targets 1024 × 768 × 32. Revision 2 or later RSDPs must declare a length between 36 and 256 bytes.
- The BIOS loader emits no boot module tags. `LOGIT_BOOT_TAG_MODULE` in `include/abi/logit_boot.h` is supplied by the UEFI loader.

## See also

- [Boot sequence](../architecture/boot-sequence.md)
- [UEFI boot path](boot-uefi.md)
- [Kernel initialisation](init.md)
- [Physical memory](physical-memory.md)
- [Building LogitOS](../getting-started/building.md)

## Source map

| File | What it contains |
|---|---|
| `c/boot/bios/preload.asm` | El Torito entry, serial setup, extended disk-service check and loader read |
| `c/boot/bios/loader.asm` | Firmware discovery, boot-block construction, ELF loading and mode transitions |
| `tools/mkiso.py` | ISO layout, boot catalog and payload-location patching |
| `include/abi/logit_boot.h` | Native entry contract, constants and wire structures |
| `tools/gen_boot_inc.py` | Generation of NASM protocol definitions from the C header |
| `Makefile` | BIOS-stage, protocol-include and product-ISO recipes; hardware-panel option |
| `linker.ld` | Kernel placement at 32 MiB and ELF entry selection |
| `c/boot/long.asm` | Kernel stack setup, CPU preparation and calls into C |
| `c/kernel/init/bootinfo.c` | Boot-block validation and conversion to the internal boot view |
| `c/kernel/init/kmain.c` | Early subsystem setup and boot progress messages |
| `c/kernel/mm/phys/pmm.c` | Firmware-map interpretation, reservations and physical-memory mappings |
| `c/drivers/char/serial.c` | Kernel serial setup, transmit queue and bounded diagnostic flush |
| `c/kernel/init/bootmod.c` | Boot module validation and registration before physical memory initialization |
| `c/boot/efi/loader.c` | Construction of module tags used by the shared kernel initialization path |
| `c/kernel/cpu/irq/lapic.c` | LAPIC setup and legacy PIC interrupt delivery through LINT0 |
| `c/drivers/board/board.c` | Early PCH discovery and watchdog handling |
| `c/kernel/core/ktime.c` | Platform clock setup and TSC calibration through HPET |
| `c/drivers/timer/hpet.c` | HPET discovery, comparator disarming and legacy-route handling |
| `c/kernel/diag/bootmark.c` | Numbered boot markers, framebuffer mapping and boot console |
| `c/kernel/diag/hwhud.c` | Hardware status panel |
