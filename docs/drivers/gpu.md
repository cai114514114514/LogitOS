# GPU drivers

LogitOS presents the desktop through the framebuffer layer in `c/kernel/gui/fb/fb.c`. It selects a VirtIO GPU resource backed by RAM or a framebuffer supplied by the boot loader. The window manager composes pixels on the CPU. Native GPU code provides adapter ownership checks, hardware discovery, an RV100 presentation path, and Polaris10 firmware and SDMA bring-up.

## Display startup

PCI enumeration and PAT initialization precede `fb_init()`. The kernel composes and submits the first desktop frame, starts the additional CPUs, and then calls `dev_probe_all()`. VirtIO GPU's registered probe binds the instance created during framebuffer initialization, connecting the early display to device removal and teardown. See `c/kernel/init/kmain.c` and `c/drivers/virtio/virtio_gpu.c`.

`fb_init()` first attempts VirtIO GPU initialization. On success, it uses the driver's RAM backing and dimensions. The boot framebuffer fallback accepts an RGB descriptor with 32 bits per pixel and records its physical address, byte pitch, dimensions, and channel positions.

The previous boot framebuffer policy used uncached mappings. The current implementation requests PAT write combining when available and uses uncached mappings as the fallback. It reads back the page table cache flags and logs the result. After the first desktop frame, `fb_present_bench()` times one full screen CPU copy with the kernel's monotonic clock and reports duration, throughput, and the recorded mapping classification. See `c/kernel/gui/fb/fb.c`.

The window manager composes into a RAM back buffer and sends damaged rectangles through `fb_present_rect()`. A native presentation callback receives these composed pixels. This places display transport and GPU copy operations beneath composition. See `c/kernel/gui/wm.c`.

## Boot framebuffers

Both production loaders describe the display through the native boot protocol in `include/abi/logit_boot.h`.

| Boot path | Framebuffer handoff |
| --- | --- |
| BIOS | `c/boot/bios/loader.asm` searches VBE modes for 1024 × 768, 32 bits per pixel, direct color, and a linear framebuffer. It sets the mode and records the address, pitch, dimensions, and channel layout. |
| UEFI | `c/boot/efi/loader.c` selects a usable GOP mode, derives byte pitch from pixels per scanline, and exports the live framebuffer address and channel layout. Supported layouts include RGB, BGR, and validated bitmask formats with eight bits per color channel. |

The earlier UEFI description said the loader exported the firmware's current mode. Selection now uses `c/boot/efi/gop_pick.c` and `c/boot/efi/gop_pick_efi.c`. Given a valid EDID with a preferred progressive base detailed timing, the selector tries its dimensions, then the largest usable mode within 2% of its aspect ratio. Its general fallback is the largest usable 32-bit mode.

EDID lookup follows the selected GOP interface's handle, associating the monitor information with that output. After a failed mode change, the loader requests the original mode and validates the live descriptor before constructing the framebuffer handoff. A display report tag carries the original dimensions, a bounded mode list, the selection reason, and captured EDID bytes.

`c/kernel/init/bootinfo.c` validates the native boot information and translates tag numbers into the compatibility view consumed by the framebuffer parser. This explains the Multiboot2 names inside `c/kernel/gui/fb/fb.c`.

For CPU presentation through a boot framebuffer, the window manager can use a software cursor overlay. Movement presents the old cursor box from the RAM back buffer, then presents the new box with cursor pixels blended into temporary storage. The back buffer supplies the desktop pixels beneath the pointer. See `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.c`, and `c/kernel/gui/wm.c`.

## Adapter ownership

`fb_boot_lfb_range()` exposes the physical boot framebuffer aperture. Intel, NVIDIA, and AMD probes require its complete range to fit inside a memory BAR of the candidate PCI function. The probes also check display identity, memory decoding, and D0 power state when a PCI power management capability exists.

Checking the complete aperture associates the display with its adapter on machines containing several GPUs. The implementations are in `c/drivers/gpu/intel_bootfb.c`, `c/drivers/gpu/nvidia_pascal.c`, and `c/drivers/gpu/amd/bootfb.c`.

## VirtIO GPU

`c/drivers/virtio/virtio_gpu.c` implements the VirtIO GPU 2D resource protocol. Initialization configures the control queue and attempts cursor queue setup before declaring the device ready.

The driver reads display information during initialization. It accepts display 0's dimensions when that display is enabled and its width and height meet the 1280 × 800 minimum. Otherwise, it uses 1920 × 1200. It allocates coherent DMA backing, creates a 2D resource, attaches the backing, and binds the resource to scanout 0. CPU pixels use `0x00RRGGBB`.

