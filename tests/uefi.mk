# tests/uefi.mk -- the ESP and the gate for LogitOS's own UEFI loader.
#
# Own fragment for the reason every other one in this tree gives (see
# tests/mem.mk, tests/desktop.mk, ...): several lines share this Makefile, and
# a whole-file overwrite from a concurrent edit has silently deleted somebody
# else's targets before. The only token the main Makefile needs is the single
# `-include tests/uefi.mk` line below (NOT added here -- the Makefile is
# contended by another workflow right now; see the orchestrator report).
#
#   make test-uefi-native   boot the native-only loader under OVMF, including
#                            the protocol controls and independent GRUB oracle.
#
# ==========================================================================
# THE CONTRACT WITH c/boot/efi/build.sh
# ==========================================================================
# build.sh is owned by the loader itself. This fragment calls it through the
# two env vars
# ITS OWN header documents (read that file before changing the invocations
# below -- these are transcribed from there, not guessed at):
#
#   EFI_OUT       where BOOTX64.EFI is written (build.sh defaults this to
#                 build/BOOTX64.EFI; every call here overrides it so the
#                 normal build and the negctl build cannot land on the same
#                 path and race each other -- build.sh derives its object
#                 directory from EFI_OUT's basename for exactly that reason).
#   EFI_CPPFLAGS  extra -D flags for the native bad-version, high-info and
#                 early-EBS controls.
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
.PHONY: test-uefi-load-policy-host test-uefi-load-policy-negctl

# The production range validator is host-testable without pretending a host
# test is firmware. The negative target mutates its one type check so ACPI NVS
# is accepted; the fixture's first forbidden-range assertion must then go red.
test-uefi-load-policy-negctl:
	@python3 tests/unit/efi_load_policy_run.py --mode negctl --build $(EFI_LOAD_POLICY_BUILD)

test-uefi-load-policy-host: test-uefi-load-policy-negctl
	@python3 tests/unit/efi_load_policy_run.py --mode positive --build $(EFI_LOAD_POLICY_BUILD)

