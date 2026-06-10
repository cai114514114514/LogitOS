# ============================================================================
# Aether OS - build system
#
#   make        build the bootable ISO
#   make run    boot it in QEMU (VGA window + serial on this terminal)
#   make debug  boot under QEMU with a gdb stub on :1234 (frozen at start)
#   make test   headless boot, assert the kernel reaches 64-bit C
#   make clean  remove build artifacts
#
# Source layout: everything lives under c/ (boot, kernel, drivers, fs, net,
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
INCDIRS := $(addprefix -I,$(shell find C include -type d))

# -MMD -MP: every compile also emits a .d makefile fragment listing its real
# header dependencies (see the -include at the bottom). Without this, editing a
# shared header (e.g. percpu.h's struct cpu) only rebuilt the .c files git
# touched -- stale objects then disagreed about struct layouts and corrupted
# memory at runtime (the M25 P4 g_cpus skew).
CFLAGS  := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 -g -MMD -MP $(INCDIRS)

# Debug knobs (objects are NOT flag-tracked: touch the affected sources or
# `make clean` when toggling these):
#   make CHURN=1   app open/close churn stress in the WM loop (freeze repro)
#   make GROWFI=1  kheap grow() fault injection (exercise pmm-contig failure)
ifeq ($(CHURN),1)
CFLAGS += -DWM_CHURN_STRESS
endif
ifeq ($(GROWFI),1)
CFLAGS += -DKHEAP_GROW_FAULT_INJECT
endif
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -T linker.ld

# Userland (ring 3) build flags
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 \
           -std=c11 -Wall -Wextra -O2 -MMD -MP $(INCDIRS)

