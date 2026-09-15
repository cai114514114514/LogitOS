# Plan: take the boot path off GRUB

Written 2026-09-15. Every number and file:line below was measured against the
tree on that date, not recalled. Re-measure before trusting any of it.

---

## 1. What is actually borrowed

Not "the boot". One path, and one image writer.

| piece | whose | where |
|---|---|---|
| BIOS/ISO bootloader | **GRUB** | `Makefile`: `$(ISO): $(KERNEL) grub.cfg` → `i686-elf-grub-mkrescue -o $@ $(ISO_DIR)` |
| ISO9660 / El Torito writer | **xorriso** | invoked by `grub-mkrescue` |
| boot menu | GRUB | `grub.cfg`, six lines, one `multiboot2 /boot/kernel.elf` |
| UEFI loader | **ours already** | `c/boot/efi/loader.c` 1,097 lines + `trampoline.S` 409 + `mb2.h` 115 |
| ESP writer | **ours already** | `tools/mkesp.py` 369 lines, including a from-scratch FAT16 writer |
| firmware (SeaBIOS, OVMF) | third party, **and out of scope** | see §6 |

So the job is: **a from-scratch BIOS-stage bootloader, and a from-scratch
bootable-image writer.** The UEFI half was done already, which matters for two
reasons: it proves this tree can do it, and it supplies a working model of
every hard part (`loader.c` already builds a Multiboot2 block by hand, and
`mkesp.py` already writes a filesystem by hand and is *booted* as its own gate,
not merely parsed).

The argument for doing it at all: today nothing in this system can read, reason
about, or change what happens between power-on and `start:`. That span is a
binary blob from another project with a six-line config file. Everything above
it — the loader, the filesystem, the compositor, the browser — this tree can
open and answer questions about. The boot cannot. That asymmetry is the reason,
and it is worth more than the independence.

---

## 2. The contract the new loader must satisfy

**Measured, not assumed.** Three consumers, three tags. That is the entire
surface, and it is why this project is tractable:

| tag | what | consumer |
|---|---|---|
| 6 | memory map | `c/kernel/mm/phys/pmm.c:408` walks it; `struct mb2_mmap_entry` at `:75` |
| 8 | framebuffer | `c/kernel/gui/fb/fb.c:210` — **optional**, see the trap in §4d |
| 14 / 15 | ACPI RSDP (1.0 / 2.0+) | `c/kernel/cpu/acpi/acpi.c:123` `rsdp_from_mb2()`, set from `kmain.c:84` |

Entry state, from `c/boot/boot.asm:17-24`:

- 32-bit protected mode, paging off, interrupts off
- `eax` = `0x36d76289` (checked at `boot.asm:36`; a mismatch prints `ERR: 0`)
- `ebx` = physical address of the Multiboot2 info block
- the kernel does the rest itself: page tables, PAE, EFER.LME, long mode

Load address, from `linker.ld:14`: **32 MiB**, one contiguous image from
`_kernel_start` to `_kernel_end`. The linker script's own comment records why it
is not 1 MiB (firmware-owned low memory on real UEFI machines), and states that
"both GRUB and the self-built UEFI loader consume this same ELF physical
address" — the new BIOS loader becomes the third consumer of that one sentence.

---

## 3. Stage 0 — write the contract down, and gate it

Before a line of assembly. Three loaders will now claim to satisfy one
contract, which is exactly the shape CLAUDE.md's third rule is about: a
constant spelled in three places agrees on the wrong value about as often as
the right one.

**Build** `tests/unit/boot_contract_test.c` (host) that asserts, from the
sources rather than from this document:

- the Multiboot2 header in `c/boot/multiboot2.asm` has the magic, the
  architecture, a correct checksum, and the framebuffer tag marked **optional**
- `boot.asm` checks `eax` against `0x36d76289`
- the tag types the kernel reads are exactly {6, 8, 14, 15} — derived by
  scanning the three consumer files for their comparisons, so that a fourth
  consumer appearing tomorrow reddens this gate rather than silently making the
  new loader incomplete
- `linker.ld` places `_kernel_start` at 32 MiB

