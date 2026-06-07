---
title: Aqua OS on bare metal — driver roadmap (M24)
status: spec (roadmap)
date: 2026-06-07
---

# Aqua OS on bare metal — driver roadmap (M24)

## Goal

Boot Aqua OS to its desktop on a **real machine**, not just QEMU. Target box:

- **CPU**: Intel Xeon E5 (x86-64, multi-core, has PS/2-less modern chipset)
- **GPU**: NVIDIA GTX 1050 — used as a *firmware framebuffer only* (see constraints)
- **Storage**: 120 GB NVMe SSD (PCIe)
- **Input**: USB keyboard/mouse only (no PS/2 port)
- **Debug**: motherboard serial header (COM, 16550 UART) — the dev lifeline
- **Boot**: legacy BIOS / CSM available (GRUB Multiboot2 works)

This is **M24**, decomposed into per-driver milestones (D0–D3 + cross-cutting).
Each driver gets its own detailed spec → plan → gated implementation later; this
document is the roadmap that fixes the architecture, ordering, and key decisions.

## Hard constraints (the reality, decided up front)

1. **The GTX 1050 is NOT a driver target.** There is no realistic path to an
   NVIDIA modesetting driver (closed, undocumented). On bare metal the display is
   a **firmware-provided linear framebuffer**: VBE under legacy BIOS (GRUB already
   requests a 1280×800×32 mode via the Multiboot2 framebuffer-request tag), or GOP
   under UEFI. Aqua's existing `fb.c` multiboot-tag-8 LFB path IS this path. The
   1050 runs as an **unaccelerated framebuffer** — pixels yes, 3D/HW-composite no.
   This is good news: display on bare metal is mostly *already implemented*.

2. **No PS/2 → input needs USB, eventually.** Real USB HID is a large stack
   (xHCI + USB core + HID). **Bridge:** BIOS "USB Legacy Support" uses SMM to
   emulate a USB keyboard as an i8042/PS/2 device. As long as Aqua **does not
   initialize the xHCI controller**, the existing PS/2 driver keeps receiving the
   keyboard. So D0 gets a keyboard for free; the real USB stack is deferred to D2
   (needed for a reliable mouse — legacy emulation is keyboard-biased).

3. **Bare-metal debugging is a different world.** No QMP, no `-snapshot`, no
   instant reboot; a triple fault is a black screen. The serial header (16550 at
   COM1 0x3F8, existing `serial.c`) is the primary debug channel — `kprintf`
   already fans to serial. Every driver milestone logs aggressively over serial.

## Architecture principle: ONE kernel, two targets (runtime probe)

Aqua keeps running on QEMU **and** runs on bare metal from the *same build*. The
kernel **probes at runtime** and picks the driver:

- Display: virtio-gpu if present → else the multiboot LFB (VBE/GOP) framebuffer.
- Storage: virtio-blk if present → else NVMe → else AHCI/ATA.
- Input: PS/2 (real or BIOS-legacy-emulated) → later, USB HID.
- NIC: virtio-net/e1000 (QEMU) → else the real NIC driver.

This keeps **QEMU as the daily dev + CI environment** (fast, scriptable, the whole
existing `make test*` suite) and bare metal as the deployment target. No fork, no
`#ifdef BAREMETAL` — a `blkdev`/`fbdev`/`netdev` indirection layer chooses the
backend. (`blkdev.c` already abstracts virtio-blk vs ATA — extend that pattern.)

## Current baseline (what exists vs what's QEMU-only)

Reusable on bare metal as-is or with hardening:
- **Boot**: GRUB Multiboot2, legacy BIOS — works on real CSM hardware. FB-request
  tag present; FB tag optional so it boots with no framebuffer too.
- **Serial** (`serial.c`, COM1 16550) — works on a real COM port unchanged.
- **Framebuffer** (`fb.c`) — multiboot tag-8 LFB path maps the high-MMIO LFB and
  draws; this is the bare-metal display path.
- **Interrupts**: PIC + **IOAPIC** (`ioapic.c ioapic_route`) + **LAPIC** + **ACPI
  MADT** parse (LAPIC base, IOAPIC addr, CPU topology, ISA-IRQ→GSI overrides) +
  SMP bringup. Real-hardware-shaped already.
- **PCI** (`pci.c`, 0xCF8/0xCFC) — config access works; but `pci_find` only scans
  bus 0 and matches by vendor/device. Needs full multi-bus enumeration + class-code
  matching for NVMe/NIC discovery.