EFI_DIR              := $(BUILD)/efi
EFI_SRC              := $(wildcard c/boot/efi/*)
BOOTX64_EFI          := $(EFI_DIR)/BOOTX64.EFI
BOOTX64_NATIVE_EFI := $(EFI_DIR)/BOOTX64-native.EFI
BOOTX64_NATIVE_BADVER_EFI := $(EFI_DIR)/BOOTX64-native-bad-version.EFI
BOOTX64_NATIVE_HIGHINFO_EFI := $(EFI_DIR)/BOOTX64-native-high-info.EFI
BOOTX64_NATIVE_EARLYEBS_EFI := $(EFI_DIR)/BOOTX64-native-early-ebs.EFI

ESP_IMG          := $(BUILD)/esp.img
ESP_PYFAT_IMG    := $(BUILD)/esp-pyfat.img
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

# The native-only production source is rebuilt under separate names so control
# objects cannot race the ordinary image.
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

# The BIOS native dump is compared byte-for-byte with the GRUB oracle because
# both see SeaBIOS E820. The oracle kernel alone contains the retired entry;
# it is never a shipping artifact. OVMF is a different firmware and its 100+
# descriptors are not that map, so the UEFI specimen compares only honest
# consumer facts: RSDP present,
# framebuffer absent under the same -vga none condition, ordered/non-overlapping
# regions, loader available-byte accounting, and complete RAM coverage of the
# loaded kernel. It explicitly does NOT compare descriptor boundaries, ACPI
# revision/addresses, or demand identical region counts.
test-uefi-native-differential: $(ESP_NATIVE_IMG) \
    $(BUILD)/bios-native/native-dump.iso $(BUILD)/bios-mb2/grub-dump.iso
	@python3 tests/unit/bios_mb2_test.py --qemu $(QEMU) compare \
	    $(BUILD)/bios-native/native-dump.iso $(BUILD)/bios-mb2/grub-dump.iso
	@if [ -z "$(UEFI_NATIVE_OVMF_CODE)" ] || [ -z "$(UEFI_NATIVE_OVMF_VARS)" ] || ! command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    echo 'SKIP: fourth UEFI differential needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	    exit 0; \
	fi; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_NATIVE_IMG)" - 1024 "$(UEFI_NATIVE_LOG_DIR)/differential.log" "LOGIT_BOOT_NATIVE_OK version=0001|[efi] gop none|MB2 ACPI|MB2 RESULT PASS" "MB2 FB"; \
	python3 -c 'import pathlib,re; t=pathlib.Path("$(UEFI_NATIVE_LOG_DIR)/differential.log").read_text(errors="replace"); rows=[tuple(int(x,16) for x in m) for m in re.findall(r"MB2 MMAP ([0-9A-F]+) ([0-9A-F]+) ([0-9A-F]+)",t)]; assert rows,"UEFI dump has no memory regions"; assert all(n>0 for _,n,_ in rows),"zero-length UEFI region"; assert all(rows[i][0]+rows[i][1] <= rows[i+1][0] for i in range(len(rows)-1)),"UEFI regions overlap or are out of order"; usable=[(a,a+n) for a,n,k in rows if k==1]; lo,hi=(int(x,16) for x in re.search(r"\[efi\] load reserved 0x([0-9a-f]+)\.\.0x([0-9a-f]+)",t).groups()); cover=[(max(a,lo),min(b,hi)) for a,b in usable if a<hi and b>lo]; assert cover and cover[0][0]==lo and cover[-1][1]==hi and all(cover[i][1]==cover[i+1][0] for i in range(len(cover)-1)),"UEFI RAM entries do not cover the complete loaded kernel"; claimed=int(re.search(r"\[efi\] mmap [0-9]+ entries, ([0-9]+) MiB available",t).group(1)); actual=sum(b-a for a,b in usable)>>20; assert actual==claimed,(actual,claimed); print("FOURTH-WAY FACTS uefi_regions=%d usable_mib=%d rsdp=present framebuffer=absent"%(len(rows),actual)); print("PASS: four-way consumer differential -- BIOS three paths agree exactly; UEFI agrees on RSDP/framebuffer presence and has internally consistent usable RAM (descriptor boundaries intentionally not compared)")'

test-uefi-native: test-uefi-load-policy-host test-uefi-native-negctl \
    test-uefi-native-differential $(ESP_IMG) $(DISK)
	@if [ -z "$(UEFI_NATIVE_OVMF_CODE)" ] || [ -z "$(UEFI_NATIVE_OVMF_VARS)" ] || ! command -v $(firstword $(QEMU)) >/dev/null 2>&1; then \
	    echo 'SKIP: test-uefi-native needs qemu-system-x86_64 plus OVMF CODE/VARS; set UEFI_NATIVE_OVMF_CODE and UEFI_NATIVE_OVMF_VARS'; \
	    exit 0; \
	fi; \
	$(UEFI_NATIVE_RUN) "$(QEMU)" "$(UEFI_NATIVE_OVMF_CODE)" "$(UEFI_NATIVE_OVMF_VARS)" "$(ESP_IMG)" "$(DISK)" 1024 "$(UEFI_NATIVE_LOG_DIR)/positive.log" "LOGIT BOOT IDENTITY VERIFY promised=0000000040000000|LOGIT_BOOT_NATIVE_OK version=0001|[smp] 2/2 CPUs online|LOGIT_BOOT_OK" -; \
	echo 'PASS: test-uefi-native OVMF reached LOGIT_BOOT_OK through native long-mode entry'

# Historical command name retained for one release; it now reaches the same
# native gate and cannot be mistaken for a second protocol path.
test-uefi: test-uefi-native

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
	@# arrangement the native OVMF gate proves -- do not "simplify" it to one bus
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

.PHONY: test-uefi esp run-uefi
