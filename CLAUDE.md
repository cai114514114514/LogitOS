# Aqua OS — notes for Claude

A from-scratch x86_64 OS kernel (C + nasm), booted via GRUB/Multiboot2, aiming
toward a macOS-style desktop. Real kernel, not a simulation.

## Build / run / test

```sh
make        # -> build/aqua.iso
make run    # QEMU: VGA window + serial on terminal
make test   # headless; asserts kernel prints AQUA_BOOT_OK on serial
make debug  # QEMU frozen with gdb stub on :1234
```

## Toolchain (macOS / Apple Silicon host, x86_64 target)

- Compile: `clang --target=x86_64-elf -ffreestanding` (clang cross-compiles natively)
- Link: **`ld.lld`** — Apple `ld` only emits Mach-O, so the LLVM linker is required (`brew install lld`)
- Assemble: `nasm -f elf64` (32-bit boot code lives in elf64 objects via `bits 32`)
- ISO: `i686-elf-grub-mkrescue` + `xorriso`
- Run: `qemu-system-x86_64` (full emulation on arm64)

## Conventions

- Kernel loads at **1 MiB**; `linker.ld` forces the Multiboot2 header first.
- Boot path: `boot/boot.asm` (32-bit: checks → identity page tables → long mode) → `boot/long.asm` (64-bit) → `kernel_main`.
- `kprintf` fans output to **both** VGA and serial; serial is also the test channel.
- IDE diagnostics about inline-asm constraints / missing headers are false
  positives unless `.clangd` is being honored — the real build uses `-Iinclude`
  and the x86_64 target.

## Roadmap

M1 Boot & Hello ✅ · M2 interrupts + keyboard ✅ · M3 memory (PMM + heap) ✅ ·
M4 multitasking (preemptive scheduler) ✅ · M5 storage (ATA + AquaFS + VFS) ✅ ·
M6 userland (GDT/TSS + ring3 + int 0x80 + ELF loader) ✅ · M7 graphics
(framebuffer + VMM + Aqua desktop) ✅ · M8 window system (font + double-buffer +
PS/2 mouse + draggable windows) ✅ · M9 networking (PCI + e1000 + ARP/IPv4/
ICMP/UDP + DNS + Network app) ✅.

Post-roadmap: per-process address spaces (each app its own PML4; `vmm_new_space`,
`schedule()` switches CR3) and ring-3 fault containment (an app fault kills only
that app; the kernel/desktop survive — `kernel/interrupts.c`).

Key notes:
- `vmm_map_page` does a real 4-level walk; maps the high-MMIO framebuffer and
  user pages. Intermediate table entries carry USER; leaf PTE flags protect
  kernel pages. User images link at 1 GiB (above the identity huge-page region).
- Disk: QEMU `-drive ...,if=ide` primary master; `drivers/ata.c` (PIO
  read+write) + `fs/aquafs.c` + `fs/vfs.c`. AquaFS is a **hierarchical,
  read-write inode FS** (on-disk v3, 4 KiB blocks: superblock, free-block
  bitmap, inode table with direct[12]+single-indirect, directories = inodes of
  dirents). Subdirectories + files up to ~4 MiB; `vfs_*` are path-based. Build
  the image with `tools/mkfs.py` (Makefile `$(DISK)` packs `fsroot/*`, a seeded
  `/docs/`, and each app's `.aex` at the root).
- Userland: `kernel/gdt.c` (TSS rsp0), `boot/enter_user.asm`, syscalls via
  int 0x80 in `kernel/syscall.c`; `user/` builds the ring-3 ELF.
- M8 window system: `tools/genfont.py` -> `include/font8x16.h` (committed, no PIL
  at build). `kernel/fb.c` draws into surfaces (`fb_target`/`fb_blit_surface`)
  with a screen back buffer + `fb_present()`. `drivers/mouse.c` = PS/2 mouse
  (IRQ12); `drivers/rtc.c` = CMOS wall clock (menu bar + Clock app).
- M9 networking: `kernel/pci.c` (0xCF8/0xCFC config) + `drivers/e1000.c` (QEMU
  e1000 8086:100E, MMIO BAR, RX/TX descriptor rings, **polled** — no NIC IRQ).
  `net/` = eth/arp/ip/icmp/udp/dns; `net_poll()` is pumped from the WM loop.
  Static IP (QEMU SLIRP: 10.0.2.15/24, gw 10.0.2.2, DNS 10.0.2.3) in
  `net_cfg` (DHCP hook later). Run/test attach `-netdev user -device e1000`
  (+ `filter-dump` pcap for `make run`). e1000 gotcha: QEMU's `set_rx_control`
  defers the RX-queue flush ~1s, so RX needs a real time base to observe.

## Application platform (on top of M8)
- Apps are `.aex` files on the AquaFS disk = real **ring-3 processes** scheduled
  by M4. `kernel/wm.c` is the window manager AND the GUI/app backend.
- Executable format: `include/aex.h` (header wrapping an ELF), built by
  `tools/mkaex.py`; loaded by `kernel/aex.c` (reuses `elf_load`). Each app links
  at a distinct base (Makefile APP_RULE: clock 0x40000000, textedit 0x41000000,
  monitor 0x42000000, terminal 0x43000000). Single instance per app.
- Process model: `thread_create_user` (sched.c) spawns a ring-3 thread with its
  own kernel stack; `schedule()` sets TSS rsp0; `thread_exit()` reaps it.
  `sched_current_data()` maps the running thread to its `struct app`.
- ABI: `include/aqua_abi.h` (shared with userland `user/aqua.h`). Syscalls via
  int 0x80: GUI create/clear/rect/text/flush, poll_event (key/mouse/close),
  get_arg, get_time, read_file, write_file/delete_file, mkdir,
  dir_count/dir_name (path-scoped listing), net_info/net_ping/net_dns (+ result
  pollers), yield, sysinfo, file_count/file_name (root), exit.
  read/write/delete/mkdir take paths. `syscall.c` routes GUI calls to
  `wm_gui_syscall()` in the app's context. Finder is a directory browser (cwd,
  folders, `..`); Terminal has cd/pwd/mkdir/ls + cat/touch/rm/echo>; TextEdit
  saves with Ctrl+S; Network app pings the gateway + resolves a host (DNS). PS/2
  keyboard supports Ctrl + Shift.
- WM: dynamic windows + per-window surfaces composited each frame; app registry
  scanned from *.aex; Dock launches apps; Finder opens a file with the app whose
  `ext` matches (file association); red close button -> EV_CLOSE -> app exits.
- Adding an app: write `user/<name>.c` (include "aqua.h", define `app_main`),
  add an `APP_RULE` line + the name to `APPS` in the Makefile.
- `tools/qmp_*.py` drive mouse/keyboard over QEMU QMP for screenshots/CI.
Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.
