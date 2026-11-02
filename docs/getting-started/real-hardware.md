# Running on real hardware

LogitOS boots through its BIOS optical loader or its UEFI loader on a FAT volume. A physical installation needs a usable display and a storage driver that can reach the LogitFS root filesystem during early startup. The repository records a physical X79-VG4 desktop boot, along with host models and QEMU experiments for other platforms and devices.

## Recorded hardware evidence

Dates below identify recorded observations. `TRANSPARENCY.md` requires every hardware claim to name the environment where it was demonstrated.

| Environment | Recorded result | Report |
|---|---|---|
| Physical X79-VG4, 2026-10-31 | X79-VG4 V2.2, AMI UEFI, Xeon E5-2670 v2 and 16 GiB DDR3 reached the desktop with 20/20 logical CPUs online. A Netac 120 GB SATA SSD supplied the root partition. A GTX 1050 supplied a 1280×1024 firmware framebuffer. Both DIMMs’ SPD data were read through the CPU memory controller, and USB keyboard and mouse enumeration was recorded. | `docs/hw/X79-VG4.md` |
| Physical RX 580 2048SP, 2026-11-01 | An 8 GiB card on the X79-VG4, PCI identity `1002:6fdf`, revision `ef`, passed the GPU self-test’s gate, lease and map stages, then stopped during binding. | `docs/superpowers/plans/2026-11-01-rx580-stage-b.md` |
| Synthetic Xeon E5 configurations | QEMU brought 16 logical CPUs online through BIOS and 24 through UEFI. | `docs/X79_XEON_E5_V1_V2_2026-09-11.md` |
| Core i7-14700KF audit | Host decoder fixtures and synthetic BIOS and UEFI guests exercised startup, APIC identities, timers and IPIs with 28 virtual CPUs. | `docs/I7_14700KF_CPU_AUDIT_2026-09-13.md` |
| X79/C600 USB experiments | Host EHCI models and QEMU exercised HID input and storage. Combined EHCI and xHCI storage runs checked data retention across two cold boots. | `docs/EHCI_X79_2026-09-11.md` |
| Intel 82579LM/V Ethernet | A host register model exercised driver initialization, transmission, reception and shutdown. | `docs/82579_PCH2_2026-09-11.md` |
| C600/X79 SMBus and DDR3 SPD | Host models exercised controller transactions, capacity decoding and CRC validation. | `docs/X79_C600_SPD_2026-09-11.md` |
| GTX 1050 framebuffer preservation | Host fixtures and QEMU with a display identity alias used in tests exercised preservation of the boot framebuffer. | `docs/nvidia-gtx1050-boot-display-2026-09-11.md` |

The X79-VG4 observations came from photographs of the screen, boot console and paged boot report. That installation boots the UEFI loader from a USB stick and mounts LogitFS from a GPT partition on the SATA SSD.

## Prepare the images

The boot medium carries the loader and kernel. The root image carries applications and runtime files.

| Output under `BUILD` | Purpose |
|---|---|
| `esp.img` | FAT volume containing the UEFI loader and kernel. |
| `logit.iso` | BIOS optical image containing the bootloader stages and kernel. |
| `disk.img` | Development LogitFS image, including test fixtures. |
| `release.img` | LogitFS image built with the release recipe’s test-path exclusions. |

From the repository root, build the UEFI and release images:

```sh
make esp release-disk
```

For BIOS optical boot, build the ISO as well:

```sh
make
```

The default target builds the ISO. Include `release-disk` when preparing the applications and root filesystem. The recipes are in `Makefile` and `tests/uefi.mk`; see [Building LogitOS](building.md) for toolchain setup.

For a diagnostic build, use:

```sh
make BUILD=build-hardware HWHUD=1 esp release-disk
```

Choose a fresh `BUILD` directory when enabling `HWHUD=1`. This option enables the hardware panel, diagnostic audio tone, Polaris GPU self-test and developer-key loading. `tests/uefi.mk` passes the corresponding option to the EFI compiler. Reusing a directory after changing the option can mix artifacts built with different settings.

## Write a UEFI boot medium

Use a removable drive whose contents can be overwritten. Writing the image to the whole device replaces its existing partition and filesystem metadata.

The physical X79-VG4 boot used the GPT layout supported by `tools/mkesp.py`. After building the loader and kernel, generate it with:

```sh
python3 tools/mkesp.py build/usb-gpt.img --layout gpt \
  --efi build/efi/BOOTX64.EFI --kernel build/kernel.elf \
  --firmware-dir third_party/amd-firmware/amdgpu
```

Substitute the selected `BUILD` directory for `build` throughout the command when using a diagnostic build.

1. Identify the destination drive by its device identity and capacity, then unmount its volumes.
2. Use a raw-image writer to write the generated image to the whole device from byte zero.
3. Flush pending writes and eject the drive.
4. Select the drive’s UEFI boot entry in the target machine’s firmware.