Presentation has two stages:

1. `fb_present_rect()` copies damaged pixels from the compositor's back buffer into the resource's RAM backing.
2. `virtio_gpu_flush()` submits the rectangle's transfer to the host resource, followed by a resource flush.

An operation lock protects command staging through request completion. The framebuffer layer can distribute large CPU copies through its parallel presentation callback.

The cursor uses a 64 × 64 resource with alpha and a dedicated queue. Defining an image transfers its pixels through the control queue; movement submits a 56-byte cursor command. The framebuffer interface exposes cursor availability through `fb_cursor_hw()`.

Attached backing remains owned by the device until teardown revokes access. Shutdown withdraws the CPU framebuffer pointer and drains writers before resetting the device and releasing backing and queue allocations. A failed drain or reset retains resources in quarantine, preventing reuse while access may remain outstanding.

## Native GPU implementations

| Adapter or family | Implemented role |
| --- | --- |
| Intel display functions | Probe ownership and availability of the firmware framebuffer. |
| NVIDIA GTX 1050 and GTX 1050 Ti | Match twelve PCI IDs, validate framebuffer ownership, verify a pinned firmware bundle, and read GP107 identity and status registers. |
| AMD display functions | Validate framebuffer ownership and dispatch device preparation. |
| AMD RV100, PCI `1002:5159` | Run a 2D engine startup check and register a desktop presentation callback after successful preparation. |
| AMD Polaris10 | Discover registers, VBIOS reservations, and firmware candidates. Builds with `HWHUD=1` add display surveying, VRAM arena planning, and a firmware and SDMA self-test. A presentation entry point accepts resources supplied by a caller. |

The Polaris10 matcher in `c/drivers/gpu/amd/polaris/pci_id.h` lists thirteen device IDs, including `67df` and `6fdf`. The header records the earlier `67df` restriction and the observed `6fdf` revision `ef` RX 580 2048SP board that prompted the correction. Device matching and SMC firmware selection share this header.

## RV100 presentation

`c/drivers/gpu/amd/rv100/engine.c` validates the scanout layout, maps register and VRAM BARs, and chooses scratch areas beyond the visible framebuffer. Preparation saves affected registers and scratch contents, submits fill and copy commands, compares their results, and verifies restoration before reporting success.

After successful preparation, `c/drivers/gpu/amd/accel.c` registers the native framebuffer callback. A uniform damaged rectangle uses an engine fill. Other rectangles are uploaded into VRAM staging and copied into scanout by the engine.

Successful presentation waits for engine and destination cache completion. The first presentation also compares destination pixels with the supplied image. Presentation requires staging capacity for the prepared scanout, so the startup check and desktop presentation have distinct acceptance conditions.

## Polaris discovery and arena planning

After driver binding and a successful root mount, `c/kernel/init/kmain.c` calls `amd_bootfb_probe_resources()`. The resource consumer rechecks PCI identity and BAR configuration, samples registers, selects a VBIOS image, and checks firmware candidates. See `c/drivers/gpu/amd/polaris/resource/device.c`.

The former VBIOS discovery path copied the first 256 KiB of the VRAM aperture. The current selector first checks a boot module named for the PCI segment, bus, slot, and function, then searches ACPI VFCT for a matching image. The UEFI loader collects available display functions' PCI ROM images before exiting boot services. These sources provide recorded provenance while preserving the scanout's cache mapping. See `c/boot/efi/vbios_rom.c` and `c/drivers/gpu/amd/polaris/resource/vbios.c`.

The ATOM parser checks ROM structure and a supported Polaris10 PCI identity, then decodes supported firmware VRAM usage tables. Its output describes firmware reservations and interpreter scratch requirements. See `c/drivers/gpu/amd/polaris/resource/atom.c`.

With `HWHUD=1`, discovery continues into a display survey. `c/drivers/gpu/amd/polaris/display/crtc.c` reads six display controllers and matches the boot framebuffer by surface address, linear 32-bit layout, and pitch. It measures the matching controller's frame counter against monotonic time over at least 100 ms.

The arena planner inventories active scanouts, enabled cursor surfaces, the complete boot framebuffer, ATOM firmware and scratch ranges, and the VBIOS framebuffer allocation. Reservations expand to 64 KiB boundaries. The planner searches from the top of the CPU aperture for a window aligned and sized to 2 MiB multiples, then validates its layout with the runtime's memory planner. Reserving the complete boot framebuffer protects rows beyond a controller's active display area. See `c/drivers/gpu/amd/polaris/memory/arena.c`.

