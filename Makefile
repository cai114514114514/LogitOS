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
FS_FILES    := $(filter-out fsroot/fonts,$(wildcard fsroot/*))
FONTS       := fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf

CC          := clang
LD          := ld.lld
ASM         := nasm
GRUB_RESCUE := i686-elf-grub-mkrescue
QEMU        := qemu-system-x86_64

CFLAGS  := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 -g -Iinclude
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -T linker.ld

# Userland (ring 3) build flags
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 -Iinclude

# net/{dom,css,layout,paint}.c are compiled into the ring-3 browser (M17 L1), not the kernel.
C_SRC   := $(filter-out net/dom.c net/css.c net/layout.c net/paint.c,\
           $(wildcard kernel/*.c drivers/*.c lib/*.c fs/*.c net/*.c crypto/*.c))
ASM_SRC := $(wildcard boot/*.asm)
OBJ     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))

.PHONY: all run debug test clean

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# roots.c #includes the generated bundle; rebuild it when the bundle changes.
$(BUILD)/crypto/roots.o: include/roots_bundle.inc include/roots.h

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

# browser is multi-file (links QuickJS) -- defined below, not via APP_RULE.
APPS := clock textedit monitor terminal netapp
AEX  := $(foreach a,$(APPS),$(BUILD)/$(a).aex) $(BUILD)/browser.aex $(BUILD)/js.aex

# --- QuickJS engine + musl libm + mini-libc, shared by the JS app and Browser ---
QJS_SRC    := third_party/quickjs/quickjs.c third_party/quickjs/cutils.c \
              third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c \
              third_party/quickjs/libbf.c
ENGINE_SRCS:= $(QJS_SRC) $(wildcard third_party/libm/*.c) $(wildcard user/libc/src/*.c)
JS_INC     := -Iuser/libc/include -Ithird_party/libm -Ithird_party/quickjs
JS_CF      := $(UCFLAGS) -w -include features.h -DCONFIG_VERSION='"aqua-2024"' -DAQUA_OS $(JS_INC)
ENGINE_OBJ := $(patsubst %.c,$(BUILD)/jsobj/%.o,$(ENGINE_SRCS))

$(BUILD)/jsobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(JS_CF) -c $< -o $@

$(BUILD)/js.crt0.o: user/crt0.asm
	@mkdir -p $(BUILD)
	$(ASM) -f elf64 user/crt0.asm -o $@

$(BUILD)/js.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/user/js.o $(BUILD)/js.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x46000000 -o $@ $(BUILD)/js.crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/user/js.o

$(BUILD)/js.aex: $(BUILD)/js.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/js.elf $@ JavaScript - 'J' 230 200 60

# M17 L1: render pipeline + image codecs compiled into the ring-3 browser.
# (net/css.c retired in L2 -> replaced by LibCSS via user/css_engine.c below.)
BROWSER_PIPE := net/dom.c net/layout.c \
                user/browser_rt.c user/browser_paint.c \
                lib/inflate.c lib/png.c lib/gif.c lib/img.c
BROWSER_OBJ  := $(patsubst %.c,$(BUILD)/browserobj/%.o,$(BROWSER_PIPE))

$(BUILD)/browserobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iuser -c $< -o $@

# M17 L2: NetSurf LibCSS (+ libparserutils + libwapcaplet) + our css_engine.c
# adapter (produces struct cstyle), all compiled into the ring-3 browser.
CSS_DIR := third_party/css
CSS_INC := -I$(CSS_DIR)/libwapcaplet/include -I$(CSS_DIR)/libparserutils/include \
           -I$(CSS_DIR)/libcss/include -I$(CSS_DIR)/libcss/src -I$(CSS_DIR)/libparserutils/src
CSS_SRC := $(shell find $(CSS_DIR) -name '*.c' ! -name css_property_parser_gen.c)
CSS_OBJ := $(patsubst %.c,$(BUILD)/cssobj/%.o,$(CSS_SRC)) $(BUILD)/cssobj/user/css_engine.o

$(BUILD)/cssobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) -Iuser/libc/include -Iuser -c $< -o $@

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/user/browser.o $(BROWSER_OBJ) $(CSS_OBJ) $(BUILD)/js.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ $(BUILD)/js.crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/user/browser.o $(BROWSER_OBJ) $(CSS_OBJ)

$(BUILD)/browser.aex: $(BUILD)/browser.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser.elf $@ Browser - 'B' 120 130 240

# Font subsets (proprietary source; regenerated, .gitignored). See tools/mkfont.py.
$(FONTS): tools/mkfont.py
	@mkdir -p fsroot/fonts
	python3 tools/mkfont.py fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf

$(DISK): $(FS_FILES) $(FONTS) $(AEX) tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex) $(BUILD)/browser.aex:browser.aex $(BUILD)/js.aex:js.aex

QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk -boot d
QEMU_RAM  := -m 512M                # headroom for the loaded fonts + glyph cache
QEMU_RTC  := -rtc base=localtime    # show the host's local wall-clock time
# e1000 NIC on QEMU user (SLIRP) networking: gw 10.0.2.2, DNS 10.0.2.3, guest
# 10.0.2.15. filter-dump writes every frame to a pcap for forensic verification.
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_RTC) $(QEMU_NET) -serial stdio -no-reboot

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_RTC) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: $(ISO) $(DISK)
	@sh scripts/run-test.sh $(ISO) $(DISK)

clean:
	rm -rf $(BUILD)
