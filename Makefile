# ============================================================================
# Aqua OS - build system (Milestone 1)
#
#   make        build the bootable ISO
#   make run    boot it in QEMU (VGA window + serial on this terminal)
#   make debug  boot under QEMU with a gdb stub on :1234 (frozen at start)
#   make test   headless boot, assert the kernel reaches 64-bit C
#   make clean  remove build artifacts
# ============================================================================

ARCH        := x86_64
BUILD       := build
ISO_DIR     := $(BUILD)/iso
KERNEL      := $(BUILD)/kernel.elf
ISO         := $(BUILD)/aqua.iso
DISK        := $(BUILD)/disk.img
FS_FILES    := $(wildcard fsroot/*)

CC          := clang
LD          := ld.lld
ASM         := nasm
GRUB_RESCUE := i686-elf-grub-mkrescue
QEMU        := qemu-system-x86_64

CFLAGS  := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -std=c11 -Wall -Wextra -O2 -g -Iinclude
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -T linker.ld

# Userland (ring 3) build flags
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -std=c11 -Wall -Wextra -O2 -Iinclude

C_SRC   := $(wildcard kernel/*.c drivers/*.c lib/*.c fs/*.c net/*.c)
ASM_SRC := $(wildcard boot/*.asm)
OBJ     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))

.PHONY: all run debug test clean

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

$(ISO): $(KERNEL) grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_RESCUE) -o $@ $(ISO_DIR)

# --- userland applications (.aex), each a ring-3 process ---
# APP_RULE: name, link base, display name, ext, icon-glyph, "r g b" color
define APP_RULE
$(BUILD)/$(1).elf: user/$(1).c user/crt0.asm user/aqua.h
	@mkdir -p $(BUILD)/user
	$(ASM) -f elf64 user/crt0.asm -o $(BUILD)/user/$(1).crt0.o
	$(CC) $(UCFLAGS) -c user/$(1).c -o $(BUILD)/user/$(1).o
	$(LD) -nostdlib -e _start -Ttext=$(strip $(2)) -o $$@ $(BUILD)/user/$(1).crt0.o $(BUILD)/user/$(1).o
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(3) $(4) '$(5)' $(6) $(7) $(8)
endef

#                     name      base       display  ext icon r   g   b   ('-' ext = none)
$(eval $(call APP_RULE,clock,   0x40000000,Clock,-,C,100,160,255))
$(eval $(call APP_RULE,textedit,0x41000000,TextEdit,txt,T,90,200,120))
$(eval $(call APP_RULE,monitor, 0x42000000,Monitor,-,M,255,100,100))
$(eval $(call APP_RULE,terminal,0x43000000,Terminal,-,>,70,80,100))
$(eval $(call APP_RULE,netapp,  0x44000000,Network,-,N,80,170,220))

APPS := clock textedit monitor terminal netapp
AEX  := $(foreach a,$(APPS),$(BUILD)/$(a).aex)

$(DISK): $(FS_FILES) $(AEX) tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex)

QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk -boot d
QEMU_RTC  := -rtc base=localtime    # show the host's local wall-clock time
# e1000 NIC on QEMU user (SLIRP) networking: gw 10.0.2.2, DNS 10.0.2.3, guest
# 10.0.2.15. filter-dump writes every frame to a pcap for forensic verification.
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RTC) $(QEMU_NET) -serial stdio -no-reboot

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RTC) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: $(ISO) $(DISK)
	@sh scripts/run-test.sh $(ISO) $(DISK)

clean:
	rm -rf $(BUILD)