**Negative control** (`test-boot-contract-negctl`): compile the same test
against a fixture header with the checksum off by one, and against a fixture
kernel source that reads a tag type not in the set. Both must FAIL, and you
must quote the failing output. A control you did not watch fail is not a
control.

Wire it `test-boot-contract: test-boot-contract-negctl` in a new
`tests/bootself.mk`, `-include`d from the root Makefile.

---

## 4. Stage 1 — our own bootable-image writer

`tools/mkiso.py`. Replaces xorriso for our one use.

Scope deliberately small: ISO9660 with a primary volume descriptor, one
directory level, and an **El Torito** boot catalog in no-emulation mode. No
Joliet, no Rock Ridge, no multi-session — we are not writing a general mastering
tool, we are writing the smallest correct thing that a BIOS will boot, and
saying so in the file's own header keeps the next person from growing it.

**The target layout, read off the ISO GRUB builds today** (2026-09-15,
`build-biliplay-rickroll/logit.iso`, 14.9 MB) so that `mkiso.py` has something
measured to match rather than a spec to interpret:

```
sector 16   Primary Volume Descriptor       (type 1, "CD001")
sector 17   Boot Record VD                  (type 0), system id
                                            "EL TORITO SPECIFICATION",
                                            catalog pointer -> LBA 49
sector 18   Terminator                      (type 255)
LBA 49      El Torito boot catalog
  validation entry  header_id=1  platform=0 (x86)  key=55 AA
                    16-bit sum over the 32 bytes = 0
  initial entry     bootable=0x88  media=0 (no emulation)
                    load segment 0x07C0 -> physical 0x7C00
                    sector count 4 (x512 = 2048 bytes)
                    image LBA 217
```

**One entry, no section header.** This ISO is BIOS-only: there is no UEFI
platform section, because the UEFI path boots from the separate ESP image that
`mkesp.py` writes, not from this ISO. So `mkiso.py` does not need a second
platform section, and adding one would be building a thing nothing boots.

Model it on `tools/mkesp.py`, which already does the equivalent job for FAT16
and is held to the right bar: its gate does not merely check that `mdir` likes
the image, it **boots the from-scratch one under OVMF** and says why
(`tests/uefi.mk`: "to prove that path is not merely mdir-clean but actually
boots OVMF"). Hold `mkiso.py` to the same bar.

**Gate** `test-mkiso`: a host-side parse asserting the PVD, the boot record
volume descriptor pointing at the catalog, the catalog's validation entry
checksum, the initial/default entry's no-emulation media type and load segment;
then a QEMU boot of the produced ISO reaching `LOGIT_BOOT_OK`.

**Negative controls**, each watched failing:
`--negctl-bad-catalog-checksum` (the validation entry's 16-bit sum must be 0 —
break it and the BIOS must refuse), `--negctl-wrong-platform-id`, and
`--negctl-emulation-floppy` (claim floppy emulation while supplying a
no-emulation image).

---

## 5. Stage 2 — preload, the 512-byte boot sector

`c/boot/bios/preload.asm`. El Torito no-emulation loads it at `0x7C00` with
`dl` = the BIOS drive number.

Its only job is to load loader and jump.

> **Correction, measured 2026-09-15 against `build-biliplay-rickroll/logit.iso`,
> and the original sentence is kept beside it because somebody will arrive
> holding it.** This section first read: *"It must fit in 512 bytes including
> the `0xAA55` signature, which is the whole reason loader exists as a separate
> thing."* Both halves are wrong, and they are the two things a person carries
> over from MBR boot without noticing that El Torito is not MBR:
>
> - **The size is not 512.** The BIOS loads `sector_count` × 512 bytes from the
>   catalog entry, and that count is ours to choose. GRUB's own entry asks for
>   **4** — 2,048 bytes, exactly one ISO sector, which is the sensible unit — and
>   then leaves sectors 1, 2 and 3 entirely zero (186 non-zero bytes in the whole
>   image, all in sector 0). So the preload/loader split is a choice about
>   clarity, not a constraint the format imposes. Make it on purpose or not at
>   all.
> - **There is no `0xAA55`.** Offset 510 of GRUB's boot image is `00 00`. The
>   `55 AA` that does exist lives in the catalog's *validation entry* (its last
>   two bytes, a key the BIOS checks), not in the boot image. A gate asserting
>   the image ends `55 AA` would fail on a correct image, which is worse than no
>   gate.

