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
USER_ELF    := $(BUILD)/user.elf
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
           -std=c11 -Wall -Wextra -O2

C_SRC   := $(wildcard kernel/*.c drivers/*.c lib/*.c fs/*.c)
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

# --- userland program (ring 3) ---
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(USER_ELF): $(BUILD)/user/ustart.o $(BUILD)/user/user.o user/user.ld
	$(LD) -nostdlib -T user/user.ld -o $@ $(BUILD)/user/ustart.o $(BUILD)/user/user.o

$(DISK): $(FS_FILES) $(USER_ELF) tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) $(USER_ELF):hello.elf

QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk -boot d

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) -serial stdio -no-reboot

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) -serial stdio -no-reboot -s -S

test: $(ISO) $(DISK)
	@sh scripts/run-test.sh $(ISO) $(DISK)

clean:
	rm -rf $(BUILD)
