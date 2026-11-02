# Kernel modules

LogitOS loads kernel modules from ELF relocatable objects, resolves references against a kernel export table, and registers their drivers with the device model. Module code executes with kernel privileges. ELF parsing and relocation live in `c/kernel/module/modelf.c`; file access, allocation, registration and syscall handling live in `c/kernel/module/modload.c`.

## Module format

A module is a little-endian x86-64 ELF64 object with type `ET_REL`. Its section headers, symbol tables and relocation records describe the image to load. Using the compiler’s object format keeps section layout and driver registration metadata together, as explained in `c/kernel/module/module.h`.

`mod_elf_size()` calculates the destination size, and `mod_elf_load()` uses the same layout routine to place sections. Sharing that routine keeps allocation size and section placement consistent. The loader copies `SHT_PROGBITS` sections carrying `SHF_ALLOC`, zeroes allocated `SHT_NOBITS` sections, and pads section offsets according to their alignment fields.

A driver defines a `struct driver` and uses `DRIVER_DECLARE` from `c/drivers/core/driver.h` to emit a pointer into the ELF section named `logit_drivers`. Relocation turns that entry into a pointer to the loaded driver structure.

The example rule in `tests/module.mk` compiles `c/drivers/core/qemu_edu.c` with the kernel’s `CFLAGS`, then copies the object to a file with the `.ko` extension.

## Loading and driver registration

`mod_load()` holds a sleeping mutex across the loading transaction. The implementation in `c/kernel/module/modload.c` performs these steps:

1. Derive a name from the path’s basename, stopping at the first dot, and check for a duplicate.
2. Check module table capacity, obtain the file size with `vfs_size()`, and read the complete image with `vfs_read()`.
3. Calculate the destination layout and allocate a block with `kmalloc_low()`.
4. Copy the allocated sections and apply their relocations.
5. Register driver pointers from `logit_drivers`, run `dev_probe_all()`, and publish the completed module record.

The input image uses `kmalloc()`. The relocated image uses `kmalloc_low()`, which allocates through an executable identity mapping below 1 GiB. This placement keeps module code within reach of relative calls to kernel exports and supports absolute addresses encoded in 32-bit relocation fields. The loader checks each 32-bit relocation against its encoding range. The allocation domains are implemented in `c/kernel/mm/phys/kheap.c`.

Module drivers enter the registry used by drivers linked into the kernel. `driver_register()` appends each driver. During `dev_probe_all()`, the device model tries matching drivers in registration order for available, unbound devices. A probe callback returning zero establishes the binding. Driver startup runs through these callbacks in `c/drivers/core/device.c`.

A successful module load can register zero drivers or bind zero devices. Its positive identifier records completion of the loading transaction; the module record reports the driver and binding counts.

## Publication and inspection

The loader assembles a private `struct kmodule` while registration and probing run. It copies the completed record into the module table, then publishes the count with a release store. `mod_count()` reads that count with acquire ordering. Readers can inspect completed entries while another load waits for file access or a probe callback. This publication scheme is implemented in `c/kernel/module/modload.c`.

The record contains an identifier, name, allocation base and size, executable section location, driver count and binding count. `mod_at()` provides access to published records.

The comment on `struct logit_modinfo` in `include/abi/logit_abi.h` describes `nbound` as “devices currently bound”. The implementation computes it once after the loading probe pass, so the reported value is a snapshot from that point.

## Linking against kernel symbols

For each symbol referenced by a relocation, `c/kernel/module/modelf.c` calculates an address from its loaded section, accepts an absolute symbol value, or passes an undefined name to a resolver callback. The kernel callback in `c/kernel/module/modload.c` calls `ksym_lookup()`.

`c/kernel/module/ksyms.c` maintains the export table. Keeping the module interface in a named list makes additions and removals visible interface changes.

| Area | Exported functions |
|---|---|
| Console | `kprintf` |
| Device control | `dev_enable_checked`, `dev_disable_checked`, `dev_enable`, `dev_disable`, `dev_bar_map` |
| Interrupts | `dev_irq_request`, `dev_irq_release`, `dev_irq_prefer`, `dev_irq_count` |
| Device lookup | `dev_find_class`, `dev_find_id` |
| VirtIO transport | `virtio_init`, `virtio_queue_setup`, `virtio_driver_ok`, `virtio_request` |
| Memory | `pmm_alloc`, `pmm_free`, `pmm_free_frames`, `kmalloc`, `kfree` |
| Memory operations | `memcpy`, `memset`, `memmove`, `memcmp` |
| Time | `time_mono_ms` |