Read loader with **INT 13h AH=42h** (extended read, LBA) — not CHS. Check for
the extensions first with AH=41h, and if they are absent, print one line and
halt rather than issuing a CHS read that will silently read the wrong sector on
a CD. Every BIOS that can boot an El Torito CD has the extensions; the check is
there so the failure names itself.

**Gate** `test-preload`: host assertion that the built sector is exactly 512
bytes and ends `55 AA`; then a QEMU boot reaching a preload serial marker.
**Negative control**: `-DPRELOAD_NO_EXT_CHECK` removes the AH=41h check — the
gate that boots it on a machine with extensions still passes, so this control
is **only meaningful with a second fixture that reports no extensions**. If you
cannot build that fixture under QEMU, say so and mark the control
`SKIP: cannot construct a no-extensions BIOS here` rather than shipping a
control that cannot fail.

---

## 6. Stage 3 — loader, the real work

`c/boot/bios/loader.asm` plus `c/boot/bios/loader.c` (16-bit-capable C is
painful; prefer assembly for the real-mode parts and C only for the MB2 block
assembly, compiled `-m32 -ffreestanding`). Do these in this order, because the
order is the design:

**a. A20.** INT 15h AX=2401, fall back to the keyboard controller, fall back to
Fast A20 (port 0x92). Then **verify** by writing a value at `0x100000` and
reading its 1 MiB alias at `0x000000`. Never assume the enable worked: a
silently-wrapped address space corrupts the kernel image you are about to copy,
and the symptom appears thousands of instructions later.

**b. Memory map.** INT 15h AX=E820 into a buffer below 1 MiB. Keep every entry
the BIOS returns, including the reserved ones — `pmm.c` reads types, and an
edited map is a lie the PMM cannot detect.

**c. ACPI RSDP.** Scan the EBDA (the word at `0x40E`, shifted left 4) then
`0xE0000`–`0xFFFFF` on 16-byte boundaries for `"RSD PTR "`. Checksum the first
20 bytes; if `revision >= 2`, also checksum `length` bytes and emit **tag 15**,
otherwise **tag 14**. `acpi.c:136` says it trusts nothing unchecked, so a bad
checksum must produce no tag rather than a tag the kernel then rejects.

**d. Framebuffer — and this is the trap.** The MB2 header
(`multiboot2.asm:20-28`) marks the framebuffer request **optional**, and its
comment records why: with `-vga none` the display is virtio-gpu and the kernel
drives it itself. So **the default, shipping path must emit no tag 8 and must
boot**. Do not make a VBE mode a precondition. Add tag 8 only when a VBE linear
mode is genuinely present, and gate that separately with `-vga std`.

**e. Load the kernel — the second trap, and where the day goes.** INT 13h works
only in real mode. The kernel goes to 32 MiB, which real mode cannot address.
So: read the ELF's `PT_LOAD` segments in real mode into a bounce buffer below
1 MiB, and copy each chunk up with **unreal mode** (or a protected-mode memcpy
stub, returning to real mode for the next read). Zero the `.bss` tail — the ELF
`p_memsz` exceeds `p_filesz` and `_kernel_end` is where the PMM starts handing
out frames, so a non-zero `.bss` is a kernel that boots and then behaves
strangely much later.

**f. Build the MB2 info block** below 1 MiB, 8-byte aligned, `total_size` and
`reserved` first, every tag 8-byte aligned, the type-0 end tag last.
`acpi.c:117` walks it defensively and bails on a zero size; write it so that
defence never has to fire.

**g. Enter protected mode**, set `eax` and `ebx`, far jump to the ELF entry
point (read it from the header; do not hardcode 32 MiB + 0).

**Negative controls**, each built as its own image and watched failing:
`-DLOADER_SKIP_A20_VERIFY` (boot must corrupt or hang, and if it does NOT on
this machine, say so — that is a finding about QEMU, not a pass),
`-DLOADER_BAD_MB2_MAGIC` (the kernel must print `ERR: 0` from `boot.asm:41`),
`-DLOADER_TRUNCATE_MMAP` (the PMM must report less memory — assert the number,
not merely that it booted).

