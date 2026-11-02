# Running LogitOS in QEMU

LogitOS provides BIOS and UEFI launch targets that build a boot image and attach a writable LogitFS disk containing applications and user data. The interactive defaults select four virtual CPUs, 1 GiB of RAM, a VirtIO GPU, an e1000 network adapter, and serial output on the launching terminal. BIOS desktop sessions also configure CoreAudio and an emulated Intel HDA device. The launch settings live in `Makefile` and `tests/uefi.mk`.

## Start a BIOS session

Run commands from the repository root after installing the prerequisites in [Building LogitOS](building.md):

```sh
make run
```

On Linux, clear the default macOS audio arguments:

```sh
make run QEMU_SND=
```

Choose the launch target according to the session you need:

| Command | Boot image | Root disk | Purpose |
|---|---|---|---|
| `make run` | `ISO` | `RELEASE_DISK` by default | Desktop session |
| `make run-dev` | `ISO` | `DISK` | Development session with test programs and probes |
| `make run-uefi` | `ESP_IMG` | `DISK` | UEFI session |
| `make debug` | `ISO` | `DISK` | BIOS session paused for GDB |
| `make shot` | Default BIOS ISO | Default development disk | Headless screenshot capture |

The BIOS image contains the boot stages from `c/boot/bios/preload.asm` and `c/boot/bios/loader.asm`, followed by the kernel. `tools/mkiso.py` packages these components. The retained `make iso-grub` target uses `grub.cfg` for the GRUB fallback image.

The desktop targets launch through `tools/agent_session.py`. When the model environment file or credentials are absent, the wrapper prints `MODEL_SESSION_OFFLINE` and boots the desktop in offline mode.

## Images and persistent state

`BUILD` defaults to `build`. These filenames identify generated artifacts; their definitions are in `Makefile` and `tests/uefi.mk`.

| Variable or producer | Default filename under `BUILD` | Contents |
|---|---|---|
| `KERNEL` | `kernel.elf` | Linked kernel with debug information |
| `ISO` | `logit.iso` | BIOS boot stages and kernel |
| `ESP_IMG` | `esp.img` | FAT boot volume containing the UEFI loader and kernel |
| `DISK` | `disk.img` | Development LogitFS root image |
| `RELEASE_DISK` | `release.img` | Desktop root image filtered through `RELEASE_EXCLUDE` |
| Kernel link recipe | `kernel.map` | Linker address map |
| Default `QEMU_NET` | `net.pcap` | Captured network packets |

A plain build produces the BIOS ISO:

```sh
make
```

To prepare the development root image:

```sh
make build/disk.img
```

To prepare the desktop root image:

```sh
make release-disk
```

The launch targets build their image prerequisites before starting QEMU. After changing an application, `make run-dev` rebuilds its packaged development image.

The BIOS desktop recipe selects its root image through `RUN_DISK`. Assigning `DISK` on the make command line also makes that image the default `RUN_DISK`; assigning `RUN_DISK` selects it directly.

Interactive sessions write changes to the attached root image. During repacking, `MKFS_PROFILE` retains selected user-state directories. `tools/mkfs.py` and `tools/disk_profile.py` validate a private snapshot and copy the retained state into the replacement image. Directories configured for merging keep existing file contents and metadata while accepting newly packaged siblings.

Stop the VM before rebuilding or reusing its disk image. The desktop wrapper and image packer use `tools/disk_guard.py`, which combines a companion lock with an ownership check through host `lsof`.

## Boot through UEFI

Build and launch the UEFI configuration with:

```sh
make run-uefi
```

Build its boot volume alone with:

```sh
make esp
```

`tests/uefi.mk` defines the interactive firmware inputs:

| Variable | Default host firmware path |
|---|---|
| `OVMF_CODE` | `/usr/share/OVMF/OVMF_CODE_4M.fd` |
| `OVMF_VARS_SRC` | `/usr/share/OVMF/OVMF_VARS_4M.fd` |

The recipe attaches the firmware code image read-only and copies the variables template to `UEFI_VARS`, whose default filename is `OVMF_VARS.fd` under `BUILD`. This writable copy holds firmware state across launches. Make refreshes it when the template prerequisite is newer.

For the Homebrew firmware layout listed in the same fragment:

```sh
make run-uefi \
  OVMF_CODE=/opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  OVMF_VARS_SRC=/opt/homebrew/share/qemu/edk2-i386-vars.fd
```

Set both variables to the matching firmware files installed on your host.

