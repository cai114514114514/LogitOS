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
BOOTX64_NATIVE_EFI := $(EFI_DIR)/BOOTX64-native.EFI
BOOTX64_NATIVE_BADVER_EFI := $(EFI_DIR)/BOOTX64-native-bad-version.EFI
BOOTX64_NATIVE_HIGHINFO_EFI := $(EFI_DIR)/BOOTX64-native-high-info.EFI
BOOTX64_NATIVE_EARLYEBS_EFI := $(EFI_DIR)/BOOTX64-native-early-ebs.EFI

ESP_IMG          := $(BUILD)/esp.img
ESP_PYFAT_IMG    := $(BUILD)/esp-pyfat.img
ESP_BADMAGIC_IMG := $(BUILD)/esp-badmagic.img
ESP_PCIDE_IMG     := $(BUILD)/esp-pcide.img
ESP_PCIDE_SKIP_IMG := $(BUILD)/esp-pcide-skip.img
ESP_PCIDE_LATE_IMG := $(BUILD)/esp-pcide-late.img
ESP_LA57_IMG      := $(BUILD)/esp-la57.img
ESP_LA57_SKIP_IMG := $(BUILD)/esp-la57-skip.img
ESP_NATIVE_IMG := $(BUILD)/esp-native.img
ESP_NATIVE_BADVER_IMG := $(BUILD)/esp-native-bad-version.img
ESP_NATIVE_HIGHINFO_IMG := $(BUILD)/esp-native-high-info.img
ESP_NATIVE_EARLYEBS_IMG := $(BUILD)/esp-native-early-ebs.img

# Native-loader builds select logit_native_start as ELF e_entry. Reuse the
# already-authoritative BIOS-native kernel rule instead of spelling the link
# line a second time; the order-only directory fixes the otherwise-unobservable
# direct-target failure where lld was asked to create a file in no directory.
UEFI_NATIVE_KERNEL := $(BUILD)/bios-native/kernel-dump.elf
$(UEFI_NATIVE_KERNEL): | $(BUILD)/bios-native/

$(BUILD)/bios-native/:
	@mkdir -p $@

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

# EFI_NATIVE selects the native v1 block and direct long-mode handoff. With no
# flag, BOOTX64_EFI remains the shipping MB2/descent build. The three following
# images are executable controls, not alternate production policies.
$(BOOTX64_NATIVE_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$@ EFI_CPPFLAGS="-DEFI_NATIVE" bash c/boot/efi/build.sh

$(BOOTX64_NATIVE_BADVER_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$@ EFI_CPPFLAGS="-DEFI_NATIVE -DEFI_NATIVE_BAD_VERSION" bash c/boot/efi/build.sh

$(BOOTX64_NATIVE_HIGHINFO_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$@ EFI_CPPFLAGS="-DEFI_NATIVE -DEFI_NATIVE_INFO_ABOVE_MAP" bash c/boot/efi/build.sh

$(BOOTX64_NATIVE_EARLYEBS_EFI): $(EFI_SRC)
	@mkdir -p $(EFI_DIR)
	EFI_OUT=$@ EFI_CPPFLAGS="-DEFI_NATIVE -DEFI_NATIVE_EBS_EARLY" bash c/boot/efi/build.sh

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

$(ESP_NATIVE_IMG): $(BOOTX64_NATIVE_EFI) $(UEFI_NATIVE_KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_NATIVE_EFI) --kernel $(UEFI_NATIVE_KERNEL)

$(ESP_NATIVE_BADVER_IMG): $(BOOTX64_NATIVE_BADVER_EFI) $(UEFI_NATIVE_KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_NATIVE_BADVER_EFI) --kernel $(UEFI_NATIVE_KERNEL)

$(ESP_NATIVE_HIGHINFO_IMG): $(BOOTX64_NATIVE_HIGHINFO_EFI) $(UEFI_NATIVE_KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_NATIVE_HIGHINFO_EFI) --kernel $(UEFI_NATIVE_KERNEL)

$(ESP_NATIVE_EARLYEBS_IMG): $(BOOTX64_NATIVE_EARLYEBS_EFI) $(UEFI_NATIVE_KERNEL) tools/mkesp.py
	@mkdir -p $(BUILD)
	python3 tools/mkesp.py $@ --efi $(BOOTX64_NATIVE_EARLYEBS_EFI) --kernel $(UEFI_NATIVE_KERNEL)

# OVMF is packaged under /usr/share on Linux and as split 3.5-MiB/528-KiB
# pflash files by Homebrew. The old runner hard-codes only the Linux pair; this
# gate names both layouts so "firmware absent" is a loud SKIP, not a macOS
# failure unrelated to the loader. Callers may still override either path.
UEFI_NATIVE_OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd /usr/local/share/qemu/edk2-x86_64-code.fd))
UEFI_NATIVE_OVMF_VARS ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS_4M.fd /opt/homebrew/share/qemu/edk2-i386-vars.fd /usr/local/share/qemu/edk2-i386-vars.fd))
UEFI_NATIVE_LOG_DIR := $(BUILD)/efi-native-logs