- **PS/2** (`keyboard.c`, `mouse.c`) — works against BIOS legacy-USB emulation.
- **Timer**: PIT + RTC + LAPIC timer.

QEMU-only (won't bind to real hardware): virtio (gpu/blk/net), e1000 (QEMU
82540EM specifically).

Gaps to fill: **NVMe**, **USB (xHCI+core+HID)**, **real NIC**, **MSI/MSI-X**,
full **PCI(e) enumeration + ECAM**, **ACPI shutdown/reset (FADT)**, **timer
calibration** (TSC/HPET, since real PIT/LAPIC frequencies must be measured).

---

## Milestones

### D0 — First light (boot + framebuffer + serial + keyboard)

**Goal:** the real box powers on, prints the boot log over serial, draws the Aqua
desktop on the 1050's framebuffer, and takes keyboard input. Proves boot + display
+ debug + basic input with **minimal new code** — mostly hardening existing
fallback paths against real-hardware quirks.

**Reuse:** GRUB Multiboot2 (USB stick, BIOS/CSM); `serial.c`; `fb.c` LFB path;
PS/2 `keyboard.c` via BIOS legacy-USB emulation; PIC/IOAPIC/LAPIC/ACPI.

**New / hardening:**
- A bootable **USB-stick image** (`grub-mkrescue` already makes an ISO that
  `dd`s to USB; verify El Torito boots on the box, or build a GRUB USB image).
- **FB robustness**: real VBE may hand back a mode whose stride ≠ width×bpp, a
  different bpp (24 vs 32), or a different resolution than requested. `fb.c` must
  read pitch/bpp/format from the multiboot tag (not assume) and handle them.
- **Real-hardware boot survival**: don't assume QEMU memory map — trust the
  Multiboot2 memory-map tag for usable RAM; verify the PMM uses it.
- **Interrupt routing on real IOAPIC** (the keyboard IRQ via legacy emulation
  arrives as ISA IRQ1 → GSI; verify the ACPI override path).
- **Watchdog/sanity**: aggressive serial logging at each init step so a hang is
  locatable.

**Risks:** GRUB not booting (CSM config), VBE giving an odd mode, legacy-USB
keyboard not enabled in BIOS, a real-hardware fault in early init invisible
without serial (mitigated by serial-first bring-up).

**Done when:** desktop renders on the monitor + serial shows `AQUA_BOOT_OK` and
the WM log; a keypress reaches an app. Mouse may be absent until D2.

### D1 — NVMe storage

**Goal:** read/write the 120 GB NVMe SSD; mount AquaFS on a real GPT partition so
the system has persistent storage on metal.

**Reuse:** `blkdev.c` block abstraction (virtio-blk/ATA already plug in here);
AquaFS + VFS unchanged (they sit above `blkdev`).

**New:**
- **Full PCI(e) enumeration**: walk all buses/devices/functions, match NVMe by
  class code (01h mass-storage / 08h NVM / progIF 02h), read BAR0 (64-bit MMIO).
- **PCIe ECAM** (MMIO config) if the box exposes extended config (ACPI MCFG
  table) — needed for some devices; legacy 0xCF8 may suffice for NVMe BAR0.
- **NVMe driver**: map the controller registers (CAP/CC/AQA/ASQ/ACQ), create the
  admin queue + one I/O submission/completion queue pair, ring doorbells, build
  PRP lists for data, issue Identify + Read/Write commands, handle completions.
  INTx via IOAPIC first; **MSI/MSI-X** as a follow-on (most NVMe needs MSI-X for
  best behavior, but INTx or polling works to start).
- **GPT partition parse** (read LBA1 GPT header + entries) to find the AquaFS
  partition; `blkdev` exposes it with an LBA offset.

**Risks:** MSI-X likely required on some controllers (fallback: poll the
completion queue); 64-bit BARs + ECAM; PRP/boundary edge cases; AquaFS was built
for a whole-disk image, now needs a partition offset.

**Done when:** AquaFS reads/writes survive a power cycle on the NVMe partition;
the shell + Finder operate on real persistent files.

### D2 — USB stack (xHCI + USB core + HID)

**Goal:** real USB keyboard **and mouse**, freeing Aqua from BIOS legacy
emulation (and giving reliable mouse input the desktop needs).

**New (the largest effort):**
- **xHCI host controller** driver: map MMIO, reset, set up the command ring,
  event ring, DCBAA + device contexts, port power/reset, handle port-status
  change events.