The UEFI recipe selects the `q35` machine, attaches the FAT boot volume through AHCI, and connects the LogitFS root disk through VirtIO block. `tools/mkesp.py` builds the default boot volume as a whole-volume FAT filesystem, commonly called a superfloppy. Its generated contents include the loader named `BOOTX64.EFI` and the kernel named `LOGIT.ELF`.

An older comment in `tests/uefi.mk` describes UEFI launch as changing firmware alone. The current recipe also selects the development disk and invokes QEMU directly; the BIOS desktop target selects the release disk and uses the model-session wrapper.

## CPU, memory, and acceleration

`Makefile` defines these interactive defaults:

| Variable | Default arguments |
|---|---|
| `QEMU_RAM` | `-m 1G` |
| `QEMU_SMP` | `-smp 4 -accel tcg,thread=multi` |
| `QEMU_CPU` | `-cpu max` |
| `QEMU_RTC` | `-rtc base=localtime` |

The macOS/Apple Silicon configuration uses TCG to emulate the x86-64 guest. The default enables multithreaded TCG. Linux launches also use this default unless overridden.

On an x86-64 Linux host with usable KVM, select the accelerator and CPU model together:

```sh
make run \
  QEMU_SMP="-smp 4 -accel kvm" \
  QEMU_CPU="-cpu host" \
  QEMU_SND=
```

`tests/boot/run-sysroot-device.sh` uses the same KVM/host and TCG/max pairings.

The Makefile selects `-cpu max` for TCG to expose RDRAND/RDSEED, which supply hardware entropy to the kernel random-number generator. The TLS client checks the generator’s strength in `c/net/tls/tls.c`.

An older Makefile comment describes 1 GiB as a memory ceiling. Current `c/kernel/mm/phys/pmm.c` builds supervisor mappings for high RAM, and `c/kernel/mm/mmhost.h` translates high physical addresses through that direct map. `tests/boot/run-wide-memory.py` defines BIOS and UEFI configurations with 512 MiB, 2 GiB, and 8 GiB. The interactive default remains 1 GiB.

## Display options

The default `QEMU_GPU` arguments are:

```text
-vga none -device virtio-gpu-pci,xres=1920,yres=1200
```

The resolution arguments request the display dimensions. For a smaller framebuffer:

```sh
make run QEMU_GPU="-vga none -device virtio-gpu-pci,xres=1280,yres=800"
```

For a higher-density display:

```sh
make run QEMU_GPU="-vga none -device virtio-gpu-pci,xres=2560,yres=1600"
```

Both examples appear in `Makefile`. `c/kernel/gui/fb/fb.c` selects UI scaling from the framebuffer dimensions against a 1280 × 800 point design canvas.

The window manager selects a hardware cursor when available and reports its cursor path on serial. The linear framebuffer path also supports a software overlay: `c/kernel/gui/fb/fb.c` and `c/kernel/gui/fb/fb_overlay.c` restore the old cursor rectangle from the back buffer and paint the new position.

`DISP` controls the host display backend:

| Override | Presentation |
|---|---|
| `DISP=gtk,gl=off` | GTK with OpenGL disabled |
| `DISP=sdl` | SDL |
| `DISP=none` | Headless display |

For a headless desktop session:

```sh
make run DISP=none QEMU_SND=
```

`Makefile` converts `DISP` into `QEMU_DISP`. Choose a backend supplied by your host QEMU installation.

For the optional hardware and performance panel, use a dedicated build directory:

```sh
make BUILD=build-hwhud HWHUD=1 run
```

`c/kernel/diag/hwhud.c` displays CPU, clock, compositor, network, audio, input, and serial statistics. Clicking its title row collapses or expands it. The panel yields the screen while system overlays are visible, following the policy in `c/kernel/diag/hwhud_panel.h`.

## Networking

The default `QEMU_NET` connects an e1000 adapter to QEMU user networking and captures packets to `net.pcap` under `BUILD`.

`c/net/core/netcfg.c`, called during initialization from `c/net/core/net.c`, selects saved static settings or DHCP. After a failed boot DHCP attempt under a detected hypervisor, it applies these fallback values:

| Setting | Value |
|---|---|
| Guest IPv4 address | `10.0.2.15` |
| Subnet mask | `255.255.255.0` |
| Gateway | `10.0.2.2` |
| DNS resolver | `10.0.2.3` |

The previous unconditional fallback applied these addresses after DHCP failure on every machine. The current policy conditions that fallback on hypervisor detection.

`c/net/core/dhcp.c` gives the boot attempt one four-second deadline covering both link negotiation and the DHCP exchange, with up to three seconds allocated to the exchange. Background acquisition continues after boot with randomized exponential backoff and restarts when the link comes up. A DHCP NAK answering an outstanding request clears the refused configuration and its routes before acquisition restarts.