# Run one bounded OVMF specimen and retain its complete serial stream. The
# console prints only boot-contract lines so a normal gate does not bury its
# verdict under desktop diagnostics. Arguments after the Python program are:
# qemu, code, vars-template, ESP, optional data disk or '-', RAM MiB, log,
# required strings separated by '|', and a forbidden string or '-'.
UEFI_NATIVE_RUN = python3 -c 'import pathlib,shlex,shutil,signal,subprocess,sys; q,code,varsrc,esp,disk,ram,log,required,forbidden=sys.argv[1:]; pathlib.Path(log).parent.mkdir(parents=True,exist_ok=True); varcopy=log+".vars"; shutil.copyfile(varsrc,varcopy); cmd=shlex.split(q)+["-machine","q35","-drive","if=pflash,format=raw,readonly=on,file="+code,"-drive","if=pflash,format=raw,file="+varcopy,"-device","ich9-ahci,id=ahci0","-drive","file="+esp+",format=raw,if=none,id=esp0,file.locking=off","-device","ide-hd,drive=esp0,bus=ahci0.0","-m",ram,"-smp","2","-display","none","-serial","stdio","-monitor","none","-no-reboot","-snapshot"]; cmd += (["-vga","none"] if disk == "-" else ["-drive","file="+disk+",format=raw,if=none,id=hd0,file.locking=off","-device","virtio-blk-pci,drive=hd0"]); p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT); signal.signal(signal.SIGALRM,lambda *_: p.terminate()); signal.alarm(14); out=p.communicate()[0]; signal.alarm(0); pathlib.Path(log).write_bytes(out); text=out.decode(errors="replace"); keep=("[efi]","LOGIT BOOT","LOGIT_BOOT","MB2 RESULT","MB2 ACPI","MB2 FB","LOGIT_BOOT_OK"); print("\n".join(line for line in text.splitlines() if any(k in line for k in keep))); missing=[s for s in required.split("|") if s and s not in text]; assert not missing,"missing required serial text: "+repr(missing); assert forbidden == "-" or forbidden not in text,"forbidden serial text present: "+forbidden'

.PHONY: test-uefi-native-negctl test-uefi-native-differential test-uefi-native

