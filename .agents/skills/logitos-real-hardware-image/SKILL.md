---
name: logitos-real-hardware-image
description: Build a LogitOS USB boot image for real hardware (UEFI ESP with GPT layout, release root image, HWHUD diagnostics, AMD firmware, developer SSH key) and read what the first boot reports. Use when preparing or debugging a physical boot.
---

# Build a real-hardware image

1. **Build in a fresh directory** with the diagnostics you need:
   `make BUILD=build-hardware HWHUD=1 esp release-disk`. `HWHUD=1` adds the hardware panel, the boot
   test tone, the Polaris GPU self-test and developer-key loading; never reuse a build directory after
   changing it.
2. **Make the GPT USB image** (the layout the X79-VG4 boots):
   ```sh
   python3 tools/mkesp.py build-hardware/usb-gpt.img --layout gpt \
     --efi build-hardware/efi/BOOTX64.EFI --kernel build-hardware/kernel.elf \
     --firmware-dir third_party/amd-firmware/amdgpu
   ```
   Add `--file <key.pub>:LOGIT/DEVSSH.PUB` to provision the key-only developer SSH account on an
   `HWHUD=1` build. The root filesystem comes from `release.img`.
3. **Writing the stick is the owner's step**: it needs the destination device confirmed by identity and
   capacity and a raw write to the whole device. Hand over the exact command instead of running it.
4. **Read the boot:** COM1 at 115200 8N1, or the hardware panel and `/dev/kmsg` over developer SSH.
   Look for `[efi] gop pick`, `[efi] ebs ok`, the `[blk]` root line, `[fs] mounted`, `[dev] … driver=`,
   `[amd-polaris-runtime] stage=`, `[devssh]` and `LOGIT_BOOT_OK`. A `[serial]` loss notice means log
   bytes were dropped.
5. Record board, firmware settings, devices, build options and image hash with every result, and label
   real-hardware evidence as such.

Details: `docs/getting-started/real-hardware.md`, `docs/drivers/gpu.md`, `docs/kernel/diagnostics.md`.
