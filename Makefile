# ============================================================================
# Aether OS - build system
#
#   make        build the bootable ISO
#   make run    boot it in QEMU (VGA window + serial on this terminal)
#   make debug  boot under QEMU with a gdb stub on :1234 (frozen at start)
#   make test   headless boot, assert the kernel reaches 64-bit C
#   make clean  remove build artifacts
#
# Source layout: everything lives under src/ (boot, kernel, drivers, fs, net,
# crypto, lib, apps). Headers sit next to their .c; header names are unique, so
# -I covers every source dir (generated below) and #include "foo.h" just works.
# include/ holds only the cross-cutting kernel<->user ABI.
# ============================================================================

ARCH        := x86_64
BUILD       := build
ISO_DIR     := $(BUILD)/iso
KERNEL      := $(BUILD)/kernel.elf
ISO         := $(BUILD)/aether.iso
DISK        := $(BUILD)/disk.img
FS_FILES    := $(filter-out fsroot/fonts fsroot/as,$(wildcard fsroot/*))
# AetherScript layout: example scripts (source, run directly) vs library modules
# (precompiled to .la). Packed to /usr/as/examples/ and /usr/as/lib/ respectively.
AS_EXAMPLES := $(wildcard fsroot/as/examples/*.as)
AS_LIB_SRCS := $(wildcard fsroot/as/lib/*.as)
AS_LA       := $(patsubst fsroot/as/lib/%.as,$(BUILD)/%.la,$(AS_LIB_SRCS))
FONTS       := fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf

CC          := clang
LD          := ld.lld
ASM         := nasm
GRUB_RESCUE := i686-elf-grub-mkrescue
QEMU        := qemu-system-x86_64

# Colocated headers resolve via -I across every source dir (names are unique).
INCDIRS := $(addprefix -I,$(shell find src include -type d))

CFLAGS  := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 -g $(INCDIRS)
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -T linker.ld

# Userland (ring 3) build flags
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 $(INCDIRS)

# Kernel sources. The browser render pipeline lives in src/apps/browser, not here.
C_SRC   := $(shell find src/kernel src/drivers src/lib src/fs src/net src/crypto -name '*.c')
ASM_SRC := $(wildcard src/boot/*.asm)
OBJ     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))

.PHONY: all run debug test test-nvme clean test-as test-as-gcstress test-shell test-as-os test-smp test-tcp-host

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# roots.c #includes the generated bundle; rebuild it when the bundle changes.
$(BUILD)/src/crypto/trust/roots.o: src/crypto/trust/roots_bundle.inc src/crypto/trust/roots.h

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
APPDIR := src/apps
# GUIDIR = windowed apps (link aether.h + crt0.asm); CLIDIR = shell + coreutils (clib.h + crt0_cli.asm)
GUIDIR := src/apps/gui
CLIDIR := src/apps/coreutils
# the aui widget toolkit (immediate-mode), compiled once + linked into every GUI app
$(BUILD)/apps/aui.o: $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/aether.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $(GUIDIR)/aui.c -o $@

define APP_RULE
$(BUILD)/$(1).elf: $(GUIDIR)/$(1).c $(APPDIR)/crt0.asm $(APPDIR)/aether.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/$(1).crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/$(1).c -o $(BUILD)/apps/$(1).o
	$(LD) -nostdlib -e _start -Ttext=$(strip $(2)) -o $$@ $(BUILD)/apps/$(1).crt0.o $(BUILD)/apps/$(1).o $(BUILD)/apps/aui.o
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(3) $(4) '$(5)' $(6) $(7) $(8)
endef

#                     name      base       display  ext icon r   g   b   ('-' ext = none)
$(eval $(call APP_RULE,clock,   0x40000000,Clock,-,C,100,160,255))
$(eval $(call APP_RULE,textedit,0x41000000,TextEdit,txt,T,90,200,120))
$(eval $(call APP_RULE,monitor, 0x42000000,Monitor,-,M,255,100,100))
$(eval $(call APP_RULE,terminal,0x43000000,Terminal,-,>,70,80,100))
$(eval $(call APP_RULE,widgets, 0x46000000,Widgets,-,W,150,120,230))
$(eval $(call APP_RULE,files,   0x47000000,Finder,-,F,120,190,140))
$(eval $(call APP_RULE,preview, 0x48000000,Preview,-,P,200,150,110))

# browser is multi-file (links QuickJS) -- defined below, not via APP_RULE.
# (Network app removed -- its ping/dns/ifconfig moved to the `net` coreutil.)
APPS := clock textedit monitor terminal widgets files preview

# --- CLI programs (sh + coreutils): exec'able ring-3 programs, all linked at a
# common base inside the private user region (0x40000000..0x7FFFFFFF). They are
# packed under /bin (not scanned by the Dock) and launched via fork+execve. ---
define CLI_RULE
$(BUILD)/$(1).elf: $(CLIDIR)/$(1).c $(APPDIR)/crt0_cli.asm $(APPDIR)/clib.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$(CC) $(UCFLAGS) -c $(CLIDIR)/$(1).c -o $(BUILD)/apps/$(1).cli.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $$@ $(BUILD)/apps/$(1).crt0c.o $(BUILD)/apps/$(1).cli.o
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
endef

CLI := sh echo ls cat pwd wc head true false sleep mkdir rm touch clear uname net cp mv smptest
$(foreach c,$(CLI),$(eval $(call CLI_RULE,$(c))))
CLI_AEX := $(foreach c,$(CLI),$(BUILD)/$(c).aex)

AEX  := $(foreach a,$(APPS),$(BUILD)/$(a).aex) $(BUILD)/browser.aex $(CLI_AEX) $(BUILD)/as.aex

# --- QuickJS engine + musl libm + mini-libc, shared by the JS app and Browser ---
QJS_SRC    := third_party/quickjs/quickjs.c third_party/quickjs/cutils.c \
              third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c \
              third_party/quickjs/libbf.c
ENGINE_SRCS:= $(QJS_SRC) $(wildcard third_party/libm/*.c) $(wildcard src/apps/libc/src/*.c)
JS_INC     := -Ithird_party/libm -Ithird_party/quickjs    # mini-libc covered by INCDIRS
JS_CF      := $(UCFLAGS) -w -include features.h -DCONFIG_VERSION='"aether-2024"' -DAETHER_OS -DCONFIG_STACK_CHECK $(JS_INC)
ENGINE_OBJ := $(patsubst %.c,$(BUILD)/jsobj/%.o,$(ENGINE_SRCS))

# mini-libc asm helpers (setjmp/longjmp) join the engine bundle.
LIBC_ASM    := $(wildcard src/apps/libc/src/*.asm)
ENGINE_OBJ  += $(patsubst %.asm,$(BUILD)/jsobj/%.o,$(LIBC_ASM))

$(BUILD)/jsobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(JS_CF) -c $< -o $@

$(BUILD)/jsobj/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/apps/crt0.o: $(APPDIR)/crt0.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $@

# --- browser: render pipeline + image codecs + LibCSS, all into one ring-3 app ---
BROWSER_PIPE := src/apps/browser/dom.c src/apps/browser/layout.c \
                src/apps/browser/browser_rt.c src/apps/browser/browser_paint.c \
                src/apps/browser/css_vars.c \
                src/lib/image/inflate.c src/lib/image/png.c src/lib/image/gif.c src/lib/image/jpeg.c src/lib/image/img.c
BROWSER_OBJ  := $(patsubst %.c,$(BUILD)/browserobj/%.o,$(BROWSER_PIPE))

$(BUILD)/browserobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# NetSurf LibCSS (+ libparserutils + libwapcaplet) + our css_engine.c adapter.
CSS_DIR := third_party/css
CSS_INC := -I$(CSS_DIR)/libwapcaplet/include -I$(CSS_DIR)/libparserutils/include \
           -I$(CSS_DIR)/libcss/include -I$(CSS_DIR)/libcss/src -I$(CSS_DIR)/libparserutils/src
CSS_SRC := $(shell find $(CSS_DIR) -name '*.c' ! -name css_property_parser_gen.c)
CSS_OBJ := $(patsubst %.c,$(BUILD)/cssobj/%.o,$(CSS_SRC)) $(BUILD)/cssobj/src/apps/browser/css_engine.o

$(BUILD)/cssobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) -c $< -o $@

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/src/apps/browser/browser.o $(BUILD)/jsobj/src/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ) $(BUILD)/apps/crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/src/apps/browser/browser.o $(BUILD)/jsobj/src/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ)

$(BUILD)/browser.aex: $(BUILD)/browser.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser.elf $@ Browser - 'B' 120 130 240

# --- AetherScript: /bin/as -- a ring-3 CLI program. Links the as core + mini-libc
# (fopen/malloc/snprintf/strtod) at the common CLI base via crt0_cli. (CLI_RULE
# can't be reused: those programs use aether.h inline syscalls, not mini-libc.) ---
AS_C    := $(wildcard src/apps/as/*.c)
AS_LIBC := $(wildcard src/apps/libc/src/*.c)
AS_LASM := $(wildcard src/apps/libc/src/*.asm)
AS_OBJ  := $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_C)) \
            $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_LIBC)) \
            $(patsubst %.asm,$(BUILD)/asobj/%.o,$(AS_LASM))
# as.h carries AS_BC_VERSION + the opcode enum; depend on it so a version bump
# rebuilds EVERY asobj (esp. as_bc.o, whose .c rarely changes) -- otherwise a
# stale as_bc.o in /bin/as rejects the freshly-bumped .la files on Aether.
AS_HDRS := $(wildcard src/apps/as/*.h)

$(BUILD)/asobj/%.o: %.c $(AS_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/asobj/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/as.elf: $(AS_OBJ) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/as.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/as.crt0c.o $(AS_OBJ)
$(BUILD)/as.aex: $(BUILD)/as.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/as.elf $@ as - '*' 150 150 150

# Font subsets (proprietary source; regenerated, .gitignored). See tools/mkfont.py.
$(FONTS): tools/mkfont.py
	@mkdir -p fsroot/fonts
	python3 tools/mkfont.py fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf

$(DISK): $(FS_FILES) $(AS_EXAMPLES) $(AS_LA) $(FONTS) $(AEX) tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex) $(BUILD)/browser.aex:browser.aex \
	    $(foreach c,$(CLI),$(BUILD)/$(c).aex:/bin/$(c)) $(BUILD)/as.aex:/bin/as \
	    $(foreach e,$(AS_EXAMPLES),$(e):/usr/as/examples/$(notdir $(e))) \
	    $(foreach l,$(AS_LA),$(l):/usr/as/lib/$(notdir $(l)))

QEMU_DISK := -drive file=$(DISK),format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d
QEMU_RAM  := -m 512M                # headroom for the loaded fonts + glyph cache
QEMU_SMP  := -smp 4 -accel tcg,thread=multi   # 4 cores, parallel TCG threads
QEMU_RTC  := -rtc base=localtime    # show the host's local wall-clock time
QEMU_GPU  := -vga none -device virtio-gpu-pci   # modern GPU; kernel drives the scanout
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: $(ISO) $(DISK)
	@sh scripts/run-test.sh $(ISO) $(DISK)

# Same smoke test, but attach the disk via NVMe -- proves the from-scratch NVMe
# driver brings up a controller and aetherfs mounts + reads off it (M24 bare-metal).
test-nvme: $(ISO) $(DISK)
	@BLK=nvme sh scripts/run-test.sh $(ISO) $(DISK)

test-shell: $(ISO) $(DISK)
	@sh scripts/run-shell-test.sh $(ISO) $(DISK)

# Host unit test for TCP out-of-order reassembly (white-box: #includes tcp.c).
# Stub headers in tools/t/tcpstub let tcp.c compile on the host (no x86 asm).
test-tcp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/tcp_test tools/t/tcp_test.c -Itools/t/tcpstub -Isrc/net/transport
	@./$(BUILD)/tcp_test

# On-Aether AetherScript test: boots and runs /bin/as on the /usr/as examples.
test-as-os: $(ISO) $(DISK)
	@sh scripts/run-as-test.sh $(ISO) $(DISK)

# M25 SMP concurrency proof: boots -smp 4, runs /bin/smptest, asserts SMP_TEST_OK
# (no cross-core corruption + genuine parallelism across >=2 cores).
test-smp: $(ISO) $(DISK)
	@sh scripts/run-smp-test.sh $(ISO) $(DISK)

# AetherScript host unit test: the language core (lexer/compiler/vm/value/object)
# is portable C, so it builds and runs natively -- no QEMU. Asserts print output
# for arithmetic/control-flow/recursion incl. fib(20).
AS_CORE := src/apps/as/value.c src/apps/as/as_io.c src/apps/as/lexer.c \
            src/apps/as/compiler.c src/apps/as/vm.c src/apps/as/object.c \
            src/apps/as/as_native.c src/apps/as/as_ll.c src/apps/as/as_bc.c
test-as:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/as_test tools/t/as_test.c $(AS_CORE) -Isrc/apps/as -Iinclude/abi
	@$(BUILD)/as_test

# GC stress: collect before EVERY allocation -> any missing GC root becomes a crash
# or wrong output. Runs the same host unit suite under -DAS_GC_STRESS.
test-as-gcstress:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_GC_STRESS -o $(BUILD)/as_test_gcstress tools/t/as_test.c $(AS_CORE) -Isrc/apps/as -Iinclude/abi
	@$(BUILD)/as_test_gcstress

# Host `asc`: the as core + the as.c entry built natively (no --target -> arm64
# host binary), used at `make` time to precompile the stdlib .as to .la. `-c`
# mode never invokes the syscall path, so the arm64 as_ll.c stub is fine. Host
# and target share AS_CORE/as.h, so AS_BC_VERSION + the opcode enum match and a
# host-produced .la loads on Aether.
ASC := $(BUILD)/asc
# as.h carries AS_BC_VERSION + the opcode enum; list it so a version bump or
# opcode change forces asc (and therefore every .la) to rebuild. Without this
# dep a bumped AS_BC_VERSION silently keeps stale .la files that the kernel's
# as_load then rejects (cf. the roots_bundle.inc dep gotcha).
$(ASC): $(AS_CORE) src/apps/as/as.c src/apps/as/as.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -o $@ src/apps/as/as.c $(AS_CORE) -Isrc/apps/as -Iinclude/abi

# Precompile the LibAether library modules (fsroot/as/lib/*.as) to .la (compiled
# bytecode). -c is compile-only (no run), so even a lib with module-mate calls
# (mathx) is fine; packed to /usr/as/lib/.
$(BUILD)/%.la: fsroot/as/lib/%.as $(ASC)
	$(ASC) -c $< -o $@

# PNG decoder host test: PIL generates a matrix of cases (colour types, bit depths,
# Adam7, tRNS) as ground truth; our decoder must match byte-for-byte. Needs PIL.
test-png:
	@mkdir -p $(BUILD)/pngtest
	@python3 tools/t/png_gen.py $(BUILD)/pngtest
	@$(CC) -O2 -o $(BUILD)/png_test tools/t/png_test.c \
	    src/lib/image/img.c src/lib/image/png.c src/lib/image/gif.c src/lib/image/jpeg.c src/lib/image/inflate.c \
	    -Isrc/lib/image -Isrc/kernel/mm
	@$(BUILD)/png_test $(BUILD)/pngtest

# JPEG baseline decoder host test: PIL encodes baseline JPEGs (grayscale + colour,
# 4:4:4 / 4:2:2 / 4:2:0), and we decode the IDENTICAL bytes with libjpeg djpeg
# (-nosmooth = box chroma upsample, matching ours) as the reference. JPEG is lossy,
# so we compare two decoders of the same bytes within a tight per-channel tolerance,
# never against the original pixels. Also asserts progressive/CMYK fail gracefully.
# Needs PIL + djpeg. (Ad-hoc tests tools/t/img_test.c, tools/t/img_fuzz.c have no
# target; if run by hand, add src/lib/image/jpeg.c to their source list.)
test-jpeg:
	@mkdir -p $(BUILD)/jpegtest
	@python3 tools/t/jpeg_gen.py $(BUILD)/jpegtest
	@$(CC) -O2 -o $(BUILD)/jpeg_test tools/t/jpeg_test.c \
	    src/lib/image/img.c src/lib/image/png.c src/lib/image/gif.c src/lib/image/jpeg.c src/lib/image/inflate.c \
	    -Isrc/lib/image -Isrc/kernel/mm
	@$(BUILD)/jpeg_test $(BUILD)/jpegtest

clean:
	rm -rf $(BUILD)