---

## 7. Stage 4 — the differential, which is the whole point

Do this **before** removing GRUB, while both loaders still exist. This is the
only stage that distinguishes "we built a bootloader" from "we built a thing
that happens to boot".

Add a kernel-side dump behind `-DBOOT_MB2_DUMP`: walk the received info block
and print every tag — type, size, and for tag 6 every memory-map entry's base,
length and type; for 14/15 the RSDP's OEM id and revision. Then boot the same
`kernel.elf` twice on the same QEMU invocation shape, once from the GRUB ISO and
once from ours, and **diff the two dumps**.

They will not be byte-identical (GRUB adds tags we ignore, and the block's
placement differs). The assertion is narrower and is the one that matters:
**for every tag the kernel actually consumes, the content is equivalent** — the
same memory-map entries in the same order, the same RSDP pointer, the same
framebuffer presence or absence.

`make test-boot-differential` runs it. Its negative control is free and strong:
feed it the `-DLOADER_TRUNCATE_MMAP` image and the diff must go red.

---

## 8. Stage 5 — cut GRUB out

Only after §7 is green.

- `Makefile`: `$(ISO)` built by `tools/mkiso.py`; drop `GRUB_RESCUE`, the
  `$(ISO_DIR)/boot/grub` copy, and `grub.cfg`.
- Keep `make iso-grub` for one release as a bisection escape hatch, and put a
  dated line in its recipe saying when it goes.
- **Then the ~100 boot harnesses must all still pass.** That is the acceptance
  test and it is not optional: every `tests/boot/run-*.sh` consumes `$(ISO)`.
  `make test-sweep` is the instrument; run the failures again alone before
  believing them, because several makes sharing one `build/` manufacture each
  other's failures.

---

## 9. What this does NOT do

Named so that nobody reads the finished work as more than it is.

- **Firmware stays borrowed.** SeaBIOS under QEMU, OVMF for UEFI, and whatever
  ships on a real board. A bootloader is not firmware; replacing firmware is
  coreboot's problem and a different project. Anyone who says "the boot is
  self-hosted now" should be able to finish the sentence with "from the boot
  sector up".
- **No Secure Boot**, no signed images.
- **ISO only.** No MBR hard-disk boot, no USB, no PXE in this pass.
- **The UEFI path is untouched.** It is already ours and it already has gates
  with watched-failing controls (`test-uefi`, `-DEFI_BAD_MAGIC`, the pcide and
  la57 corners). Do not "unify" the two loaders in this project. One jar, two
  doors is a rule about constants, not an instruction to merge two programs
  that run in different CPU modes against different firmware.

---

## 10. Ordering constraint, stated once

§7 before §8. Removing GRUB before the differential is green means the next
regression has nothing to be compared against, and the tree has already paid
for that shape once: `tools/verify-commit.sh` exists because three commits
landed in one day whose own clean clone did not compile.

---

# Phase 2 — retire Multiboot2 itself

Phase 1 above replaces *who loads the kernel*. This replaces *the protocol they
hand it over with*. It is a separate phase for one hard reason stated in §14.

## 11. The argument, and it is not independence

Independence is the weak version of this argument. Here is the strong one,
measured 2026-09-15.

`c/boot/efi/trampoline.S` opens by saying what it is for, in its own words:

> "UEFI hands an application the CPU in 64-bit long mode with paging on. The
> kernel's entry (`c/boot/boot.asm:19 start`) is 32-bit protected-mode code...
> so the loader has to give back what the firmware gave it and arrive at
> `start` in the machine state the Multiboot2 spec (sec 3.2/3.3) describes."

Laid out in order, the UEFI boot does this:

1. firmware hands us **64-bit long mode, paging on, identity-mapped**
2. `trampoline.S`, **409 lines**, tears it down: compatibility mode, clear
   `CR0.PG`, disable LME — and handles the PCIDE and LA57 traps on the way
3. we arrive in **32-bit protected mode, paging off**
4. `boot.asm`, **158 lines**, builds identity page tables, sets PAE/LME/PG, and
   far-jumps **back into 64-bit long mode**