## Polaris boot self-test

The HWHUD boot path invokes `c/drivers/gpu/amd/polaris/selftest/selftest.c` after resource discovery and arena surveying. The self-test checks device identity, opt-out state, the arena plan, and firmware bundle sizing before acquiring an AMD GPU lease.

The lease requires the device to be bound to the requesting driver. Runtime adapters check the lease around register access, mapping resolution, and visibility operations. Retirement lasts for the rest of the boot. See `c/drivers/gpu/amd/gpu_lease.c` and `c/drivers/gpu/amd/bootfb.c`.

The self-test proceeds through these stages:

1. Map BAR5 registers and the arena window within BAR0.
2. Prepare the memory controller's system aperture and bind the native platform.
3. Compare runtime arena mappings with the survey plan.
4. Read the SMU security selector through its indirect register port.
5. Load the selected firmware set and run firmware startup and SDMA fill and copy checks inside staging.

BAR0 window mapping uses `dev_bar_map_window()` in `c/drivers/core/device.c`. Restricting that mapping to the arena preserves the boot scanout's PAT cache flags.

Memory controller preparation checks the existing access mode, VM contexts, AGP range, protection fault status, and blackout state. For the accepted firmware state, it programs the system aperture to cover VRAM, sets physical access inside that aperture, and invalidates VM context 0 translations. Register readbacks check the programmed aperture and access fields. See `c/drivers/gpu/amd/polaris/memory/gmc_aperture.c`.

The runtime runs in `POLARIS_DESKTOP_MODE_SELF_TEST`. In this mode the CPU presents the desktop while GPU commands exercise staging memory. Serial output and the hardware panel report stage outcomes and SMU startup observations.

The UEFI loader recognizes `\LOGIT\NOGPU` and `\LOGIT\NOGPU.TXT` on the boot volume as opt-out markers. File presence becomes a boot module. Lookup or delivery failures produce an assumed opt-out tag, which also disables the self-test. See `c/boot/efi/gpu_optout_marker.c` and `c/drivers/gpu/amd/polaris/selftest/opt_out.c`.

During shutdown, `dev_quiesce_all()` invokes the AMD display driver's quiesce hook. The self-test inspects configured SDMA ring pointers and runtime quarantine, attempts saved memory controller restoration subject to the ring check, and retires the lease. Failure during startup also attempts restoration after memory controller preparation. These paths report their outcomes through the serial log.

## Polaris presentation runtime

`polaris_desktop_start()` in `c/drivers/gpu/amd/polaris/desktop.c` accepts platform resources and a runtime request. The caller supplies device, arena, and scanout ownership, mappings, and a reservation inventory. Binding checks these resources against the current boot surface.

`c/drivers/gpu/amd/polaris/native.c` validates PCI configuration and memory controller state to establish the relationship between CPU BAR addresses and GPU VRAM addresses. Its accepted HDP layout uses linear physical addressing with byte swapping disabled.

Startup in `c/drivers/gpu/amd/polaris/runtime.c` parses firmware, plans memory, resolves mappings, checks overlaps and CPU aliases, stages firmware, loads engines through the SMU, and configures the SDMA ring. Presenter initialization exercises a fill and a nonconstant copy within staging.

Successful startup in presentation mode installs the framebuffer callback. The boot self-test uses `polaris_desktop_bind()` and `polaris_desktop_run()` so it can read the security selector between binding and firmware selection.

The presenter uploads damaged pixels into staging and submits SDMA copies into scanout. It combines complete scanlines when the destination layout permits and preserves pitch for partial rectangles. Queue completion checks the fence sequence, ring pointers, and destination contents before recording success. See `c/drivers/gpu/amd/polaris/present.c` and `c/drivers/gpu/amd/polaris/sdma/queue.c`.

## Presentation ownership

Native presentation runs under the framebuffer graphics mutex. Before invoking a callback, `fb_present_rect()` blocks admission of CPU front buffer writers and waits for existing writers, including copy workers on other CPUs.

The callback contract in `c/kernel/gui/fb/fb.h` defines three outcomes:

| Result | Framebuffer action |
| --- | --- |
| `0` | Record completed native presentation. |
| `-1` | Continue through CPU presentation. |
| `-2` | Latch quarantine and freeze front buffer writes until reboot. |

Quarantine protects scanout ownership when GPU completion is uncertain: a delayed GPU write could race a later CPU copy. The framebuffer retains quarantine after callback unregistration.

## Firmware handling

