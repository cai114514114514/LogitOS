# GTX 1050 physical boot-display support — 2026-09-11

## Delivered boundary

LogitOS can now bind a GeForce GTX 1050-family display as `nv-bootfb` while
continuing to use the linear framebuffer established by the card firmware.
This is the useful first physical-PC boundary: keep a visible desktop through
boot, identify the installed card exactly, and publish diagnostics without
resetting a GPU that firmware already made usable.

This is deliberately not a native Pascal graphics driver.  There is no GP107
reclocking, NVIDIA firmware upload, display-head modesetting, command channel,
VRAM allocator, 2D/3D acceleration, CUDA, suspend/resume, multi-monitor control,
or HDMI-audio implementation.  The driver makes no GPU MMIO write, does not map
a GPU BAR for itself, does not enable bus mastering, and requests no IRQ.  A
full native implementation would need the whole firmware, memory-controller,
display-engine, fault and graphics-channel sequence; inventing only a few of
those writes would turn a known-good firmware scanout into a black screen.

## Exact devices

Vendor `10de` is accepted only for these NVIDIA-published GTX 1050 / 1050 Ti
device IDs:

| Device IDs | Product label |
|---|---|
| `1c22`, `1c62`, `1c81`, `1c83`, `1c8d`, `1c91`, `1c92` | GeForce GTX 1050 |
| `1c21`, `1c61`, `1c82`, `1c8c`, `1c8f` | GeForce GTX 1050 Ti |

The source is NVIDIA's current supported-products table for its Linux driver:
<https://download.nvidia.com/XFree86/Linux-x86_64/595.45.04/README/supportedchips.html>.
Adjacent Pascal IDs are not aliases.  In particular `1c90` is an MX150 and
`1cb3` is a Quadro P400, so neither is claimed.

## Probe contract

The probe runs after PCI enumeration and after the framebuffer is already live.
It binds only if all of the following are true:

1. The function is a type-0 PCI display controller, subclass VGA (`03.00`) or
   3D (`03.02`).
2. PCI memory decoding is already enabled.  The driver refuses instead of
   changing the command register.
3. If a PCI Power Management capability exists, PMCSR reports D0.  D1–D3 are
   refused instead of attempting an incomplete wake sequence.
4. The enumerator found at least one non-wrapping memory BAR.
5. `fb_boot_lfb_range()` reports a real Multiboot2/GOP linear framebuffer, and
   its complete byte range is contained by one of this exact function's memory
   BARs.  A virtio RAM scanout, a second GPU's framebuffer, a zero range, and an
   overflowing range are refused.

The success record is machine-greppable:

```text
[nv-bootfb] 0000:01:00.0 10de:1c81 GeForce GTX 1050 mode=passive bootfb=... source=multiboot-lfb bar=... lfb=... cmd=... pm=D0 ...
[nv-bootfb] framebuffer retained; no GPU MMIO, modeset, clocks, DMA, IRQ or 3D
[dev] 0000:01:00.0 10de:1c81 class=03.00.00 vga ... driver=nv-bootfb ...
```

The BAR containment check matters on X79: PCIe slots can contain several GPUs,
and the active boot adapter is a firmware choice.  Dimensions alone cannot say
which PCI function owns the pixels.

The shared PCI enumerator now serializes the complete BAR-sizing transaction.
It first disables I/O and memory decode and confirms the Command readback before
writing the all-ones sizing value.  It then restores every low/high BAR half and
the original Command value, with readback checks.  Any ignored disable or failed
restore leaves decode off and publishes no BAR resource, so this passive driver
cannot inherit a resource measured while the GPU was still decoding the probe.
The host safety gate covers the positive path plus independent mutation controls
for ignored decode-disable, partial decode-disable without recovery, failed
32-bit/64-bit-high BAR restore and failed Command restore.

## Evidence

`make BUILD=build-x79-pascal test-nvidia-pascal-host` runs the production probe
with instrumented PCI/framebuffer seams.  It currently passes 56 checks:

- all 12 published IDs and the exact declarative match table;
- rejection of MX150, Quadro, wrong vendor/class/header, disabled memory decode,
  D3, missing/wrapped BARs, absent/virtio LFB, other-GPU LFB and wrapped LFB;
- VGA and 3D-controller forms, D0 and an absent optional PM capability;
- zero config writes, `dev_enable`, BAR maps, IRQ requests and bus-master setup.

Two negative controls are prerequisites of that positive gate and were watched
failing exactly once each:

```text
FAIL: adjacent MX150 ID is not accepted as GTX 1050
NV_BOOTFB: 56 checks, 1 failures

FAIL: D3 GPU is refused instead of being woken by an unsafe config write
NV_BOOTFB: 56 checks, 1 failures
```

`make BUILD=build-x79-pci-bar test-pci-bar-safety-host` separately verifies the
enumerator prerequisite above.  Its positive model passes all five transactions;
each of the five compiled mutation controls fails its single expected safety
assertion.  The complete PCI unit suite also passes 58 checks after the change.

`make BUILD=build-x79-pascal-synth PASCALVERIFY=1
test-nvidia-pascal-synthetic-guest` builds an isolated test kernel.  It admits
QEMU stdvga `1234:1111` under a `TEST-ONLY` alias, then boots both GRUB/BIOS LFB
and the in-tree UEFI loader/GOP path.  The gate requires the real LFB range to
fit the emulated display BAR, `driver=nv-bootfb` to appear after declarative
binding, the no-takeover line above, and a post-bind QMP scanout whose dimensions
match the guest record and whose sampled pixels are not blank.  This proves the
integration and preservation mechanism; QEMU does not emulate GP107 and this
result is not physical GTX 1050 evidence.

Both isolated guest runs pass.  BIOS and UEFI each retained a `1280x800`
scanout after binding, with all 16,000 sampled pixels lit and 228 sampled
colours.  Their records are under
`build-x79-pascal-synth/nvidia-pascal-guest/{bios,uefi}/`: `serial.log` contains
the PCI/BAR/bind evidence, `desktop.ppm` is the post-bind screen capture, and
`result.json` records the measured dimensions and sample counts.
`summary.json` records both runs and explicitly marks
`physical_gtx1050_verified` false.

## Physical acceptance still required

A real GTX 1050 result requires booting the ordinary image on the X79 machine.
Keep the card selected as the firmware's primary display and capture the three
records shown above.  A success means the desktop remains visible and the LFB
is proven to lie inside that card's BAR.  An unclaimed device or a refusal line
is a useful exact result; it identifies the missing condition without writing
to the GPU.  It does not establish native modesetting or acceleration.