The GPT layout contains a protective MBR, primary and backup GPT metadata, and an EFI System Partition beginning at LBA 2048. The `esp` make target uses the generator’s default layout: a FAT filesystem beginning at byte zero, also called a superfloppy. `tools/mkesp.py` records OVMF boot experiments with that default layout.

The generated volume contains `\EFI\BOOT\BOOTX64.EFI` and `\LOGIT.ELF`. The loader reads the kernel from the filesystem attached to its own firmware device handle; see `c/boot/efi/loader.c`.

The command also packages AMD firmware under `\LOGIT\FW`, with short filenames and an `INDEX.TXT` name map. The loader reads these files into boot modules before leaving boot services. The Polaris resource loader can use them when the root filesystem’s firmware copy fails validation. Both root-image recipes also package firmware under `/lib/firmware/amdgpu`. See `tools/mkesp.py`, `Makefile` and `c/drivers/gpu/amd/polaris/resource/files.c`.

## Write BIOS optical media

Burn `logit.iso` as an optical-disc image and select the firmware’s legacy BIOS optical boot entry.

`tools/mkiso.py` writes ISO9660 with a BIOS El Torito entry using no emulation. Firmware loads `c/boot/bios/preload.asm`, which reads the next stage in native CD sectors of 2,048 bytes. That stage comes from `c/boot/bios/loader.asm`.

## Prepare the root disk

For initial setup, use a dedicated AHCI SATA or NVMe disk for LogitFS. Write `release.img` to that disk from byte zero with a raw-image writer; the write overwrites existing data.

The image begins with the LogitFS superblock. The block layer recognizes this layout on a whole disk or inside a discovered partition. `tools/mkfs.py` defines the image layout, and `c/drivers/block/blkdev.c` selects the first registered device carrying a recognized superblock.

The recorded X79-VG4 installation uses a 20 GiB GPT partition after Windows C:. For that arrangement, `tools/windows/install-root.ps1` creates a partition in an existing free extent and writes the image at its offset. It requires Windows, PowerShell 7 and administrator rights. It requests confirmation before partition creation and again before writing, then reads back the written range for SHA-256 comparison. Its inventory and placement policy is in `tools/windows/RootPlan.psm1`.

For SATA, select AHCI mode in firmware. `c/drivers/block/ahci.c` matches PCI class, subclass and programming interface `01:06:01`. It also resets links that firmware hands over without a usable disk signature, which addresses the port-4 startup problem recorded on the X79-VG4.

Startup initializes early block devices and mounts LogitFS before the general device-probing pass. USB controllers enumerate their storage during that later pass. This order is visible in `c/kernel/init/kmain.c`, `c/drivers/block/blkdev.c` and `c/drivers/usb/usb_core.c`.

Use one intended LogitFS root during initial setup so that registration order selects it predictably. Treat it as writable: mounting can replay an interrupted journal transaction through the recovery path in `c/fs/logitfs/logitfs.c`.

## Display and firmware handoff

The UEFI loader selects among usable 32-bit GOP modes. Earlier instructions described it as preserving the firmware’s graphics mode. It prefers the display’s preferred progressive EDID size, then the largest mode within 2% of its aspect ratio, then the largest usable mode. If changing modes fails, it requests the original mode and validates the resulting live framebuffer description. The policy is in `c/boot/efi/gop_pick.c` and `c/boot/efi/gop_pick_efi.c`.

The loader passes the framebuffer address, dimensions, stride and colour layout to the kernel. The kernel tries VirtIO GPU first, then the supplied 32-bit RGB framebuffer; see `c/kernel/gui/fb/fb.c`.

The kernel links at 32 MiB in `linker.ld`. The EFI loader reserves its destination before copying and checks firmware memory ownership after allocation and again before leaving boot services. It also copies the final handoff stub into executable memory below 1 GiB. This supports firmware placement of the EFI application above the initial identity mapping, as recorded on the X79-VG4. See `c/boot/efi/loader.c` and `c/boot/efi/load_policy.h`.

An `HWHUD=1` build can run the Polaris self-test, which loads microcode and exercises the copy engine within a reserved VRAM arena while the desktop uses CPU rendering. To disable this test, place an empty `\LOGIT\NOGPU` or `\LOGIT\NOGPU.TXT` file on the boot volume. Errors checking or forwarding that marker also disable the test. See `c/drivers/gpu/amd/bootfb.c`, `c/boot/efi/gpu_optout_marker.c` and `c/drivers/gpu/amd/polaris/selftest/opt_out.c`.

## Capture the first boot

For a compatible legacy UART, capture COM1 at I/O address `0x3f8`, using 115200 baud, eight data bits, no parity, one stop bit and no flow control. The divisor-1 configuration in `c/boot/bios/preload.asm`, `c/boot/efi/loader.c` and `c/drivers/char/serial.c` supersedes the earlier 38400-baud instructions.

