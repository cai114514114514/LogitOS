# tests/uefi.mk -- the ESP and the gate for LogitOS's own UEFI loader.
#
# Own fragment for the reason every other one in this tree gives (see
# tests/mem.mk, tests/desktop.mk, ...): several lines share this Makefile, and
# a whole-file overwrite from a concurrent edit has silently deleted somebody
# else's targets before. The only token the main Makefile needs is the single
# `-include tests/uefi.mk` line below (NOT added here -- the Makefile is
# contended by another workflow right now; see the orchestrator report).
#
#   make test-uefi          the gate: boot LogitOS through the self-built UEFI
#                            loader under OVMF and verify the full hand-off
#                            contract, in order, against BOTH ways the ESP can
#                            be built (mtools, and the from-scratch FAT16
#                            writer -- see WHY TWO ESP IMAGES below).
#   make test-uefi-negctl   THE CONTROL: a loader built to hand the kernel a
#                            corrupted Multiboot2 magic, and an assertion that
#                            the kernel's own entry-point check refuses it.
#
# ==========================================================================
# THE CONTRACT WITH c/boot/efi/build.sh
# ==========================================================================
# build.sh is owned by the loader itself (c/boot/efi/{efi.h,mb2.h,trampoline.S,
# loader.c,build.sh}); this fragment only calls it, through the two env vars
# ITS OWN header documents (read that file before changing the invocations
# below -- these are transcribed from there, not guessed at):
#
#   EFI_OUT       where BOOTX64.EFI is written (build.sh defaults this to
#                 build/BOOTX64.EFI; every call here overrides it so the
#                 normal build and the negctl build cannot land on the same
#                 path and race each other -- build.sh derives its object
#                 directory from EFI_OUT's basename for exactly that reason).
#   EFI_CPPFLAGS  extra -D flags for both TUs. -DEFI_BAD_MAGIC is what makes
#                 the trampoline hand the kernel eax != 0x36D76289 -- see
#                 trampoline.S's comment on MB2_MAGIC for why that's a
#                 preprocessor define and not a runtime branch (it has to be
#                 the assembled constant itself, not a value that could be
#                 read off a corrupted-but-still-well-formed hand-off).
#
# build.sh computes its own ROOT from $BASH_SOURCE, so it must be invoked as
# `c/boot/efi/build.sh` (a real path it can resolve), which every recipe
# below does. It also runs the PE-header half of the gate itself (Machine ==
# x86-64, Subsystem == EFI application) and refuses loudly if either is
# wrong, so this fragment does not repeat that check.
#
# c/boot/efi/build.sh is deliberately NOT a literal prerequisite of the rules
# below: a hard dependency on a file with no rule to build it fails `make -n`
# (the dry-run gate) for a reason unrelated to whether THIS fragment's own
# recipes are well-formed, which mattered while build.sh did not exist yet
# and is kept now on the same reasoning -- the loud `test -f` in each recipe
# is the refusal instead. $(EFI_SRC) still depends on every file actually
# present in c/boot/efi/ (build.sh included once it exists), so editing any
# of them triggers a rebuild.
#
# ==========================================================================
# WHY TWO ESP IMAGES (plus a third for the control)
# ==========================================================================
# tools/mkesp.py has two independent ways to build the FAT superfloppy: mtools
# (mformat/mcopy/mmd, a build-time host tool) and a from-scratch FAT16 writer
# used when mtools is unavailable. `mdir` reading the from-scratch image back
# proves the BYTES are a well-formed FAT16 filesystem; it does not prove a
# real UEFI firmware's own FAT driver agrees, which is a different parser
# entirely. So test-uefi boots BOTH: $(ESP_IMG) (mtools, the common case) and
# $(ESP_PYFAT_IMG) (MKESP_FORCE_PYFAT=1, forcing the from-scratch path even on
# a machine that has mtools installed) get the SAME full boot-and-assert run.
# A bug in the from-scratch writer that `mdir` cannot see -- the missing-field
# struct.pack_into that shipped here until this fragment's own author found it
# by actually booting OVMF against it, see tools/mkesp.py's _entry() comment
# -- is exactly the class of bug this second run exists to catch.
# Fallbacks so this fragment is also dry-runnable standalone (`make -n -f
# tests/uefi.mk test-uefi`); when -included from the real Makefile these are
# already bound by `:=` well before any `-include` line runs, so `?=` here
# never overrides them.
BUILD  ?= build
KERNEL ?= $(BUILD)/kernel.elf
DISK   ?= $(BUILD)/disk.img