NVIDIA preparation uses a manifest of 22 files pinned to a linux-firmware revision. The VFS reader loads files beneath `/lib/firmware/` and checks each size and SHA-256 digest. After all files pass, preparation maps BAR0 and reads three identity and status registers. GP107 identification requires chipset value `0x137`. See `c/drivers/gpu/nvidia_pascal_accel.c`.

Polaris loading reads seven common engine files beneath `/lib/firmware/amdgpu/` and selects an SMC file from the hardware security selector, device ID, and revision. Root filesystem files must be root owned regular files, have group and other write permissions clear, and contain between one byte and 2 MiB. The reader checks the length against file metadata.

A failed root filesystem load is retried through a boot module bearing the firmware filename. Both sources pass firmware format validation and SHA-256 checks against pins generated from `third_party/amd-firmware/MANIFEST`. Boot module bytes are copied into owned heap storage, giving both sources the same release path. Discovery checks both security selector candidates and releases the loaded bytes after recording its results. See `c/drivers/gpu/amd/polaris/resource/files.c`.

SMC selection follows these rules:

| Selection | SMC filename |
| --- | --- |
| Key 0 | `polaris10_smc_sk.bin` |
| Key 1, device `67df`, revisions `e3`, `e4`, `e5`, `e7`, `ef`; or device `6fdf`, revisions `e7`, `ef`, `ff` | `polaris10_k_smc.bin` |
| Key 1, device `67df`, revisions `e1`, `f7` | `polaris10_k2_smc.bin` |
| Key 1, remaining combinations | `polaris10_smc.bin` |

The rules live in `c/drivers/gpu/amd/polaris/pci_id.h` and `c/drivers/gpu/amd/polaris/resource/files.c`. The boot self-test obtains the key through `c/drivers/gpu/amd/polaris/smu/security_key.c`.

The engine bundle represents seven source files as nine load entries, including two copies of the MEC jump table. SMC firmware reaches the loader through its own input. The SMU loader handles an already running controller and cold starts in protected or unprotected mode, then checks mailbox responses and the firmware load mask. See `c/drivers/gpu/amd/polaris/firmware/bundle.h` and `c/drivers/gpu/amd/polaris/smu/loader.c`.

The `Makefile` generates digest pins through `tools/gpu/amd/fwpin.py` and includes the vendored AMD firmware in disk image packaging. `make build/disk.img` rebuilds the development disk. `tools/mkesp.py` accepts `--firmware-dir` to place firmware and an index on the EFI system partition. The UEFI loader delivers indexed files as boot modules; `c/kernel/init/bootmod.c` holds their pages before physical memory initialization.

## Limitations

- The framebuffer frontend manages one display. VirtIO uses scanout 0 and reads dimensions during initialization; runtime resizing and multiple monitor management are absent.
- Intel support observes firmware framebuffer ownership. NVIDIA preparation stops at `NV1050_BLOCK_CHANNEL_STACK`, and its fill and copy entry points return failure. AMD GCN, Vega, and RDNA classifications identify devices without implementing command engines. See `c/drivers/gpu/intel_bootfb.c`, `c/drivers/gpu/nvidia_pascal_accel.c`, and `c/drivers/gpu/amd/accel.c`.
- Polaris discovery records unresolved ownership and security selection. Production boot invokes the HWHUD self-test; it has no caller of `polaris_desktop_start()` to activate desktop presentation. See `c/drivers/gpu/amd/polaris/resource/device.c`, `c/drivers/gpu/amd/bootfb.c`, and `c/drivers/gpu/amd/polaris/desktop.c`.
- Polaris firmware parsers validate structure without CRC or vendor signature verification. The file loader's SHA-256 pins check integrity against the vendored manifest. See `c/drivers/gpu/amd/polaris/firmware.h` and `c/drivers/gpu/amd/polaris/resource/files.h`.
- Native GPU test scope varies: RV100 presentation uses QEMU, NVIDIA guest binding uses a synthetic VGA identity, and Polaris runtime tests model hardware interactions on the host. These establish behavior in those environments. The Polaris self-test and GMC aperture contracts record pending X79-VG4 hardware validation in `c/drivers/gpu/amd/polaris/selftest/selftest.h` and `c/drivers/gpu/amd/polaris/memory/gmc_aperture.h`.

## See also

- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Compositing and effects](../kernel/compositing-and-effects.md)
- [Device and driver model](../architecture/device-model.md)
- [VirtIO drivers](virtio.md)
- [Running on real hardware](../getting-started/real-hardware.md)

## Source map