# These are controls of the native boundary itself. They are a prerequisite of
# the positive gate, so nobody can run the green path while silently skipping
# the evidence that version, reachability and EBS ordering are load-bearing.
test-uefi-native-negctl: $(ESP_NATIVE_BADVER_IMG) $(ESP_NATIVE_HIGHINFO_IMG) $(ESP_NATIVE_EARLYEBS_IMG)
	@if [ -z "$(UEFI_NATIVE_OVMF_CODE)" ] || [ -z "$(UEFI_NATIVE_OVMF_VARS)" ] || ! command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    echo 'SKIP: test-uefi-native-negctl needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	    exit 0; \
	fi; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_BADVER_IMG)" - 1024 "$(UEFI_NATIVE_LOG_DIR)/bad-version.log" "[efi] ebs ok|LOGIT BOOT VERSION REFUSED got=0002 wanted=0001" LOGIT_BOOT_OK; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_HIGHINFO_IMG)" - 2048 "$(UEFI_NATIVE_LOG_DIR)/high-info.log" "[efi] info 0x40000000|[efi] ebs ok|LOGIT BOOT HEADER OUTSIDE IDENTITY MAP" LOGIT_BOOT_OK; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_EARLYEBS_IMG)" - 1024 "$(UEFI_NATIVE_LOG_DIR)/early-ebs.log" "CONTROL ExitBootServices before native block complete|[efi] ebs ok|LOGIT BOOT HEADER EXTENT REFUSED" LOGIT_BOOT_OK; \
	echo 'PASS: uefi-native controls refused bad version, unreachable block, and incomplete-before-EBS block'

# The BIOS three-way is byte-for-byte because all three see SeaBIOS E820. OVMF
# is a different firmware and its 100+ descriptors are not that map. The fourth
# specimen therefore compares only honest consumer facts: RSDP present,
# framebuffer absent under the same -vga none condition, ordered/non-overlapping
# regions, loader available-byte accounting, and complete RAM coverage of the
# loaded kernel. It explicitly does NOT compare descriptor boundaries, ACPI
# revision/addresses, or demand identical region counts.
test-uefi-native-differential: $(ESP_NATIVE_IMG) \
    $(BUILD)/bios-native/native-dump.iso $(BUILD)/bios-boot/ours-dump.iso \
    $(BUILD)/bios-mb2/grub-dump.iso
	@python3 tests/unit/bios_mb2_test.py --qemu $(QEMU) three-way \
	    $(BUILD)/bios-native/native-dump.iso $(BUILD)/bios-boot/ours-dump.iso \
	    $(BUILD)/bios-mb2/grub-dump.iso
	@if [ -z "$(UEFI_NATIVE_OVMF_CODE)" ] || [ -z "$(UEFI_NATIVE_OVMF_VARS)" ] || ! command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    echo 'SKIP: fourth UEFI differential needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	    exit 0; \
	fi; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_IMG)" - 1024 "$(UEFI_NATIVE_LOG_DIR)/differential.log" "LOGIT_BOOT_NATIVE_OK version=0001|[efi] gop none|MB2 ACPI|MB2 RESULT PASS" "MB2 FB"; \
	python3 -c 'import pathlib,re; t=pathlib.Path("$(UEFI_NATIVE_LOG_DIR)/differential.log").read_text(errors="replace"); rows=[tuple(int(x,16) for x in m) for m in re.findall(r"MB2 MMAP ([0-9A-F]+) ([0-9A-F]+) ([0-9A-F]+)",t)]; assert rows,"UEFI dump has no memory regions"; assert all(n>0 for _,n,_ in rows),"zero-length UEFI region"; assert all(rows[i][0]+rows[i][1] <= rows[i+1][0] for i in range(len(rows)-1)),"UEFI regions overlap or are out of order"; usable=[(a,a+n) for a,n,k in rows if k==1]; lo,hi=(int(x,16) for x in re.search(r"\[efi\] load reserved 0x([0-9a-f]+)\.\.0x([0-9a-f]+)",t).groups()); cover=[(max(a,lo),min(b,hi)) for a,b in usable if a<hi and b>lo]; assert cover and cover[0][0]==lo and cover[-1][1]==hi and all(cover[i][1]==cover[i+1][0] for i in range(len(cover)-1)),"UEFI RAM entries do not cover the complete loaded kernel"; claimed=int(re.search(r"\[efi\] mmap [0-9]+ entries, ([0-9]+) MiB available",t).group(1)); actual=sum(b-a for a,b in usable)>>20; assert actual==claimed,(actual,claimed); print("FOURTH-WAY FACTS uefi_regions=%d usable_mib=%d rsdp=present framebuffer=absent"%(len(rows),actual)); print("PASS: four-way consumer differential -- BIOS three paths agree exactly; UEFI agrees on RSDP/framebuffer presence and has internally consistent usable RAM (descriptor boundaries intentionally not compared)")'