EFI_LOAD_POLICY_BUILD := $(BUILD)/efi-load-policy
EFI_PCIDE_ORDER_BUILD := $(BUILD)/efi-pcide-order
EFI_LA57_ORDER_BUILD  := $(BUILD)/efi-la57-order

.PHONY: test-uefi-load-policy-host test-uefi-load-policy-negctl test-uefi-pcide-host \
    test-uefi-la57-host test-uefi-la57-negctl-host

# The production range validator is host-testable without pretending a host
# test is firmware. The negative target mutates its one type check so ACPI NVS
# is accepted; the fixture's first forbidden-range assertion must then go red.
test-uefi-load-policy-negctl:
	@python3 tests/unit/efi_load_policy_run.py --mode negctl --build $(EFI_LOAD_POLICY_BUILD)

test-uefi-load-policy-host: test-uefi-load-policy-negctl
	@python3 tests/unit/efi_load_policy_run.py --mode positive --build $(EFI_LOAD_POLICY_BUILD)

# This assembles the actual production trampoline four times. The positive and
# forced-PCIDE objects must place clear/write/readback before both the far jump
# and CR0.PG clear; skip and late mutations must each redden that one verdict.
test-uefi-pcide-host:
	@python3 tests/unit/efi_pcide_order_run.py --build $(EFI_PCIDE_ORDER_BUILD)

# LogitOS builds a four-level PML4. Assemble the production trampoline itself
# and require its LA57 clear/readback after PG and LME are off. Three mutated
# instruction streams prove the verdict is load-bearing.
test-uefi-la57-negctl-host:
	@python3 tests/unit/efi_la57_order_run.py --mode controls --build $(EFI_LA57_ORDER_BUILD)/controls

test-uefi-la57-host: test-uefi-la57-negctl-host
	@python3 tests/unit/efi_la57_order_run.py --mode positive --build $(EFI_LA57_ORDER_BUILD)/positive