| Output | Meaning |
|---|---|
| `[efi] REFUSING TO BOOT:` | A loader precondition failed; retain the following reason. |
| `[efi] gop firmware` and `[efi] gop pick` | Initial and selected display geometry. |
| `[efi] ebs ok` | The loader exited boot services. |
| `LOGIT_FB_FAIL` | Kernel framebuffer initialization failed and startup halted. |
| `[blk]` root-selection line | Selected root device and superblock-discovery result. |
| `[fs] mounted` or `[fs] mount FAILED` | Initial root-mount result. |
| `[dev]` lines containing `driver=` | Enumerated PCI functions and driver bindings. |
| `[amd-polaris-runtime] stage=` | Polaris self-test progress and result. |
| `[devssh]` | Developer-key provisioning status. |
| `LOGIT_BOOT_OK` | Startup reached the marker following a successful root mount and device probing. |

These messages come from the EFI loader, `c/kernel/init/kmain.c`, `c/drivers/block/blkdev.c`, `c/drivers/core/device.c`, the Polaris self-test and `c/kernel/init/devssh.c`.

Kernel serial output uses a 64 KiB transmit ring. Preserve any `[serial]` loss notice with the capture; it identifies missing log bytes. The hardware panel shows queue, transmission and drop counters. Clicking its title collapses or expands it; see `c/drivers/char/serial.c` and `c/kernel/diag/hwhud.c`.

The diagnostic build schedules a one-second 440 Hz tone after five seconds have elapsed from the panel’s first audio poll. Record sound heard at each connector alongside the reported stream and hardware-position observations. The implementation is in `c/kernel/audio/boot_test_tone.c` and `c/kernel/audio/boot_test_tone.h`.

For network diagnosis, capture link status and DHCP messages together. The current DHCP code shares a four-second boot deadline between link waiting and address acquisition, then continues acquisition in the background. With automatic configuration selected, a machine without a detected hypervisor retains `0.0.0.0` while awaiting its first lease. Static configuration comes from settings. See `c/net/core/netcfg.c` and `c/net/core/dhcp.c`.

An `HWHUD=1` loader can read Ed25519 public keys from `\LOGIT\DEVSSH.PUB`, added through the image generator’s `--file` option. After mounting the root, the kernel validates the keys, provisions a `dev` account with a locked password, writes its authorized keys and enables SSH startup at login. The account and key files persist on the root filesystem across boots. See `c/kernel/init/devssh.c` and `c/kernel/init/devssh.h`.

Save the complete log or screen photographs with the motherboard model, firmware version and settings, attached devices, build options and image hashes. Record visible desktop behaviour, typing and mouse input as observations alongside device enumeration.

## Limitations

- The X79-VG4 keyboard enumerates, but typing remains unresolved. Its board report also records about 10–13% clock tick loss during boot, with audio playback, reboot, power off and NCT6779 sensor validation still pending.
- The board report initially marks networking as passed, then records a later slow-link run using the QEMU fallback address `10.0.2.15`. The revised DHCP policy, serial buffering, developer SSH path and subsequent diagnostic changes still require verification on that machine.
- The recorded RX 580 self-test stopped during binding at native mapping validation. Subsequent changes address the observed memory-controller and linear-mapping state; physical completion of the SMU/SDMA test remains pending. GTX 1050 hardware fill and copy return failure in `c/drivers/gpu/nvidia_pascal_accel.c`.
- The synthetic i7 experiments exercise virtual CPUs; physical hybrid-core topology and behaviour still need hardware verification. Standard builds provide 32 logical-CPU slots through `PERCPU_MAXCPU` in `c/kernel/cpu/smp/percpu.h`.
- The BIOS ISO has no hybrid USB layout and no UEFI boot section, so raw USB writing has no established BIOS boot procedure.
- USB storage is discovered after root selection and mounting, so the current startup path cannot use it as the root filesystem.
- AHCI requires 512-byte logical sectors and rejects native 4Kn media. Controllers exposing a RAID interface fall outside its AHCI class match. Later MSI and NCQ changes carry model/QEMU evidence pending hardware verification.
- `c/boot/efi/build.sh` produces an unsigned EFI application, and the loader performs no kernel-signature authentication; firmware must permit that application. A framebuffer available solely through firmware blitting is unusable after boot services end; startup halts if kernel framebuffer initialization fails.
- Intel AX200/AX210 transport and firmware startup remain unimplemented, as recorded in `c/drivers/net/wifi/README.md`. The 82579 driver rejects the managed-firmware path in `c/drivers/net/e1000_pch2.c`.
- EHCI lacks UHCI/OHCI companion routing, isochronous transfers and high-bandwidth interrupt endpoints. Devices requiring a companion on an EHCI root port are refused; see `c/drivers/usb/ehci.c`.
- GPU HDMI/DisplayPort audio is surveyed for diagnostics; playback awaits display-engine audio routing in `c/drivers/audio/hda.c`.