We descend and immediately climb, and the only reason is that a specification
written for 1995's BIOS loaders says the kernel is entered in 32-bit protected
mode. The cost is not only those lines. **Two of the three negative-control
corners in `tests/uefi.mk` exist solely to police that descent**
(`-DEFI_FORCE_PCIDE`, `-DEFI_FORCE_LA57_AFTER_PG`, 34 references) — gates
guarding a teardown a native protocol never performs.

The design statement, one sentence: *today both loaders contort the CPU into a
state neither firmware gives them and the kernel climbs back out of it; natively,
each loader delivers the state the kernel actually wants, and the adaptation
lives in the component that knows what its own firmware did.*

## 12. The dependency surface, counted

HISTORY (retired 2026-09-15): this section originally counted six production
files and about 45 Multiboot2 references. The scanned header, 32-bit entry and
UEFI descent are now absent from production. Their only surviving executable
copies live under `tests/fixtures/bootoracle/`, linked into a separately named
GRUB kernel for the fixed-QEMU differential and one-release escape hatch.

The three mature consumers still receive an internal legacy-shaped view from
`bootinfo.c`. That was chosen over three simultaneous parser conversions because
`acpi.c` was owned by another active task and the unchanged payload layouts let
one validated entry adapter preserve one reader per fact. This is not a second
wire protocol: both shipping loaders emit native tag numbers exclusively.

## 13. The shape of the native protocol

Not designed here — designed in its own spec with its own review. What follows
is the set of decisions that are already forced by what was measured, so that
the design starts from them instead of rediscovering them.

- **Enter in 64-bit long mode.** This is the whole point (§11). Interrupts off,
  a known GDT, and a minimal identity map the loader documents.
- **The loader's identity map is a runway, not a memory model.** The kernel
  keeps building its own tables: `mmhost.h:43` rests "phys == virt" on the
  kernel's own first-1-GiB map, and handing that responsibility to two separate
  loaders would be one jar with three doors. The loader maps enough to execute
  the kernel's early code and says exactly how much.
- **Keep TLV.** The tag list is the good part of Multiboot2: extensible,
  skippable, self-describing, and `acpi.c:117` already walks it defensively.
  Keep the shape, drop the provenance.
- **Version, and fail loudly.** A `version` field the kernel refuses when it
  does not recognise it, printing what it got and what it wanted. Multiboot2's
  magic answers "was I loaded by a loader"; it never answers "by a loader that
  agrees with me about the block".
- **No scanned header, and this is the one somebody will get wrong out of
  symmetry.** `multiboot2.asm` exists so GRUB can *find and validate* a kernel
  by scanning the first 32 KiB of the file. Once both loaders are ours and both
  parse ELF program headers, nothing scans for anything. The 37 lines delete and
  **nothing replaces them**. Writing a `logitboot.asm` would be building a
  landmark for a search that no longer happens.
- **Stay ELF.** Leaving Multiboot2 costs us the ability for any third-party
  loader to boot this kernel, and that is a real loss to state plainly rather
  than wave off. Remaining a plain ELF with documented `PT_LOAD` addresses keeps
  a compatibility shim possible for anyone who ever wants one.

## 14. The ordering constraint, which is the reason this is Phase 2

HISTORY (completed 2026-09-15): GRUB first left the product path, both in-tree
loaders then gained native v1 behind test flags, and the differential showed the
same consumed machine facts. Production now has only the native long-mode entry.

GRUB remains deliberately test-only for one release. Its separately linked
kernel retains the retired entry so the oracle is still an independent loader,
not our loader compared with itself. The boundary is narrower than the old live
differential: the fixed SeaBIOS invocation can catch changes in the facts our
BIOS loader reports, but cannot catch QEMU/SeaBIOS drift or behavior on other
firmware and real machines.

## 15. What Phase 2 is still not

Firmware. Again, and it will keep being true: SeaBIOS, OVMF, and whatever a real
board ships stay third-party. When both phases are done the honest sentence is
**"self-hosted from the boot sector up, on someone else's firmware"** — which is
what every operating system that is not also a firmware project can say.