The memory operations cover references the compiler can introduce for structure copies and storage clearing. Driver registration is performed by the loader using the relocated `logit_drivers` section.

An unresolved reference produces `MOD_E_UNDEF`. The loader prints the missing symbol’s name before releasing the input image, whose string table holds that name.

## Relocations and validation

`mod_reloc_apply()` in `c/kernel/module/modelf.c` implements the following operations. Here, `S` is the resolved symbol address, `A` is the relocation addend, and `P` is the address being patched.

| Relocation | Operation |
|---|---|
| `R_X86_64_NONE` | Preserve the destination bytes |
| `R_X86_64_64` | Write the 64-bit value `S + A` |
| `R_X86_64_PC32` | Write `S + A - P`, checked against the signed 32-bit range |
| `R_X86_64_PLT32` | Apply the same calculation as `R_X86_64_PC32` |
| `R_X86_64_32` | Write `S + A`, checked against the unsigned 32-bit range |
| `R_X86_64_32S` | Write `S + A`, checked against the signed 32-bit range |

Treating `R_X86_64_PLT32` as a direct relative relocation lets calls resolve straight to kernel functions. An encoding overflow returns `MOD_E_RANGE`.

The parser checks file extents and symbol and relocation record sizes. For relocations applied to loaded sections, it validates the target section, symbol table, referenced symbol index, symbol string table and patch location. Patch bounds use the selected relocation’s width, allowing a four-byte relocation in the final four bytes of a section.

Extent checks compare sizes against the remaining buffer length. This avoids overflow in offset-plus-size calculations. Relocations targeting sections omitted from the destination layout are skipped.

## System call interface

`c/kernel/exec/syscall.c` dispatches module operations to `mod_syscall()`. Their numbers and public result structure are defined in `include/abi/logit_abi.h`.

| Operation | Number | Arguments |
|---|---:|---|
| `SYS_MODULE_LOAD` | 182 | Path string |
| `SYS_MODULE_UNLOAD` | 183 | Module identifier |
| `SYS_MODULE_LIST` | 184 | Output array and capacity |
| `SYS_MODULE_SYM` | 185 | Symbol name and optional address destination |

Load, unload and symbol lookup require UID 0, checked through `vfs_cred_current()`. Listing is available to every caller. The module file read also goes through the VFS permission checks in `c/fs/vfs/vfs.c`.

`SYS_MODULE_LOAD` returns a positive module identifier or a negative loader error. `SYS_MODULE_SYM` returns 1 for an exported name and writes its address when an output destination is supplied; it returns 0 when the name is absent.

`SYS_MODULE_LIST` copies `struct logit_modinfo` entries containing the identifier, name, relocated size, driver count and binding snapshot. It returns the total loaded count, which lets callers detect an undersized array. A null output pointer with zero capacity also invokes `mod_dump()`, which prints the table, including allocation addresses, to the kernel console.

Loader errors are defined in `c/kernel/module/module.h`. They distinguish permissions, file access, image size, allocation, malformed ELF data, relocation type, relocation range, symbol resolution, table capacity, duplicate names and invalid arguments.

## Boot module data

The UEFI boot protocol also uses the term module for named data blobs. `c/boot/efi/loader.c` reads firmware files from the ESP and describes their memory ranges with `LOGIT_BOOT_TAG_MODULE`, defined in `include/abi/logit_boot.h`. These blobs enter the registry in `c/kernel/init/bootmod.c`, where consumers retrieve them through `bootmod_find()`.

In `c/kernel/init/kmain.c`, `bootmod_init()` runs before `pmm_init()`. Registration calls `pmm_hold_range()` so physical allocation and metadata placement preserve the blobs while reclaiming loader memory. The reservation machinery lives in `c/kernel/mm/phys/pmm.c`.

The Polaris firmware reader in `c/drivers/gpu/amd/polaris/resource/files.c` tries the root filesystem first and boot module data as a fallback. Both sources pass through firmware format checks and pinned SHA-256 checks. Those checks belong to the firmware consumer.

## Repository checks

`tests/module.mk` defines `test-modreloc`, which exercises the ELF core with compiler-generated objects. Assertions in `tests/unit/modreloc_test.c` cover relocation arithmetic, section contents, symbol resolution and malformed input. Its prerequisite controls remove range checks or record bounds checks and require failures. Tests requiring fixed address mappings or execution of x86-64 code report skipped coverage when the host lacks those capabilities.