## Source map

| File | What it contains |
|---|---|
| `TRANSPARENCY.md` | Rules for claims and test-environment attribution. |
| `docs/hw/X79-VG4.md` | Physical-machine inventory, boot observations and outstanding checks. |
| `docs/superpowers/plans/2026-11-01-rx580-stage-b.md` | Recorded RX 580 failure and subsequent bring-up work. |
| `docs/X79_XEON_E5_V1_V2_2026-09-11.md` | Synthetic Xeon CPU results. |
| `docs/I7_14700KF_CPU_AUDIT_2026-09-13.md` | CPU audit, host fixtures and synthetic guest evidence. |
| `docs/EHCI_X79_2026-09-11.md` | EHCI host-model and QEMU HID/storage evidence. |
| `docs/82579_PCH2_2026-09-11.md` | 82579 register-model evidence. |
| `docs/X79_C600_SPD_2026-09-11.md` | SMBus and DDR3 SPD model evidence. |
| `docs/nvidia-gtx1050-boot-display-2026-09-11.md` | Framebuffer-preservation experiments. |
| `Makefile` | Kernel, ISO, root-image and diagnostic-build rules. |
| `tests/uefi.mk` | EFI build wiring and FAT-image targets. |
| `tools/mkesp.py` | FAT and GPT image generation, extra files and firmware index. |
| `tools/mkiso.py` | BIOS El Torito ISO generation. |
| `tools/mkfs.py` | LogitFS image generation. |
| `tools/windows/install-root.ps1`, `tools/windows/RootPlan.psm1` | Partition installation, inventory policy and readback verification. |
| `c/boot/efi/build.sh` | EFI application compilation and linking. |
| `c/boot/efi/loader.c`, `c/boot/efi/load_policy.h` | Boot-volume reading, memory placement and firmware handoff. |
| `c/boot/efi/gop_pick.c`, `c/boot/efi/gop_pick_efi.c` | Display-mode selection and firmware calls. |
| `c/boot/efi/gpu_optout_marker.c` | GPU self-test marker lookup and forwarding. |
| `c/boot/bios/preload.asm`, `c/boot/bios/loader.asm` | BIOS optical boot stages. |
| `linker.ld` | Kernel load address and section layout. |
| `c/kernel/init/kmain.c` | Startup ordering and boot markers. |
| `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h` | Developer-key validation, account provisioning and persistent paths. |
| `c/drivers/block/blkdev.c` | Disk registration, partition discovery and root selection. |
| `c/drivers/block/ahci.c` | SATA link startup, sector validation, polling, MSI and NCQ. |
| `c/drivers/usb/usb_core.c` | USB discovery during controller probing. |
| `c/fs/logitfs/logitfs.c` | Filesystem mounting and journal recovery. |
| `c/kernel/gui/fb/fb.c` | Kernel framebuffer selection and mapping. |
| `c/drivers/char/serial.c` | UART configuration, transmit buffering and loss reporting. |
| `c/drivers/core/device.c` | Device enumeration and driver-binding diagnostics. |
| `c/kernel/diag/hwhud.c` | Hardware panel and diagnostic observations. |
| `c/kernel/audio/boot_test_tone.c`, `c/kernel/audio/boot_test_tone.h` | Diagnostic tone generation, scheduling and reporting. |
| `c/net/core/netcfg.c`, `c/net/core/dhcp.c` | Address policy, boot deadline and background acquisition. |
| `c/drivers/gpu/amd/bootfb.c` | Polaris resource discovery and self-test integration. |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading and validation. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.c`, `c/drivers/gpu/amd/polaris/selftest/opt_out.c` | GPU test stages and disabling policy. |
| `c/drivers/gpu/nvidia_pascal_accel.c` | GTX 1050 acceleration blockers. |
| `c/kernel/cpu/smp/percpu.h` | Logical-CPU capacity. |
| `c/drivers/net/wifi/README.md`, `c/drivers/net/e1000_pch2.c` | Wi-Fi implementation scope and 82579 firmware restrictions. |
| `c/drivers/usb/ehci.c` | EHCI transfer and companion-controller restrictions. |
| `c/drivers/audio/hda.c` | Analog audio and GPU audio diagnostics. |

## See also

- [Building LogitOS](building.md)
- [Running LogitOS in QEMU](running-in-qemu.md)
- [Boot sequence](../architecture/boot-sequence.md)
- [UEFI boot path](../kernel/boot-uefi.md)
- [Storage drivers](../drivers/storage.md)
- [USB](../drivers/usb.md)
- [GPU drivers](../drivers/gpu.md)