The host is reachable through `10.0.2.2` with this user-networking setup; `tests/boot/run-net-bench.sh` uses that address to reach its host HTTP server.

For host-to-guest SSH access, apply the forwarding configuration used by `tests/boot/run-ssh-test.sh`:

```sh
make run QEMU_NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2299-10.0.2.15:22 -device e1000,netdev=n0"
```

This maps host loopback port 2299 to guest port 22. The assignment replaces the complete network argument list. To retain packet capture, include the capture filter from the default `QEMU_NET` definition. Configure the guest service as described in [SSH](../networking/ssh.md).

## Audio

The BIOS desktop targets consume `QEMU_SND`. Its default combines CoreAudio with an emulated Intel HDA output device:

```text
-audiodev coreaudio,id=snd0 -device intel-hda -device hda-output,audiodev=snd0
```

Clear these arguments with:

```sh
make run QEMU_SND=
```

For WAV capture, use the override documented in `Makefile`:

```sh
make run QEMU_SND="-audiodev wav,id=snd0,path=out.wav -device intel-hda -device hda-output,audiodev=snd0"
```

Here `out.wav` names the generated host capture. List the audio backends supplied by your QEMU installation with:

```sh
qemu-system-x86_64 -audiodev help
```

`QEMU_SND` supplies both the backend and device arguments. The audio test configuration uses `QEMU_AUDIO` for device arguments and constructs its capture backend in the test launcher, as documented in `Makefile`.

`HWHUD=1` builds also arm a one-second, 440 Hz diagnostic tone. `c/kernel/audio/boot_test_tone.c` starts playback on a kernel thread after at least five seconds have elapsed since its first panel poll. A boot module named `NOSOUND` selects the skipped state.

## Serial console and boot diagnostics

The interactive recipes pass `-serial stdio`, connecting the guest UART to the launching terminal. `c/drivers/char/serial.c` configures COM1 for 115200 baud, eight data bits, no parity, and one stop bit. The previous setting was 38400 baud; the BIOS stages in `c/boot/bios/preload.asm` and `c/boot/bios/loader.asm` now use the same 115200 baud divisor as the kernel.

Kernel serial output enters a 64 KiB transmit ring. Writes and timer ticks drain available UART FIFO slots so producers can continue while transmission proceeds. `c/kernel/cpu/irq/interrupts.c` calls the periodic drain on each CPU’s timer tick.

The window manager launches the console login program. When the account store is absent, `c/apps/coreutils/login.c` prints that condition and starts a root shell. With an account store present, it follows the authentication path.

Useful serial landmarks include:

| Output | Meaning |
|---|---|
| `[logitos] long mode, C kernel running` | Entry into `kernel_main` |
| `[fs] mounted` | Successful root filesystem mount |
| `LOGIT_BOOT_OK` | Late kernel initialization reached with the root mounted |
| `[wm] desktop live; launching apps as ring-3 processes` | Entry into window-manager startup |

The first three messages come from `c/kernel/init/kmain.c`; the last comes from `c/kernel/gui/wm.c`.

Boot also emits `[hw]` records for device bindings, CPU topology, memory, root selection, and startup milestones through `c/kernel/diag/hwreport.c`. When filesystem writes succeed, `c/kernel/diag/hwreport_log.c` saves the report in numbered boot logs under the guest directory `/var/log`.

## Screenshots and QMP

The BIOS desktop and UEFI launch recipes expose QMP at the host Unix socket `/tmp/logit-qmp.sock`. `tests/qmp/qmp_ui.py` provides screenshot and keyboard and pointer injection helpers.

To capture the scanout from a fresh headless boot:

```sh
make shot
```

`tools/shot.sh` writes the generated file `desktop.png` under the default build directory. It boots the development disk with snapshot writes, a 1280 × 800 VirtIO display, four virtual CPUs, and multithreaded TCG. The Makefile passes its RAM arguments to the script.

This capture helps distinguish guest rendering from host window presentation. Compare its disk, display, and CPU settings with the session being investigated when interpreting the image.

## Debugging with GDB

Start the BIOS guest paused:

```sh
make debug
```

The recipe adds `-s -S`, exposing the GDB stub on TCP port 1234 and stopping the virtual CPUs at startup.

In another terminal, use GDB with x86-64 ELF support:

1. Load the generated kernel ELF named by `KERNEL` as the symbol file.
2. Connect to the remote target at localhost, port 1234.
3. Set a hardware breakpoint at `kernel_main` in `c/kernel/init/kmain.c`, or at the earlier `logit_native_start` entry in `c/boot/long.asm`.
4. Resume execution.