| File | What it contains |
| --- | --- |
| `include/abi/logit_boot.h` | Framebuffer, display report, module, and GPU opt-out handoff layouts. |
| `c/boot/bios/loader.asm` | VBE mode selection and framebuffer handoff. |
| `c/boot/efi/loader.c` | GOP handoff and firmware module delivery. |
| `c/boot/efi/gop_pick.c`, `c/boot/efi/gop_pick_efi.c` | EDID parsing, GOP selection, and mode change recovery. |
| `c/boot/efi/vbios_rom.c` | Display PCI ROM collection into boot modules. |
| `c/boot/efi/gpu_optout_marker.c` | Boot volume opt-out marker handling. |
| `c/kernel/init/bootinfo.c`, `c/kernel/init/bootmod.c` | Boot information normalization and module page ownership. |
| `c/kernel/init/kmain.c` | Display initialization, driver binding, and resource discovery ordering. |
| `c/kernel/gui/wm.c` | CPU composition, damaged rectangle presentation, and cursor integration. |
| `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb.h` | Backend selection, cache policy, presentation ownership, and callback contract. |
| `c/kernel/gui/fb/fb_overlay.c` | Software cursor geometry and pixel composition. |
| `c/drivers/core/device.c` | Display quiesce and BAR window mappings. |
| `c/drivers/virtio/virtio_gpu.c` | VirtIO resources, queues, cursor plane, and teardown. |
| `c/drivers/gpu/intel_bootfb.c` | Intel firmware framebuffer ownership probe. |
| `c/drivers/gpu/nvidia_pascal.c`, `c/drivers/gpu/nvidia_pascal_accel.c` | NVIDIA matching, ownership checks, firmware verification, and identification. |
| `c/drivers/gpu/amd/bootfb.c`, `c/drivers/gpu/amd/accel.c` | AMD binding, preparation dispatch, and presentation integration. |
| `c/drivers/gpu/amd/gpu_lease.c` | Device lease acquisition, checks, and retirement. |
| `c/drivers/gpu/amd/rv100/engine.c` | RV100 startup checks and 2D presentation. |
| `c/drivers/gpu/amd/polaris/pci_id.h` | Polaris10 matching and SMC revision selection. |
| `c/drivers/gpu/amd/polaris/resource/device.c` | Boot resource discovery and firmware inventory. |
| `c/drivers/gpu/amd/polaris/resource/vbios.c`, `c/drivers/gpu/amd/polaris/resource/atom.c` | VBIOS selection and reservation parsing. |
| `c/drivers/gpu/amd/polaris/display/crtc.c` | Display controller survey and refresh measurement. |
| `c/drivers/gpu/amd/polaris/memory/arena.c` | Reservation inventory and arena planning. |
| `c/drivers/gpu/amd/polaris/memory/gmc_aperture.c`, `c/drivers/gpu/amd/polaris/memory/gmc_aperture.h` | Memory controller preparation, restoration, and validation scope. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.c`, `c/drivers/gpu/amd/polaris/selftest/selftest.h` | Boot self-test stages, diagnostics, and quiesce. |
| `c/drivers/gpu/amd/polaris/selftest/opt_out.c` | Kernel consumption of self-test opt-out state. |
| `c/drivers/gpu/amd/polaris/desktop.c`, `c/drivers/gpu/amd/polaris/native.c` | Runtime activation and hardware address validation. |
| `c/drivers/gpu/amd/polaris/runtime.c` | Firmware, SMU, SDMA, and presenter startup. |
| `c/drivers/gpu/amd/polaris/present.c`, `c/drivers/gpu/amd/polaris/sdma/queue.c` | Rectangle uploads, command completion, and readback checks. |
| `c/drivers/gpu/amd/polaris/resource/files.c`, `c/drivers/gpu/amd/polaris/resource/files.h` | Firmware selection, source fallback, metadata checks, and digest pins. |
| `c/drivers/gpu/amd/polaris/firmware.h`, `c/drivers/gpu/amd/polaris/firmware/bundle.h` | Parser contracts and firmware load inventory. |
| `c/drivers/gpu/amd/polaris/smu/security_key.c`, `c/drivers/gpu/amd/polaris/smu/loader.c` | Hardware security selection and firmware startup. |
| `Makefile`, `tools/gpu/amd/fwpin.py` | HWHUD selection, disk packaging, and digest pin generation. |
| `third_party/amd-firmware/MANIFEST`, `tools/mkesp.py` | Firmware provenance and EFI system partition packaging. |
| `tests/gpu/amd/rv100/present.mk`, `tests/nvidia_pascal.mk`, `tests/gpu/amd/polaris/runtime.mk` | Native GPU guest and host test arrangements. |