- **USB core**: enumerate devices (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIG),
  parse config/interface/endpoint descriptors, manage control/interrupt transfers.
- **HID**: boot-protocol keyboard + mouse (the simple fixed report formats), feed
  events into the existing WM input path (`wm_key`, `wm_mouse_event`).
- Take ownership from BIOS (the xHCI legacy handoff: BIOS-owned → OS-owned via the
  USBLEGSUP capability) — once Aqua owns xHCI, legacy PS/2 emulation stops, so the
  HID driver must be live before that handoff completes.

**Risks:** the single biggest driver; legacy-handoff timing (don't lose the
keyboard mid-transition); interrupt (MSI-X) vs polling; hub support if the kbd/
mouse are behind an internal hub.

**Done when:** USB keyboard + mouse drive the desktop with the xHCI controller
OS-owned; PS/2/legacy path no longer needed.

### D3 — Real NIC (deferred; not needed to reach the desktop)

**Goal:** networking on the onboard NIC.

**New:** identify the chip (Intel igb/e1000e or Realtek RTL8168/8111) by PCI ID;
write the matching driver (RX/TX descriptor rings, MMIO, link setup). The existing
`net/` stack (eth/ip/tcp/udp/dns/http/tls) sits above a `netdev` interface — only
the device driver is new. e1000e is close to the existing e1000; Realtek is a
fresh driver.

**Done when:** ping/DNS/HTTP work over the real NIC (the `net` coreutil + Browser).

### Cross-cutting (threaded through D0–D3 as needed)

- **PCI(e) enumeration + ECAM** (D1 prerequisite; reused by D3).
- **MSI/MSI-X** (D1/D2/D3): modern devices prefer it; provide an allocator that
  programs the MSI-X table + maps vectors to LAPIC.
- **ACPI FADT**: clean **shutdown** (via ACPI) + **reset**; currently only MADT is
  parsed. Real hardware needs a real power-off, not QEMU's debug exit.
- **Timer calibration**: PIT/LAPIC-timer/TSC frequencies are fixed in QEMU but
  must be **measured** on real hardware (calibrate LAPIC timer against PIT/HPET;
  use HPET via ACPI if present).
- **CPU/firmware quirks**: trust the Multiboot2 memory map; handle real E5
  topology (already via ACPI MADT); microcode/cache are firmware's job.

---

## The bare-metal dev loop

1. `make` builds the ISO; write to USB (`dd` the ISO, or a GRUB USB image).
2. Boot the E5 box from USB (BIOS/CSM); **serial cable** (USB↔TTL or serial card)
   to a dev machine running a terminal (115200 8N1) captures `kprintf`.
3. Iterate: every driver bring-up is serial-log-driven. Keep QEMU as the fast
   inner loop (`make test*`), use the real box for the bare-metal-specific steps
   (FB mode, NVMe, USB) that QEMU can't exercise.
4. A second-channel safety: a `kpanic` that prints to serial + halts (not triple
   faults) so failures are diagnosable.

## Scope & deferrals

- **In scope:** boot to interactive desktop on the E5 box, persistent NVMe
  storage, USB keyboard+mouse, optional networking.
- **Deferred / out of scope:** GPU acceleration (framebuffer only — forever, for
  the 1050); UEFI boot (default to BIOS/CSM unless the box is UEFI-only — then a
  separate UEFI-boot spec); audio; multiple monitors; suspend/resume; power
  management beyond shutdown; SATA/AHCI (only if a SATA disk is added).
- **Each driver (D0–D3) gets its own detailed spec → plan → gated implementation.**
  This roadmap fixes the ordering and the cross-driver decisions only.

## Testing strategy

- **QEMU (unchanged):** the entire existing `make test / test-shell / test-aqs* `
  suite stays green — the dual-target architecture means bare-metal work must not
  regress QEMU. Every milestone re-runs the QEMU suite.
- **Bare metal:** per-milestone serial-log assertions (boot reaches a marker,
  NVMe identify succeeds + a read-back matches, a USB HID report arrives). Manual
  + photographed-screen verification for the framebuffer/desktop. No automated CI
  on real hardware initially; the serial log is the evidence.

## Recommended first step

**D0 (first light)** — it's the cheapest (mostly hardening existing fallback
paths), it stands up the serial-debug + framebuffer foundation every later driver
needs, and it produces a visible, motivating result (Aqua's desktop on real
hardware). NVMe (D1) and USB (D2) are blind without it.