`Makefile` compiles kernel C with `-g`, assembles ELF objects with DWARF debug information, and emits `kernel.map` under `BUILD`. Use symbols from the same build as the booted ISO. `linker.ld` places the kernel at 32 MiB and names `logit_native_start` as its entry.

The debug recipe accepts the RAM, CPU, accelerator, RTC, GPU, and network variables and attaches the development disk.

## Limitations

- `run-uefi` and `debug` bypass the model-session wrapper and omit `QEMU_SND`. The debug recipe also omits `QEMU_DISP` and the interactive QMP endpoint.
- The UEFI recipe disables QEMU file locking for its ESP and root disk attachments and bypasses the desktop wrapper’s disk guard. Stop the VM before rebuilding or reusing those images.
- BIOS desktop and UEFI sessions share a fixed QMP socket. Concurrent sessions need distinct endpoints in their launch arguments.
- `tools/shot.sh` hardcodes the default build directory for its input images and default output. A custom `BUILD` assignment changes the make prerequisites while the script continues using those paths.
- Hardware cursor pixels can be absent from screenshots. `tests/qmp/qmp_ui.py` uses guest serial pointer reports when available.
- Serial bytes can be lost when the transmit ring fills or a blocking drain abandons a stuck UART. A later enqueue reports the lost-byte count when space permits.
- The TLS client refuses handshakes when the kernel reports weak randomness. CPU feature configuration can affect that check.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | BIOS image rules, disk selection, QEMU defaults, launch targets, and debug flags |
| `tests/uefi.mk` | UEFI images, firmware variables, and interactive launch recipe |
| `c/boot/bios/preload.asm` | BIOS first stage and early serial setup |
| `c/boot/bios/loader.asm` | BIOS kernel loading and serial setup |
| `tools/mkiso.py` | Native BIOS ISO construction |
| `tools/mkesp.py` | FAT UEFI boot-volume construction |
| `grub.cfg` | GRUB fallback configuration |
| `tools/mkfs.py` | LogitFS image packing and preservation options |
| `tools/disk_profile.py` | Validation and copying of retained state |
| `tools/disk_guard.py` | Disk lifecycle locks and open-file checks |
| `tools/agent_session.py` | Desktop model-session wrapper and offline fallback |
| `tools/shot.sh` | Headless boot and screenshot capture |
| `tests/boot/run-sysroot-device.sh` | KVM/host and TCG/max configurations |
| `tests/boot/run-wide-memory.py` | BIOS and UEFI memory configurations |
| `tests/boot/run-net-bench.sh` | Host-server access through user networking |
| `tests/boot/run-ssh-test.sh` | Loopback SSH forwarding configuration |
| `tests/qmp/qmp_ui.py` | QMP screenshots, input injection, and pointer helpers |
| `c/drivers/char/serial.c` | COM1 configuration and buffered transmission |
| `c/kernel/cpu/irq/interrupts.c` | Serial transmit draining on timer ticks |
| `c/kernel/init/kmain.c` | Kernel initialization and boot markers |
| `c/kernel/gui/wm.c` | Desktop startup, console login launch, and cursor selection |
| `c/apps/coreutils/login.c` | Console authentication and startup with an absent account store |
| `c/kernel/gui/fb/fb.c` | Display scaling and software cursor presentation |
| `c/kernel/gui/fb/fb_overlay.c` | Software cursor rectangle and composition helpers |
| `c/kernel/diag/hwhud.c` | Hardware and performance panel |
| `c/kernel/diag/hwhud_panel.h` | Panel visibility and title-row interaction |
| `c/kernel/audio/boot_test_tone.c` | Diagnostic boot-tone scheduling and playback |
| `c/kernel/diag/hwreport.c` | Boot hardware report and milestones |
| `c/kernel/diag/hwreport_log.c` | Persistence of numbered boot logs |
| `c/kernel/mm/phys/pmm.c` | High-RAM direct-map construction |
| `c/kernel/mm/mmhost.h` | Physical-address translation |
| `c/net/core/net.c` | Network initialization and polling |
| `c/net/core/netcfg.c` | Static settings, DHCP selection, and hypervisor fallback |
| `c/net/core/dhcp.c` | Boot deadline, background acquisition, and lease handling |
| `c/net/tls/tls.c` | TLS random-source strength check |
| `c/boot/long.asm` | Native kernel entry and transfer to C |
| `linker.ld` | Kernel link address and entry symbol |

## See also

- [Building LogitOS](building.md)
- [Running on real hardware](real-hardware.md)
- [Boot sequence](../architecture/boot-sequence.md)
- [Testing](../architecture/testing.md)
- [SSH](../networking/ssh.md)