The same fragment defines `test-module`, with `test-module-negctl` as a prerequisite. `tests/boot/run-module-test.sh` boots a kernel built with the QEMU education driver excluded, first with module loading disabled and then enabled using the same artifacts. Its assertions check probing, MSI and legacy interrupts, interrupt payloads, and credential handling.

`tests/unit/bkl_module_test.c` exercises the loader with substituted allocation, ELF and device operations and controlled file and probe boundaries. `tests/unit/bkl_module_test.py` checks concurrent duplicate loads and publication during probing, including controls that remove the transaction mutex or publish the count early.

## Limitations

- Unloading returns `MOD_E_NOUNLOAD` after the syscall privilege check. The driver registry retains pointers into module storage and has no unregister operation; freeing that storage would leave dangling pointers.
- The format carries no kernel ABI version check. Rebuild modules against the current `c/drivers/core/driver.h`: its `quiesce` callback precedes `next`, changing the structure layout from versions without that field. An incompatible layout can relocate successfully and fail during registration or use.
- The executable module loader performs no signature or cryptographic hash verification.
- Module storage remains writable and executable through the low identity mapping established by `c/boot/bios/loader.asm` and `c/boot/efi/loader.c`. The loader applies no W^X protection.
- External symbol resolution uses the kernel export table. Module exports and dependency loading are unsupported. Referenced `SHN_COMMON` symbols produce `MOD_E_UNDEF`, with a diagnostic directing the author to rebuild with `-fno-common`.
- The loader rejects `SHT_REL` records and supports the relocation types listed above. Objects are limited to 64 sections. For placed sections, nonzero alignment must be a power of two through 4096.
- `MOD_MAX` permits eight loaded modules. `MOD_MAX_IMAGE` caps both the input file and destination layout at 1 MiB. Names retain up to 31 bytes, so basename derivation and truncation can cause collisions.
- Module drivers follow previously registered drivers. Probing preserves existing bindings, so loading a module cannot replace a driver already bound to a device.

## See also

- [Device and driver model](../architecture/device-model.md)
- [Kernel heap](kernel-heap.md)
- [System calls](../architecture/system-calls.md)
- [Security model](../architecture/security-model.md)
- [Testing](../architecture/testing.md)
- [UEFI boot path](boot-uefi.md)
- [GPU drivers](../drivers/gpu.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/module/module.h` | Module structures, limits, errors, interfaces and design rationale |
| `c/kernel/module/modelf.c` | ELF validation, section layout, symbol resolution and relocation arithmetic |
| `c/kernel/module/modload.c` | Loading transaction, driver registration, publication and syscall handling |
| `c/kernel/module/ksyms.c` | Kernel symbol export table and lookup |
| `c/drivers/core/driver.h` | Driver layout, callbacks and `DRIVER_DECLARE` |
| `c/drivers/core/device.c` | Driver registration, matching, probing and binding |
| `c/drivers/core/qemu_edu.c` | Driver compiled as the example module |
| `c/kernel/mm/phys/kheap.c` | Heap allocation domains, including `kmalloc_low()` |
| `c/kernel/mm/phys/pmm.c` | Physical allocation and reservation of boot module pages |
| `c/fs/vfs/vfs.c` | File reads and permission checks |
| `include/abi/logit_abi.h` | Module syscall numbers and `struct logit_modinfo` |
| `c/kernel/exec/syscall.c` | Dispatch to the module syscall handler |
| `c/boot/bios/loader.asm` | BIOS initial identity mapping |
| `c/boot/efi/loader.c` | UEFI initial identity mapping and boot module handoff |
| `include/abi/logit_boot.h` | Boot module tags and physical ranges |
| `c/kernel/init/bootmod.c` | Boot module registry, validation and lookup |
| `c/kernel/init/kmain.c` | Boot module registration before physical memory initialisation |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading from the root filesystem and boot modules, with format and digest checks |
| `tests/module.mk` | Module compilation rules and relocation and guest test targets |
| `tests/unit/modreloc_test.c` | Relocation, format and object-loading assertions |
| `tests/boot/run-module-test.sh` | Guest loading, permission and interrupt checks |
| `tests/unit/bkl_module_test.c` | Loader concurrency and publication assertions |
| `tests/unit/bkl_module_test.py` | Concurrency controls and test orchestration |