test-uefi-native: test-uefi-native-negctl test-uefi-native-differential $(ESP_NATIVE_IMG) $(DISK)
	@if [ -z "$(UEFI_NATIVE_OVMF_CODE)" ] || [ -z "$(UEFI_NATIVE_OVMF_VARS)" ] || ! command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    echo 'SKIP: test-uefi-native needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	    exit 0; \
	fi; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_IMG)" "$(DISK)" 1024 "$(UEFI_NATIVE_LOG_DIR)/positive.log" "LOGIT BOOT IDENTITY VERIFY promised=0000000040000000|LOGIT_BOOT_NATIVE_OK version=0001|MB2 RESULT PASS|[smp] 2/2 CPUs online|LOGIT_BOOT_OK" -; \
	echo 'PASS: test-uefi-native OVMF reached LOGIT_BOOT_OK through native long-mode entry'

# ---------------------------------------------------------------------------
# test-uefi -- THE GATE.
# ---------------------------------------------------------------------------
# $(DISK) is the same LogitFS data disk every other boot harness in this tree
# mounts (tests/boot/run-test.sh's own $(DISK) argument) -- this milestone
# changes nothing about the filesystem or what is on it, only how the kernel
# gets STARTED, so reusing it is the point, not a shortcut.
test-uefi: test-uefi-load-policy-host test-uefi-pcide-host test-uefi-la57-host $(ESP_IMG) $(ESP_PYFAT_IMG) $(DISK)
	@if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] && [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then \
	    bash tests/boot/run-uefi-test.sh $(ESP_IMG) $(DISK); \
	    echo "--- repeating against the from-scratch (no-mtools) FAT16 ESP, to prove"; \
	    echo "    that path is not merely mdir-clean but actually boots OVMF ---"; \
	    bash tests/boot/run-uefi-test.sh $(ESP_PYFAT_IMG) $(DISK); \
	elif [ -n "$(UEFI_NATIVE_OVMF_CODE)" ] && [ -n "$(UEFI_NATIVE_OVMF_VARS)" ] && command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    $(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_IMG)" "$(DISK)" 1024 "$(UEFI_NATIVE_LOG_DIR)/mb2-positive.log" "[efi] descent ICPLJ|LOGIT_BOOT_OK" -; \
	    echo "--- repeating against the from-scratch (no-mtools) FAT16 ESP, to prove"; \
	    echo "    that path is not merely mdir-clean but actually boots OVMF ---"; \
	    $(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_PYFAT_IMG)" "$(DISK)" 1024 "$(UEFI_NATIVE_LOG_DIR)/mb2-pyfat-positive.log" "[efi] descent ICPLJ|LOGIT_BOOT_OK" -; \
	else \
	    echo 'SKIP: test-uefi needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	fi

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
	@if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] && [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then \
	    bash tests/boot/run-uefi-test.sh $(ESP_BADMAGIC_IMG) $(DISK) negctl; \
	elif [ -n "$(UEFI_NATIVE_OVMF_CODE)" ] && [ -n "$(UEFI_NATIVE_OVMF_VARS)" ] && command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    $(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_BADMAGIC_IMG)" "$(DISK)" 1024 "$(UEFI_NATIVE_LOG_DIR)/mb2-bad-magic.log" "[efi] descent ICPLJ" LOGIT_BOOT_OK; \
	else \
	    echo 'SKIP: test-uefi-negctl needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	fi

# The old claim here said the normal positive emits FICPLJ. Measured under OVMF
# on 2026-09-15, the unforced MB2 build emits ICPLJ and reaches LOGIT_BOOT_OK;
# F belongs to the EFI_FORCE_PCIDE specimens below, not the default build. Both
# controls force PCIDE but remove or delay its clear; each must emit FU and halt
# before the far jump. U comes from a fresh CR4 readback in 64-bit paged mode.
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