EFI_DIR              := $(BUILD)/efi
EFI_SRC              := $(wildcard c/boot/efi/*)
BOOTX64_EFI          := $(EFI_DIR)/BOOTX64.EFI
BOOTX64_BADMAGIC_EFI := $(EFI_DIR)/BOOTX64-badmagic.EFI
BOOTX64_PCIDE_EFI    := $(EFI_DIR)/BOOTX64-pcide.EFI
BOOTX64_PCIDE_SKIP_EFI := $(EFI_DIR)/BOOTX64-pcide-skip.EFI
BOOTX64_PCIDE_LATE_EFI := $(EFI_DIR)/BOOTX64-pcide-late.EFI
BOOTX64_LA57_EFI     := $(EFI_DIR)/BOOTX64-la57.EFI
BOOTX64_LA57_SKIP_EFI := $(EFI_DIR)/BOOTX64-la57-skip.EFI

ESP_IMG          := $(BUILD)/esp.img
ESP_PYFAT_IMG    := $(BUILD)/esp-pyfat.img
ESP_BADMAGIC_IMG := $(BUILD)/esp-badmagic.img
ESP_PCIDE_IMG     := $(BUILD)/esp-pcide.img
ESP_PCIDE_SKIP_IMG := $(BUILD)/esp-pcide-skip.img
ESP_PCIDE_LATE_IMG := $(BUILD)/esp-pcide-late.img
ESP_LA57_IMG      := $(BUILD)/esp-la57.img
ESP_LA57_SKIP_IMG := $(BUILD)/esp-la57-skip.img

$(BOOTX64_EFI): $(EFI_SRC)
	@test -f c/boot/efi/build.sh || { \
	    echo "tests/uefi.mk: c/boot/efi/build.sh not found -- see the CONTRACT"; \
	    echo "  comment at the top of this file for the interface it must provide."; \
	    exit 1; }
	EFI_OUT=$(BOOTX64_EFI) EFI_CPPFLAGS= bash c/boot/efi/build.sh

# THE CONTROL's loader. Same source, one extra -D -- see the CONTRACT above.
$(BOOTX64_BADMAGIC_EFI): $(EFI_SRC)
	@test -f c/boot/efi/build.sh || { \
	    echo "tests/uefi.mk: c/boot/efi/build.sh not found -- see the CONTRACT"; \
	    echo "  comment at the top of this file for the interface it must provide."; \
	    exit 1; }
	EFI_OUT=$(BOOTX64_BADMAGIC_EFI) EFI_CPPFLAGS=-DEFI_BAD_MAGIC bash c/boot/efi/build.sh

# Executable PCIDE seam: every variant forces and confirms PCIDE=1 before
# entering the production gate. A test-only readback just before the far jump
# lets the unsafe variants report FU and halt. This proves their live CR4 state;
# it deliberately does not depend on QEMU TCG emulating the hardware #GP for a
# later CR0.PG clear. The host object gate checks the production instruction
# order independently.
$(BOOTX64_PCIDE_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$(BOOTX64_PCIDE_EFI) EFI_CPPFLAGS="-DEFI_FORCE_PCIDE -DEFI_PCIDE_TEST_ASSERT_CLEAR" bash c/boot/efi/build.sh

$(BOOTX64_PCIDE_SKIP_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$(BOOTX64_PCIDE_SKIP_EFI) EFI_CPPFLAGS="-DEFI_FORCE_PCIDE -DEFI_PCIDE_TEST_ASSERT_CLEAR -DEFI_PCIDE_NEGCTL_SKIP_CLEAR" bash c/boot/efi/build.sh

$(BOOTX64_PCIDE_LATE_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$(BOOTX64_PCIDE_LATE_EFI) EFI_CPPFLAGS="-DEFI_FORCE_PCIDE -DEFI_PCIDE_TEST_ASSERT_CLEAR -DEFI_PCIDE_NEGCTL_LATE_CLEAR" bash c/boot/efi/build.sh

# LA57 can only be changed with paging off. The positive image sets and reads
# it after the descent has disabled PG/LME, then passes through the production
# clear/readback. The control leaves it set so the fresh test readback emits U
# and halts before boot.asm can interpret a PML4 as a PML5.
$(BOOTX64_LA57_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$(BOOTX64_LA57_EFI) EFI_CPPFLAGS="-DEFI_FORCE_LA57_AFTER_PG -DEFI_LA57_TEST_ASSERT_CLEAR" bash c/boot/efi/build.sh

$(BOOTX64_LA57_SKIP_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$(BOOTX64_LA57_SKIP_EFI) EFI_CPPFLAGS="-DEFI_FORCE_LA57_AFTER_PG -DEFI_LA57_TEST_ASSERT_CLEAR -DEFI_LA57_NEGCTL_SKIP_CLEAR" bash c/boot/efi/build.sh

$(ESP_IMG): $(BOOTX64_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_EFI) --kernel $(KERNEL)

# MKESP_FORCE_PYFAT=1 -- see WHY TWO ESP IMAGES above. Rebuilt from the SAME
# BOOTX64.EFI as $(ESP_IMG); only the FAT writer differs, which is exactly the
# one variable this second run is meant to isolate.
$(ESP_PYFAT_IMG): $(BOOTX64_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	MKESP_FORCE_PYFAT=1 python3 tools/mkesp.py $@ --efi $(BOOTX64_EFI) --kernel $(KERNEL)

$(ESP_BADMAGIC_IMG): $(BOOTX64_BADMAGIC_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_BADMAGIC_EFI) --kernel $(KERNEL)

$(ESP_PCIDE_IMG): $(BOOTX64_PCIDE_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_PCIDE_EFI) --kernel $(KERNEL)

$(ESP_PCIDE_SKIP_IMG): $(BOOTX64_PCIDE_SKIP_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_PCIDE_SKIP_EFI) --kernel $(KERNEL)

$(ESP_PCIDE_LATE_IMG): $(BOOTX64_PCIDE_LATE_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_PCIDE_LATE_EFI) --kernel $(KERNEL)

$(ESP_LA57_IMG): $(BOOTX64_LA57_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_LA57_EFI) --kernel $(KERNEL)

$(ESP_LA57_SKIP_IMG): $(BOOTX64_LA57_SKIP_EFI) $(KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_LA57_SKIP_EFI) --kernel $(KERNEL)

# ---------------------------------------------------------------------------
# test-uefi -- THE GATE.
# ---------------------------------------------------------------------------
# $(DISK) is the same LogitFS data disk every other boot harness in this tree
# mounts (tests/boot/run-test.sh's own $(DISK) argument) -- this milestone
# changes nothing about the filesystem or what is on it, only how the kernel
# gets STARTED, so reusing it is the point, not a shortcut.
test-uefi: test-uefi-load-policy-host test-uefi-pcide-host test-uefi-la57-host $(ESP_IMG) $(ESP_PYFAT_IMG) $(DISK)
	@bash tests/boot/run-uefi-test.sh $(ESP_IMG) $(DISK)
	@echo "--- repeating against the from-scratch (no-mtools) FAT16 ESP, to prove"
	@echo "    that path is not merely mdir-clean but actually boots OVMF ---"
	@bash tests/boot/run-uefi-test.sh $(ESP_PYFAT_IMG) $(DISK)

# ---------------------------------------------------------------------------
# test-uefi-negctl -- THE CONTROL.
# ---------------------------------------------------------------------------
# "The kernel checks the Multiboot2 magic" is not a fact about this boot path
# until something has been WATCHED failing it. This is that something: the
# loader is built to hand over a wrong eax on purpose (see the CONTRACT
# above), and the assertion is that boot.asm's check_multiboot .fail path
# (c/boot/boot.asm:38-44) swallows the jump -- no LOGIT_BOOT_OK, ever -- while
# every breadcrumb BEFORE the jump still fires, so a failure here can only
# mean the kernel's own check did not run, not that the loader broke.
test-uefi-negctl: $(ESP_BADMAGIC_IMG) $(DISK)
	@bash tests/boot/run-uefi-test.sh $(ESP_BADMAGIC_IMG) $(DISK) negctl

# The positive run must emit FICPLJ and boot the complete kernel. Both controls
# force PCIDE but remove or delay its clear; each must emit FU and halt before
# the far jump. U comes from a fresh CR4 readback in 64-bit paged mode.
.PHONY: test-uefi-pcide test-uefi-pcide-negctl
test-uefi-pcide-negctl: $(ESP_PCIDE_SKIP_IMG) $(ESP_PCIDE_LATE_IMG) $(DISK)
	@bash tests/boot/run-uefi-test.sh $(ESP_PCIDE_SKIP_IMG) $(DISK) pcide-negctl
	@bash tests/boot/run-uefi-test.sh $(ESP_PCIDE_LATE_IMG) $(DISK) pcide-negctl

test-uefi-pcide: test-uefi-pcide-host $(ESP_PCIDE_IMG) $(DISK) test-uefi-pcide-negctl
	@bash tests/boot/run-uefi-test.sh $(ESP_PCIDE_IMG) $(DISK) pcide-positive

# The QEMU CPU advertises five-level paging so the forced CR4.LA57 transition
# is architecturally available. The guest control observes LA57=1 at a fresh
# readback and halts; the positive image observes the same injected state,
# clears it in production code, and boots through the normal kernel path.
.PHONY: test-uefi-la57 test-uefi-la57-negctl
test-uefi-la57-negctl: test-uefi-la57-host $(ESP_LA57_SKIP_IMG) $(DISK)
	@QEMU_CPU=max,la57=on bash tests/boot/run-uefi-test.sh $(ESP_LA57_SKIP_IMG) $(DISK) la57-negctl

test-uefi-la57: test-uefi-la57-host $(ESP_LA57_IMG) $(DISK) test-uefi-la57-negctl
	@QEMU_CPU=max,la57=on bash tests/boot/run-uefi-test.sh $(ESP_LA57_IMG) $(DISK) la57-positive

# ---------------------------------------------------------------------------
# esp / run-uefi -- THE TWO TARGETS A PERSON TYPES.
# ---------------------------------------------------------------------------
# A boot path that only a test can reach is a boot path nobody has. Until these
# existed, BOOTX64.EFI and the ESP appeared nowhere outside this fragment: the
# main Makefile had never heard of them, `make run` was BIOS-and-GRUB only, and
# the only way to see the machine come up under UEFI was to run the gate. The
# firmware support was committed, green, and unusable, which is the same thing
# as absent for anyone who is not reading test output.
#
# `esp` is the artifact by name -- the FAT superfloppy carrying
# /EFI/BOOT/BOOTX64.EFI and the kernel. It is NOT hung off `all`: it costs a
# separate link plus a filesystem build, and the BIOS path does not need it, so
# every ordinary build should not pay for it. Ask for it and you get it.
#
# `run-uefi` is `run` with the firmware swapped and nothing else changed --
# same disk, same RAM, same virtio-gpu, same serial-on-stdio -- so a difference
# between the two runs is a difference in the FIRMWARE and not in how they were
# invoked. That is the whole point of having both.
esp: $(ESP_IMG)
	@echo "ESP: $(ESP_IMG)  (boot it with: make run-uefi)"

# OVMF's VARS region is writable NVRAM, so the guest writes boot variables back
# into it. Handing QEMU the distro's own copy would mutate a file every other
# UEFI test also reads; this keeps a per-tree copy and makes it once.
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC ?= /usr/share/OVMF/OVMF_VARS_4M.fd
UEFI_VARS := $(BUILD)/OVMF_VARS.fd

$(UEFI_VARS): $(OVMF_VARS_SRC)
	@mkdir -p $(BUILD)
	@cp $(OVMF_VARS_SRC) $@

run-uefi: $(ESP_IMG) $(DISK) $(UEFI_VARS)
	@[ -f "$(OVMF_CODE)" ] || { \
	    echo "run-uefi: no OVMF firmware at $(OVMF_CODE)"; \
	    echo "  install it (Debian/Ubuntu: apt install ovmf) or set OVMF_CODE=/path/to/OVMF_CODE.fd"; \
	    exit 1; }
	@# The ESP rides AHCI and the LogitFS disk rides virtio-blk, which is the
	@# arrangement run-uefi-test.sh proved -- do not "simplify" it to one bus
	@# without booting it first. $(QEMU_DISK) is deliberately NOT reused: it
	@# carries `-boot d`, which tells the firmware to boot the CD-ROM that this
	@# path does not have. Everything else is the same knob `run` uses.
	$(QEMU) -machine q35 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(UEFI_VARS) \
	    -device ich9-ahci,id=ahci0 \
	    -drive file=$(ESP_IMG),format=raw,if=none,id=esp0,file.locking=off \
	    -device ide-hd,drive=esp0,bus=ahci0.0 \
	    -drive file=$(DISK),format=raw,if=none,id=hd0,file.locking=off \
	    -device virtio-blk-pci,drive=hd0 \
	    $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) $(QEMU_DISP) \
	    -serial stdio -no-reboot -qmp unix:/tmp/logit-qmp.sock,server,nowait

.PHONY: test-uefi test-uefi-negctl test-uefi-pcide test-uefi-pcide-negctl \
    test-uefi-la57 test-uefi-la57-negctl esp run-uefi