# Kernel sources. The browser render pipeline lives in c/apps/browser, not here.
# inflate.c + png.c are excluded: ported to Rust (rust/src/{inflate,png}.rs provide
# inflate_raw/zlib_decompress + png_register/png_detect/png_decode) -- the hybrid
# C+Rust modules. The .c files are deleted, so the filter-out is a defensive guard.
C_SRC   := $(filter-out c/lib/image/inflate.c c/lib/image/png.c,$(shell find c/kernel c/drivers c/lib c/fs c/net c/crypto -name '*.c'))
ASM_SRC := $(wildcard c/boot/*.asm)
OBJ     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))

# --- Hybrid C+Rust: a no_std staticlib (rust/) linked with the C objects. Rust
# owns the memory-safety-critical untrusted-input parsers; C owns the core. Use
# the RUSTUP toolchain's cargo/rustc (Homebrew's rust lacks cross targets); the
# x86_64-unknown-none std is `rustup target add x86_64-unknown-none`. ---
RUST_BIN  := $(shell rustup which cargo 2>/dev/null | xargs dirname)
RUST_LIB  := rust/target/x86_64-unknown-none/release/libaether_rust.a
RUST_SRC  := $(shell find rust/src -name '*.rs') rust/Cargo.toml

.PHONY: all run debug test test-nvme test-selfhost-lex clean test-as test-as-gcstress test-shell test-as-os test-smp test-tcp-host test-complete test-libc

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rust no_std staticlib (kept after `all:` so it never becomes the default goal).
$(RUST_LIB): $(RUST_SRC)
	cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release --target x86_64-unknown-none

# Same crate built for the HOST, for the host-side image tests (test-png/test-jpeg):
# the crate is no_std either way; the tests' own malloc shims satisfy kmalloc/kfree.
RUST_LIB_HOST := rust/target/release/libaether_rust.a
$(RUST_LIB_HOST): $(RUST_SRC)
	cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release

# roots.c #includes the generated bundle; rebuild it when the bundle changes.
$(BUILD)/c/crypto/trust/roots.o: c/crypto/trust/roots_bundle.inc c/crypto/trust/roots.h

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJ) $(RUST_LIB) linker.ld
	$(LD) $(LDFLAGS) -o $@ --start-group $(OBJ) $(RUST_LIB) --end-group

$(ISO): $(KERNEL) grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_RESCUE) -o $@ $(ISO_DIR)

# --- userland applications (.aex), each a ring-3 process ---
# APP_RULE: name, link base, display name, ext, icon-glyph, "r g b" color
APPDIR := c/apps
# GUIDIR = windowed apps (link aether.h + crt0.asm); CLIDIR = shell + coreutils (clib.h + crt0_cli.asm)
GUIDIR := c/apps/gui
CLIDIR := c/apps/coreutils
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
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ '$(3)' $(4) '$(5)' $(6) $(7) $(8)
endef

#                     name      base       display  ext icon r   g   b   ('-' ext = none)
$(eval $(call APP_RULE,clock,   0x40000000,Clock,-,C,100,160,255))
$(eval $(call APP_RULE,textedit,0x41000000,TextEdit,txt,T,90,200,120))
$(eval $(call APP_RULE,monitor, 0x42000000,Monitor,-,M,255,100,100))
$(eval $(call APP_RULE,terminal,0x43000000,Terminal,-,>,70,80,100))
$(eval $(call APP_RULE,widgets, 0x46000000,Widgets,-,W,150,120,230))
$(eval $(call APP_RULE,files,   0x47000000,Finder,-,F,120,190,140))
$(eval $(call APP_RULE,preview, 0x48000000,Preview,-,P,200,150,110))
# Code Studio links the AetherScript completion engine (complete.o) for IntelliSense.
$(BUILD)/apps/complete.o: c/apps/as/complete.c c/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c c/apps/as/complete.c -o $@
$(BUILD)/studio.elf: $(GUIDIR)/studio.c $(APPDIR)/crt0.asm $(APPDIR)/aether.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(BUILD)/apps/complete.o c/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/studio.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/studio.c -o $(BUILD)/apps/studio.o -Ic/apps/as
	$(LD) -nostdlib -e _start -Ttext=0x49000000 -o $@ $(BUILD)/apps/studio.crt0.o $(BUILD)/apps/studio.o $(BUILD)/apps/aui.o $(BUILD)/apps/complete.o
$(BUILD)/studio.aex: $(BUILD)/studio.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/studio.elf $@ 'Code Studio' as '{' 200 160 250

# browser is multi-file (links QuickJS) -- defined below, not via APP_RULE.
# (Network app removed -- its ping/dns/ifconfig moved to the `net` coreutil.)
APPS := clock textedit monitor terminal widgets files preview studio

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
ENGINE_SRCS:= $(QJS_SRC) $(wildcard third_party/libm/*.c) $(wildcard c/apps/libc/src/*.c)
JS_INC     := -Ithird_party/libm -Ithird_party/quickjs    # mini-libc covered by INCDIRS
JS_CF      := $(UCFLAGS) -w -include features.h -DCONFIG_VERSION='"aether-2024"' -DAETHER_OS -DCONFIG_STACK_CHECK $(JS_INC)
ENGINE_OBJ := $(patsubst %.c,$(BUILD)/jsobj/%.o,$(ENGINE_SRCS))

# mini-libc asm helpers (setjmp/longjmp) join the engine bundle.
LIBC_ASM    := $(wildcard c/apps/libc/src/*.asm)
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
# inflate + png are gone -- the ring-3 browser links the same Rust staticlib as the
# kernel (rust/src/{inflate,png}.rs provide zlib_decompress + png_*; the crate only
# calls kmalloc/kfree/img_register, which browser_rt.c shims into the ring-3 heap).
BROWSER_PIPE := c/apps/browser/dom.c c/apps/browser/layout.c \
                c/apps/browser/browser_rt.c c/apps/browser/browser_paint.c \
                c/apps/browser/css_vars.c \
                c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/img.c
BROWSER_OBJ  := $(patsubst %.c,$(BUILD)/browserobj/%.o,$(BROWSER_PIPE))

$(BUILD)/browserobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# NetSurf LibCSS (+ libparserutils + libwapcaplet) + our css_engine.c adapter.
CSS_DIR := third_party/css
CSS_INC := -I$(CSS_DIR)/libwapcaplet/include -I$(CSS_DIR)/libparserutils/include \
           -I$(CSS_DIR)/libcss/include -I$(CSS_DIR)/libcss/src -I$(CSS_DIR)/libparserutils/src
CSS_SRC := $(shell find $(CSS_DIR) -name '*.c' ! -name css_property_parser_gen.c)
CSS_OBJ := $(patsubst %.c,$(BUILD)/cssobj/%.o,$(CSS_SRC)) $(BUILD)/cssobj/c/apps/browser/css_engine.o

$(BUILD)/cssobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) -c $< -o $@

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/c/apps/browser/browser.o $(BUILD)/jsobj/c/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/c/apps/browser/browser.o $(BUILD)/jsobj/c/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) --end-group

$(BUILD)/browser.aex: $(BUILD)/browser.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser.elf $@ Browser - 'B' 120 130 240

# --- AetherScript: /bin/as -- a ring-3 CLI program. Links the as core + mini-libc
# (fopen/malloc/snprintf/strtod) at the common CLI base via crt0_cli. (CLI_RULE
# can't be reused: those programs use aether.h inline syscalls, not mini-libc.) ---
AS_C    := $(wildcard c/apps/as/*.c)
AS_LIBC := $(wildcard c/apps/libc/src/*.c)
AS_LASM := $(wildcard c/apps/libc/src/*.asm)
AS_OBJ  := $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_C)) \
            $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_LIBC)) \
            $(patsubst %.asm,$(BUILD)/asobj/%.o,$(AS_LASM))
# as.h carries AS_BC_VERSION + the opcode enum; depend on it so a version bump
# rebuilds EVERY asobj (esp. as_bc.o, whose .c rarely changes) -- otherwise a
# stale as_bc.o in /bin/as rejects the freshly-bumped .la files on Aether.
AS_HDRS := $(wildcard c/apps/as/*.h)

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

# /bin/libctest -- mini-libc on-target test battery (run by `make test-libc`).
# Links the real mini-libc objects already built for /bin/as + a test main, at
# the common CLI base via crt0_cli. (Host-native testing is awkward: string.c
# defines memcpy/etc. which clash with the host libc -- so we test on Aether.)
LIBC_OBJS := $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_LIBC)) $(patsubst %.asm,$(BUILD)/asobj/%.o,$(AS_LASM))
$(BUILD)/asobj/tests/unit/libctest_main.o: tests/unit/libctest_main.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/libctest.elf: $(BUILD)/asobj/tests/unit/libctest_main.o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/libctest.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/libctest.crt0c.o $(BUILD)/asobj/tests/unit/libctest_main.o $(LIBC_OBJS)
$(BUILD)/libctest.aex: $(BUILD)/libctest.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/libctest.elf $@ libctest - '?' 150 150 150

# Font subsets (proprietary source; regenerated, .gitignored). See tools/mkfont.py.
$(FONTS): tools/mkfont.py
	@mkdir -p fsroot/fonts
	python3 tools/mkfont.py fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf

$(DISK): $(FS_FILES) $(AS_EXAMPLES) $(AS_LA) $(FONTS) $(AEX) $(BUILD)/libctest.aex tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex) $(BUILD)/browser.aex:browser.aex \
	    $(foreach c,$(CLI),$(BUILD)/$(c).aex:/bin/$(c)) $(BUILD)/as.aex:/bin/as $(BUILD)/libctest.aex:/bin/libctest \
	    $(foreach e,$(AS_EXAMPLES),$(e):/usr/as/examples/$(notdir $(e))) \
	    $(foreach l,$(AS_LA),$(l):/usr/as/lib/$(notdir $(l))) \
	    $(foreach s,$(AS_LIB_SRCS),$(s):/usr/as/lib/$(notdir $(s)))

QEMU_DISK := -drive file=$(DISK),format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d
QEMU_RAM  := -m 512M                # headroom for the loaded fonts + glyph cache
# 4 cores, parallel TCG threads. Both system-freeze bugs are now fixed: the
# single-core WM IRQ-vs-render race (input deferral, commit ffd3b90) and the
# multi-core g_bkl/g_sched_lock ABBA deadlock (first-run threads now start IF=0,
# see docs/superpowers/specs/2026-06-08-smp-bkl-deadlock.md). Overridable, e.g.
# `make run QEMU_SMP="-smp 1"`. (On an Apple-Silicon host, thread=multi still has
# the separate QEMU-MTTCG FP/XMM artifact for FP-heavy ring-3 apps -- not a freeze;
# see the smp-mttcg note. Drop `,thread=multi` for correct-but-serial 4-core TCG.)
QEMU_SMP  ?= -smp 4 -accel tcg,thread=multi
QEMU_RTC  := -rtc base=localtime    # show the host's local wall-clock time
QEMU_GPU  := -vga none -device virtio-gpu-pci   # modern GPU; kernel drives the scanout
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: $(ISO) $(DISK)
	@sh tests/boot/run-test.sh $(ISO) $(DISK)

# Same smoke test, but attach the disk via NVMe -- proves the from-scratch NVMe
# driver brings up a controller and aetherfs mounts + reads off it (M24 bare-metal).
test-nvme: $(ISO) $(DISK)
	@BLK=nvme sh tests/boot/run-test.sh $(ISO) $(DISK)

test-shell: $(ISO) $(DISK)
	@sh tests/boot/run-shell-test.sh $(ISO) $(DISK)

# Host unit test for TCP out-of-order reassembly (white-box: #includes tcp.c).
# Stub headers in tests/unit/tcpstub let tcp.c compile on the host (no x86 asm).
test-tcp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/tcp_test tests/unit/tcp_test.c -Itests/unit/tcpstub -Ic/net/transport
	@./$(BUILD)/tcp_test

# On-Aether AetherScript test: boots and runs /bin/as on the /usr/as examples.
test-as-os: $(ISO) $(DISK)
	@sh tests/boot/run-as-test.sh $(ISO) $(DISK)

# mini-libc on-target test battery: boots Aether, runs /bin/libctest, asserts LIBC_OK.
test-libc: $(ISO) $(DISK)
	@sh tests/boot/run-libc-test.sh $(ISO) $(DISK)

# M25 SMP concurrency proof: boots -smp 4, runs /bin/smptest, asserts SMP_TEST_OK
# (no cross-core corruption + genuine parallelism across >=2 cores).
test-smp: $(ISO) $(DISK)
	@sh tests/boot/run-smp-test.sh $(ISO) $(DISK)

# AetherScript host unit test: the language core (lexer/compiler/vm/value/object)
# is portable C, so it builds and runs natively -- no QEMU. Asserts print output
# for arithmetic/control-flow/recursion incl. fib(20).
AS_CORE := c/apps/as/value.c c/apps/as/as_io.c c/apps/as/lexer.c \
            c/apps/as/compiler.c c/apps/as/vm.c c/apps/as/object.c \
            c/apps/as/as_native.c c/apps/as/as_ll.c c/apps/as/as_bc.c
test-as:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/as_test tests/unit/as_test.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_test

# libcomplete host unit tests: the completion engine is self-contained C, so it
# builds and runs natively -- no QEMU.
test-complete:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_COMPLETE_TEST -o $(BUILD)/complete_test tests/unit/complete_test.c c/apps/as/complete.c -Ic/apps/as
	@$(BUILD)/complete_test

# GC stress: collect before EVERY allocation -> any missing GC root becomes a crash
# or wrong output. Runs the same host unit suite under -DAS_GC_STRESS.
test-as-gcstress:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_GC_STRESS -o $(BUILD)/as_test_gcstress tests/unit/as_test.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
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
$(ASC): $(AS_CORE) c/apps/as/as.c c/apps/as/as.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -o $@ c/apps/as/as.c $(AS_CORE) -Ic/apps/as -Iinclude/abi

# Precompile the LibAether library modules (fsroot/as/lib/*.as) to .la (compiled
# bytecode). -c is compile-only (no run), so even a lib with module-mate calls
# (mathx) is fine; packed to /usr/as/lib/.
$(BUILD)/%.la: fsroot/as/lib/%.as $(ASC)
	$(ASC) -c $< -o $@

# M21-P3 self-hosting S1: the AetherScript lexer (lib/aslex.as) must emit a
# token stream byte-identical to the C lexer over the whole in-tree corpus.
test-selfhost-lex: $(BUILD)/asc
	@bash tests/unit/run-selfhost-lex.sh $(BUILD)/asc

# kheap host test: compiles the real kheap.c against stub pmm/spinlock/kprintf
# headers (tests/unit/kheapstub/ shadows the kernel ones via -I order) and asserts
# the no-two-live-allocations-overlap invariant -- including across injected
# pmm_alloc_contig failures (the grow() double-accounting bug class).
test-kheap:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address -o $(BUILD)/kheap_test tests/unit/kheap_test.c c/kernel/mm/kheap.c \
	    -Itests/unit/kheapstub -Ic/kernel/mm
	@$(BUILD)/kheap_test

# PNG decoder host test: PIL generates a matrix of cases (colour types, bit depths,
# Adam7, tRNS) as ground truth; our decoder must match byte-for-byte. Needs PIL.
test-png: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/pngtest
	@python3 tests/unit/png_gen.py $(BUILD)/pngtest
	@$(CC) -O2 -o $(BUILD)/png_test tests/unit/png_test.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST) \
	    -Ic/lib/image -Ic/kernel/mm
	@$(BUILD)/png_test $(BUILD)/pngtest

# JPEG baseline decoder host test: PIL encodes baseline JPEGs (grayscale + colour,
# 4:4:4 / 4:2:2 / 4:2:0), and we decode the IDENTICAL bytes with libjpeg djpeg
# (-nosmooth = box chroma upsample, matching ours) as the reference. JPEG is lossy,
# so we compare two decoders of the same bytes within a tight per-channel tolerance,
# never against the original pixels. Also asserts progressive/CMYK fail gracefully.
# Needs PIL + djpeg. (Ad-hoc tests tests/unit/img_test.c, tests/unit/img_fuzz.c have no
# target; if run by hand, add c/lib/image/jpeg.c to their source list.)
test-jpeg: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/jpegtest
	@python3 tests/unit/jpeg_gen.py $(BUILD)/jpegtest
	@$(CC) -O2 -o $(BUILD)/jpeg_test tests/unit/jpeg_test.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST) \
	    -Ic/lib/image -Ic/kernel/mm
	@$(BUILD)/jpeg_test $(BUILD)/jpegtest

clean:
	rm -rf $(BUILD)

# Header-dependency fragments emitted by -MMD (kernel AND app objects). A stale
# object compiled against an old struct layout is memory corruption at runtime.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
