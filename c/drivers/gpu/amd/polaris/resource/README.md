# Polaris boot resources

This directory closes the missing **discovery and installed-file input** part
of the boot path. It does not turn a BAR range into a VRAM allocation.

The production path is root VFS mount → `dev_probe_all` → `amd_bootfb_probe`
→ save the actual boot-display device → `amd_bootfb_probe_resources`
→ `polaris_resources_probe_device`.
It runs before the scheduler, outside the framebuffer graphics lock. The device
entry reads fresh PCI identity, class, header, BAR addresses/types and a bounded
PM capability chain before mapping anything. The existing read-only Polaris
probe validates the complete boot framebuffer against that same function's
BAR0, then samples BAR5. No register-write API is linked into this new path.

## VBIOS and declared reservations

`device.c` copies at most the first 256 KiB of the enabled VRAM aperture into
ordinary RAM, following the already-POSTed dGPU path in Linux v6.12
[`amdgpu_bios.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c).
It does not enable a ROM BAR, access an indirect ROM port, run ATOM commands or
assume arbitrary physical shadow RAM belongs to this device.

`atom.c` validates the ROM length, PCIR identity `1002:67df`, ATOM ROM header,
master data table and the `VRAM_UsageByFirmware` table. Versions 1.4 and 1.5 use
KiB offsets relative to MC VRAM base; older byte-address versions are refused.
The firmware range and preceding v1.5 interpreter scratch range are separate.
A v1.5 zero-start request is marked `driver_allocation_required`: the address
specified by ATOM is reported but no allocation is made. Unsupported SR-IOV
policy, reserved policy, truncated tables and out-of-VRAM ranges are refused.

These layouts and policies are defined in Linux v6.12
[`atombios.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/atombios.h)
(`ATOM_ROM_HEADER`, `ATOM_MASTER_LIST_OF_DATA_TABLES`,
`ATOM_VRAM_USAGE_BY_FIRMWARE[_V1_5]`). Its comment explicitly says the usage
table is filled at VBIOS runtime. Neither a file's signature nor finding its
copy in VRAM proves that its reservation list is current or exhaustive.

## Installed firmware inputs

`files.c` reads these seven common files from `/lib/firmware/amdgpu/`:

- `polaris10_sdma.bin`, `polaris10_sdma1.bin`
- `polaris10_ce.bin`, `polaris10_pfp.bin`, `polaris10_me.bin`
- `polaris10_mec.bin`, `polaris10_rlc.bin`

The eighth file is selected by the actual SMU security key, and for key 1 by
the actual PCI revision:

| Selection for PCI 67df | SMC file |
| --- | --- |
| key 0 | `polaris10_smc_sk.bin` |
| key 1, revisions e3/e4/e5/e7/ef | `polaris10_k_smc.bin` |
| key 1, revisions e1/f7 | `polaris10_k2_smc.bin` |
| key 1, other revisions | `polaris10_smc.bin` |

Names follow Linux v6.12
[`amdgpu_cgs.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_cgs.c)
and the `ASICID_IS_P20/P30` definitions in
[`amdgpu.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu.h).
Files must be root-owned regular files, without group/other write permission,
and at most 2 MiB each. A bounded read detects shrinking/growing files; the
existing parser validates the expected per-kind header, IP version and payload
layout. This is **format validation, not CRC verification or authentication**.
Installation must retain trusted upstream provenance and the applicable AMD
firmware license. This code never downloads or generates firmware.

The loader returns owned immutable file bytes, usable as the inputs to the
existing synchronous runtime entry; release is explicit even on partial error.
Discovery independently loads both key candidates, records each file's result,
then releases all RAM. Reading the actual SMU security selector would require
writing an indirect INDEX register without the GPU lease, so discovery leaves
`security_key_unknown` set. `firmware_mask[0/1] == 0xff` means eight structural
file checks passed for that candidate, not that its key matches the device.

## The remaining ownership fact

Current LogitOS boot data gives the framebuffer and system memory map, but no
complete GPU VRAM reservation inventory or exclusive allocated GPU arena.
Neither PCI BAR sizes, MC/HDP apertures, an idle SDMA bit nor ATOM's usage table
can reveal every live GOP allocation, additional scanout/cursor buffer and
firmware scratch allocation. UEFI `AllocatePages` allocates system RAM, not an
exclusive slice of a graphics card's BAR. The existing CPU-visible scanout
therefore remains active and `ownership_missing` stays set even when every
discovery check succeeds. No call to `polaris_desktop_start` is made here.

A real handoff would need an exact BDF/device/revision, MC/aperture mapping,
all preserved scanout and firmware ranges, an immutable runtime VBIOS copy
with origin, and a page-aligned arena granted by the allocator that actually
owns that VRAM. Its lifetime must cover all DMA including failure quarantine.
Alternatively, an actual display/GMC takeover must establish those resources
before allocating; copying Linux's free-space arithmetic without its takeover
and memory-manager lifecycle is insufficient. Linux v6.12
[`amdgpu_gmc.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c)
and [`gmc_v8_0.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c)
track VBIOS stolen memory within that broader lifecycle. No invented handoff
boolean or guessed BAR tail is accepted as a substitute in this directory.

## Validation

Run `make -f tests/gpu/amd/polaris/resource/gate.mk
BUILD=build/drivers/expansion/amd CC=clang test-polaris-resources-host` on one
shell line. The gate links the actual production entry, parser and file loader;
only kernel PCI mappings, heap and VFS services are replaced in the host test.
Literal fixtures cover decoded reservations, bounded truncation, wrong PCIR,
file selection, real bundle-plan consumption, metadata/read/allocation errors,
fresh BAR/PM refusal before mapping, split discovery results and RAM release.
The watched mutation drops the firmware reservation and must fail both the
parser and device-consumer oracles. ASan and UBSan are enabled. This establishes
software behavior only; it is not an RX 580 hardware acceleration result.
