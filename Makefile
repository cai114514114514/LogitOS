# ============================================================================
# LogitOS - build system
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
ISO         := $(BUILD)/logit.iso
DISK        := $(BUILD)/disk.img
FS_FILES    := $(filter-out fsroot/fonts fsroot/as,$(wildcard fsroot/*))
# AetherScript layout: example scripts (source, run directly) vs library modules
# (precompiled to .la). Packed to /usr/as/examples/ and /usr/as/lib/ respectively.
AS_EXAMPLES := $(wildcard fsroot/as/examples/*.as)
AS_LIB_SRCS := $(wildcard fsroot/as/lib/*.as)
AS_LA       := $(patsubst fsroot/as/lib/%.as,$(BUILD)/%.la,$(AS_LIB_SRCS))
FONTS       := fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf
FONT_UI_SRC := third_party/fonts/NotoSansSC-VF.ttf
FONT_MONO_SRC := third_party/fonts/NotoSansMono-VF.ttf
FONT_NOTICES := third_party/fonts/OFL-NotoSansSC.txt \
                third_party/fonts/OFL-NotoSansMono.txt \
                third_party/fonts/README.md
RELEASE_NOTICES := LICENSE LICENSING.md \
                   LICENSES/GPL-3.0-or-later.txt LICENSES/MIT.txt \
                   THIRD_PARTY.md $(FONT_NOTICES)

CC          := clang
LD          := ld.lld
ASM         := nasm
GRUB_RESCUE := i686-elf-grub-mkrescue
QEMU        := qemu-system-x86_64

# Colocated headers resolve via -I across every source dir (names are unique).
INCDIRS := $(addprefix -I,$(shell find c include -type d))
# Host-built unit tests compile kernel sources against the host libc: the
# mini-libc headers (c/apps/libc/include) would shadow glibc's <features.h>
# and break <stdint.h>, so host tests use INCDIRS without that dir.
HOST_INCDIRS := $(filter-out -Ic/apps/libc/include,$(INCDIRS))

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
#
# c/lib/video is excluded because the H.264 decoder is a RING-3 library, not a
# kernel one: it allocates with malloc/free, and the find below was dragging it
# into the kernel link, where those names do not exist (kmalloc/kfree do) --
# which broke `make` outright. It stays out on purpose. An image is decoded
# once, so the image codecs can sit behind SYS_IMG_DECODE; a video is decoded
# thirty times a second, holds megabytes of reference frames, and would run
# under the BKL. See the VID_OBJ rules further down for who does link it.
C_SRC   := $(filter-out c/lib/image/inflate.c c/lib/image/png.c $(wildcard c/lib/video/*.c),$(shell find c/kernel c/drivers c/lib c/fs c/net c/crypto -name '*.c'))
ASM_SRC := $(wildcard c/boot/*.asm)
OBJ     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))

# --- Hybrid C+Rust: a no_std staticlib (rust/) linked with the C objects. Rust
# owns the memory-safety-critical untrusted-input parsers; C owns the core. Use
# the RUSTUP toolchain's cargo/rustc (Homebrew's rust lacks cross targets); the
# x86_64-unknown-none std is `rustup target add x86_64-unknown-none`. ---
RUST_BIN  := $(shell rustup which cargo 2>/dev/null | xargs dirname)
RUST_LIB  := rust/target/x86_64-unknown-none/release/liblogit_rust.a
RUST_SRC  := $(shell find rust/src -name '*.rs') rust/Cargo.toml

.PHONY: all run debug test test-durability test-barrier test-fscrash test-hugefile test-fsreplay test-h264 test-h264-units test-h264-diff test-browser test-nvme test-selfhost test-selfhost-lex test-selfhost-compile test-selfhost-fixpoint clean test-as test-as-gcstress test-as-stress test-as-asan test-as-fast check-asops check-abi test-as-bcstable test-shell test-video test-html5lib test-html5lib-tok test-as-os test-smp test-net test-net-os test-tcp-host test-net-proto test-dhcp-host test-dhcp-os test-https-smoke test-complete test-libc test-fb-clip test-kheap test-png test-jpeg test-svg test-crypto test-crypto-diff test-x509-fuzz

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rust no_std staticlib (kept after `all:` so it never becomes the default goal).
$(RUST_LIB): $(RUST_SRC)
	@if [ -z "$(RUST_BIN)" ]; then \
	    echo "error: rustup/cargo not found (RUST_BIN is empty)."; \
	    echo "       install rustup (https://rustup.rs), then: rustup target add x86_64-unknown-none"; \
	    exit 1; \
	fi
	cd rust && RUSTC="$(RUST_BIN)/rustc" "$(RUST_BIN)/cargo" build --release --target x86_64-unknown-none

# Same crate built for the HOST, for the host-side image tests (test-png/test-jpeg):
# the crate is no_std either way; the tests' own malloc shims satisfy kmalloc/kfree.
RUST_LIB_HOST := rust/target/release/liblogit_rust.a
$(RUST_LIB_HOST): $(RUST_SRC)
	@if [ -z "$(RUST_BIN)" ]; then \
	    echo "error: rustup/cargo not found (RUST_BIN is empty)."; \
	    echo "       install rustup (https://rustup.rs), then: rustup target add x86_64-unknown-none"; \
	    exit 1; \
	fi
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
# GUIDIR = windowed apps (link logit.h + crt0.asm); CLIDIR = shell + coreutils (clib.h + crt0_cli.asm)
GUIDIR := c/apps/gui
CLIDIR := c/apps/coreutils
# the aui widget toolkit (immediate-mode), compiled once + linked into every GUI app
$(BUILD)/apps/aui.o: $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $(GUIDIR)/aui.c -o $@

define APP_RULE
$(BUILD)/$(1).elf: $(GUIDIR)/$(1).c $(APPDIR)/crt0.asm $(APPDIR)/logit.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o
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
# Preview is NOT built by APP_RULE -- it links the H.264 decoder and mini-libc,
# so its rule lives with the VID_OBJ definitions further down.
# Code Studio links the AetherScript completion engine (complete.o) for IntelliSense.
$(BUILD)/apps/complete.o: c/apps/as/complete.c c/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c c/apps/as/complete.c -o $@
$(BUILD)/studio.elf: $(GUIDIR)/studio.c $(APPDIR)/crt0.asm $(APPDIR)/logit.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(BUILD)/apps/complete.o c/apps/as/complete.h
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
ENGINE_SRCS:= $(QJS_SRC) $(wildcard third_party/libm/*.c) \
              $(filter-out c/apps/libc/src/malloc.c,$(wildcard c/apps/libc/src/*.c))
JS_INC     := -Ithird_party/libm -Ithird_party/quickjs    # mini-libc covered by INCDIRS
JS_CF      := $(UCFLAGS) -w -include features.h -DCONFIG_VERSION='"logit-2024"' -DLOGIT_OS -DCONFIG_STACK_CHECK $(JS_INC)
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
                c/apps/browser/css_vars.c c/apps/browser/css_extra.c c/net/http/url.c \
                c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c c/lib/image/img.c
BROWSER_OBJ  := $(patsubst %.c,$(BUILD)/browserobj/%.o,$(BROWSER_PIPE))

# dom.c interns element and attribute names through libwapcaplet (LibCSS's own
# interning library, already linked), so this group needs CSS_INC. Only dom.c
# actually does: dom.h forward-declares lwc_string, so every other consumer of
# the DOM still compiles without the include path.
$(BUILD)/browserobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(CSS_INC) -c $< -o $@

# NetSurf LibCSS (+ libparserutils + libwapcaplet) + our css_engine.c adapter.
CSS_DIR := third_party/css
CSS_INC := -I$(CSS_DIR)/libwapcaplet/include -I$(CSS_DIR)/libparserutils/include \
           -I$(CSS_DIR)/libcss/include -I$(CSS_DIR)/libcss/src -I$(CSS_DIR)/libparserutils/src
CSS_SRC := $(shell find $(CSS_DIR) -name '*.c' ! -name css_property_parser_gen.c)
CSS_OBJ := $(patsubst %.c,$(BUILD)/cssobj/%.o,$(CSS_SRC)) $(BUILD)/cssobj/c/apps/browser/css_engine.o

$(BUILD)/cssobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) -c $< -o $@

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/c/apps/browser/browser.o $(BUILD)/jsobj/c/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/c/apps/browser/browser.o $(BUILD)/jsobj/c/apps/browser/js_dom.o $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

# The shared libc arena (24 MiB, sized as a JS heap) ran dry while LibCSS parsed
# github.com's ~3 MiB of stylesheets: malloc started returning NULL mid-sheet, the
# tail of the CSS (incl. the marketing-header module) was silently dropped and the
# page rendered unstyled. The browser is the only ENGINE_OBJ consumer, so it gets
# its own malloc with a 96 MiB arena; every other app keeps the 24 MiB default.
$(BUILD)/browserobj/malloc_big.o: c/apps/libc/src/malloc.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DARENA_SIZE=100663296u -c $< -o $@

$(BUILD)/browser.aex: $(BUILD)/browser.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser.elf $@ Browser - 'B' 120 130 240

# --- AetherScript: /bin/as -- a ring-3 CLI program. Links the as core + mini-libc
# (fopen/malloc/snprintf/strtod) at the common CLI base via crt0_cli. (CLI_RULE
# can't be reused: those programs use logit.h inline syscalls, not mini-libc.) ---
AS_C    := $(wildcard c/apps/as/*.c)
AS_LIBC := $(wildcard c/apps/libc/src/*.c)
AS_LASM := $(wildcard c/apps/libc/src/*.asm)
AS_OBJ  := $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_C)) \
            $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_LIBC)) \
            $(patsubst %.asm,$(BUILD)/asobj/%.o,$(AS_LASM))
# as.h carries AS_BC_VERSION + the opcode enum; depend on it so a version bump
# rebuilds EVERY asobj (esp. as_bc.o, whose .c rarely changes) -- otherwise a
# stale as_bc.o in /bin/as rejects the freshly-bumped .la files on Logit.
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
# defines memcpy/etc. which clash with the host libc -- so we test on Logit.)
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

# --- H.264 decoder, built for the target ---------------------------------
# c/lib/video is plain C99 over malloc/memset, so the same sources that the
# host gate proves bit-exact against ffmpeg compile straight for x86_64-elf
# against mini-libc. It links into ring-3 consumers, NOT into the kernel: the
# image codecs live behind SYS_IMG_DECODE because a picture is decoded once,
# but a video is decoded thirty times a second and carries megabytes of
# reference frames, and this is exactly the direction M17 moved the browser's
# render pipeline. Keeping a media parser out of ring 0 is the other half.
VID_SRC  := $(wildcard c/lib/video/*.c)
VID_HDRS := $(wildcard c/lib/video/*.h)
VID_OBJ  := $(patsubst %.c,$(BUILD)/vidobj/%.o,$(VID_SRC))

$(BUILD)/vidobj/%.o: %.c $(VID_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# /bin/vidcheck -- decodes a stream on-device and prints the same CRC32 the
# host driver prints, which is what turns "it also works on LogitOS" into a
# comparison rather than a claim. Driven by tests/boot/run-video-test.sh.
$(BUILD)/vidcheck.elf: $(BUILD)/vidobj/c/apps/video/vidcheck.o $(VID_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/vidcheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/vidcheck.crt0c.o \
	    $(BUILD)/vidobj/c/apps/video/vidcheck.o $(VID_OBJ) $(LIBC_OBJS)
$(BUILD)/vidcheck.aex: $(BUILD)/vidcheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/vidcheck.elf $@ vidcheck - 'V' 150 150 150

# Preview: the windowed half. Same link base and the same GUI crt0 as any other
# app, but it links VID_OBJ + mini-libc instead of going through APP_RULE --
# the browser already proves logit.h and mini-libc coexist in one .aex. It
# claims the h264 extension so the Finder opens streams with it; images still
# arrive through the png/gif case in wm.c's launch_for_ext.
$(BUILD)/preview.elf: $(GUIDIR)/preview.c $(APPDIR)/logit.h $(VID_HDRS) $(BUILD)/apps/crt0.o $(VID_OBJ) $(LIBC_OBJS)
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $(GUIDIR)/preview.c -o $(BUILD)/apps/preview.o
	$(LD) -nostdlib -e _start -Ttext=0x48000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(BUILD)/apps/preview.o $(VID_OBJ) $(LIBC_OBJS) --end-group
$(BUILD)/preview.aex: $(BUILD)/preview.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/preview.elf $@ Preview h264 'P' 200 150 110

# Redistributable OFL font subsets are checked in, so a normal build never
# consults host fonts or the network. Regeneration is explicit and reproducible
# from the vendored source fonts. See third_party/fonts/README.md.
.PHONY: verify-font-sources verify-fonts regen-fonts
verify-font-sources:
	@cd third_party/fonts && sha256sum -c SHA256SUMS

verify-fonts: verify-font-sources
	@cd fsroot/fonts && sha256sum -c SHA256SUMS

regen-fonts: verify-font-sources tools/mkfont.py $(FONT_UI_SRC) $(FONT_MONO_SRC)
	@mkdir -p fsroot/fonts
	python3 tools/mkfont.py fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf
	@cd fsroot/fonts && sha256sum -c SHA256SUMS

$(FONTS):
	@echo "missing tracked runtime font '$@'; run 'make regen-fonts'" >&2
	@false

$(DISK): $(FS_FILES) $(AS_EXAMPLES) $(AS_LA) $(FONTS) $(RELEASE_NOTICES) $(AEX) $(BUILD)/libctest.aex $(BUILD)/vidcheck.aex tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    LICENSE:/licenses/README.txt LICENSING.md:/licenses/Logit-LICENSING.md \
	    LICENSES/GPL-3.0-or-later.txt:/licenses/GPL-3.0-or-later.txt \
	    LICENSES/MIT.txt:/licenses/MIT.txt THIRD_PARTY.md:/licenses/THIRD_PARTY.md \
	    third_party/fonts/OFL-NotoSansSC.txt:/licenses/fonts/OFL-NotoSansSC.txt \
	    third_party/fonts/OFL-NotoSansMono.txt:/licenses/fonts/OFL-NotoSansMono.txt \
	    third_party/fonts/README.md:/licenses/fonts/SOURCES.md \
	    $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex) $(BUILD)/browser.aex:browser.aex \
	    $(foreach c,$(CLI),$(BUILD)/$(c).aex:/bin/$(c)) $(BUILD)/as.aex:/bin/as $(BUILD)/libctest.aex:/bin/libctest \
	    $(BUILD)/vidcheck.aex:/bin/vidcheck \
	    tests/fixtures/video/sample.h264:/media/sample.h264 \
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
# The default TCG cpu (qemu64) has no RDRAND/RDSEED, and the kernel TLS client
# refuses to handshake on the weak rdtsc-only RNG fallback -- expose the
# hardware-RNG flags so HTTPS works. Overridable: `make run QEMU_CPU="-cpu qemu64"`.
QEMU_CPU  ?= -cpu max
QEMU_RTC  := -rtc base=localtime    # show the host's local wall-clock time
QEMU_GPU  := -vga none -device virtio-gpu-pci,xres=1280,yres=800   # modern GPU; kernel drives the scanout. xres/yres pin the EDID preferred mode: the driver reads the resolution ONCE at boot, so a small/not-yet-realized QEMU window would otherwise lock the desktop to 640x480 with most windows off-screen.
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot -qmp unix:/tmp/logit-qmp.sock,server,nowait

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: test-crypto test-net $(ISO) $(DISK)
	@sh tests/boot/run-test.sh $(ISO) $(DISK)

# Host-side crypto known-answer tests: 90 vectors for SHA/HMAC/HKDF/AEAD/
# X25519/ECDSA/RSA (tests/unit/crypto_vec_test.c + crypto_vectors.h, generated
# by crypto_vec_gen.py), plus the ecdsa modmul and rsa modexp batteries.
CRYPTO_SRC := $(shell find c/crypto/aead c/crypto/hash c/crypto/pubkey -name '*.c')
test-crypto: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_vec_test tests/unit/crypto_vec_test.c $(CRYPTO_SRC) -Ic/crypto -Itests/unit
	$(BUILD)/crypto_vec_test
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/ecdsa_test tests/unit/ecdsa_test.c c/crypto/pubkey/ecdsa.c -Ic/crypto
	$(BUILD)/ecdsa_test
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/rsa_test tests/unit/rsa_test.c c/crypto/pubkey/rsa.c -Ic/crypto
	$(BUILD)/rsa_test
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/rng_test tests/unit/rng_test.c c/kernel/core/rng.c c/crypto/hash/sha256.c -Ic/crypto -Ic/kernel/core -Itests/unit/rngstub
	$(BUILD)/rng_test

# Randomized differential tests: a self-checked pure-Python reference
# (tests/unit/crypto_diff_gen.py) emits ~127k random vectors; the C asserter
# (tests/unit/crypto_diff_test.c) replays them against the C implementations
# and requires byte-identical output. Long-running; not part of `make test`.
test-crypto-diff: $(BUILD)
	python3 tests/unit/crypto_diff_gen.py $(BUILD)/crypto_diff_vec.txt
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_diff_test tests/unit/crypto_diff_test.c $(CRYPTO_SRC) -Ic/crypto
	$(BUILD)/crypto_diff_test $(BUILD)/crypto_diff_vec.txt

# ASan/UBSan fuzz of the X.509 DER parser (attacker-controlled input on every
# HTTPS handshake) against a real cert. Long-running; not part of `make test`.
test-x509-fuzz: $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -o $(BUILD)/x509_fuzz tests/unit/x509_fuzz.c c/net/tls/x509.c $(CRYPTO_SRC) c/crypto/trust/roots.c -Ic/net/tls -Ic/crypto -Ic/crypto/trust
	$(BUILD)/x509_fuzz tests/unit/cert.der

# Same smoke test, but attach the disk via NVMe -- proves the from-scratch NVMe
# driver brings up a controller and logitfs mounts + reads off it (M24 bare-metal).
test-nvme: $(ISO) $(DISK)
	@BLK=nvme sh tests/boot/run-test.sh $(ISO) $(DISK)

test-shell: $(ISO) $(DISK)
	@sh tests/boot/run-shell-test.sh $(ISO) $(DISK)

# How spec-conformant is the HTML parser? Runs the shared tree-construction
# suite every browser is measured against (third_party/html5lib-tests -- data
# only, the runner is ours) and prints a pass rate.
#
# This is a MEASUREMENT, not a gate: it exits 0 whatever the rate. The parser
# is being rewritten, and a target that is red on every single run for weeks
# only teaches people to stop reading the build. It becomes a gate, with a
# ratchet on the pass count, once there is a rate worth defending.
#
#   make test-html5lib          per-file counts + the total
#   make test-html5lib V=20     also dump the first 20 failing cases
# How spec-conformant is the TOKENIZER? Companion to test-html5lib below, which
# measures tree construction. The tokenizer is the layer where the number can
# honestly be 100%: mechanical, no DOM interaction, exhaustively covered by the
# upstream corpus. The 7032 cases are converted from JSON at build time (650 KB
# of derived C is not worth committing); the entity and tag tables ARE
# committed, following the tools/genroots.py -> roots_bundle.inc convention.
#   make test-html5lib-tok          pass counts
#   make test-html5lib-tok V=20     dump the first 20 failures
$(BUILD)/html5lib_tok_cases.inc: tools/gen_html5lib_tok.py \
                                 $(wildcard third_party/html5lib-tests/tokenizer/*.test)
	@mkdir -p $(BUILD)
	@python3 tools/gen_html5lib_tok.py third_party/html5lib-tests/tokenizer $@

test-html5lib-tok: $(BUILD)/html5lib_tok_cases.inc
	@$(CC) -O2 -w $(BTEST_INC) -I$(BUILD) -o $(BUILD)/html_tok_test \
	    tests/unit/html_tok_test.c c/apps/browser/html_tokenizer.c
	@$(BUILD)/html_tok_test $(if $(V),-v $(V),)

# roots.o carries an explicit dep on roots_bundle.inc because regenerating a
# bundle without one silently keeps the old data in the binary. Same trap here.
$(BUILD)/browserobj/c/apps/browser/html_tokenizer.o: \
    c/apps/browser/html_entities.inc c/apps/browser/html_tags.inc

test-html5lib: $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/html5lib_test tests/unit/html5lib_test.c \
	    c/apps/browser/dom.c $(BUILD)/libcss_host.a
	@$(BUILD)/html5lib_test third_party/html5lib-tests/tree-construction \
	    $(if $(V),-v $(V),)

# Does the H.264 decoder work on LogitOS, not just on the host? make test-h264
# proves it bit-exact against ffmpeg, but that is a glibc build on Linux. This
# boots the OS, runs /bin/vidcheck on the stream packed into the disk image,
# and requires the CRC32 to match the pinned one -- so mini-libc's malloc, the
# 24 MiB arena, boot-time SSE and LogitFS are all in the loop.
test-video: $(ISO) $(DISK)
	@bash tests/boot/run-video-test.sh $(ISO) $(DISK)

# Does the filesystem store data? Every other boot harness passes -snapshot,
# which discards the disk on exit -- deterministic, and it means nothing here has
# ever asserted that a write survives a reboot. This one runs five real boots
# against one image with no -snapshot, verifying three files BYTE FOR BYTE (a
# filesystem that hands one block to two files yields a file of exactly the right
# length holding someone else's data, which a length check cannot see). Slow by
# nature -- five boots -- so it is its own target rather than part of any suite.
test-durability: $(ISO) $(DISK)
	@bash tests/boot/run-durability-test.sh $(ISO) $(DISK)

# A journal orders nothing unless the ordering is asked of the hardware: a disk
# reorders freely inside its own write cache, so "blocks, then commit record"
# only holds if a barrier separates them. Asserts both that the device reports a
# writeback cache (otherwise the test proves nothing and says so) and that a file
# write issues barriers -- counted by the kernel, not inferred from the source.
test-barrier: $(ISO) $(DISK)
	@bash tests/boot/run-barrier-test.sh $(ISO) $(DISK)

# The other half of durability: SIGKILL mid-write, four rounds, and demand the
# journal's contract -- the victim is always whole-or-absent, never torn.
test-fscrash: $(ISO) $(DISK)
	@bash tests/boot/run-fscrash-test.sh $(ISO) $(DISK)

# The double-indirect tree (>1036 blocks, ~4.1 MiB up) is touched by no other
# test. Write a 4.4 MB file, verify it in the same boot and again after a
# clean reboot. Slow: the AS interpreter builds the content char by char.
test-hugefile: $(ISO) $(DISK)
	@bash tests/boot/run-hugefile-test.sh $(ISO) $(DISK)

# log_recover deterministically: craft a sealed-but-not-installed transaction
# (the state a crash between seal and install leaves), boot on it, assert the
# logged block was installed and the header cleared. No kill -9 timing luck.
test-fsreplay: $(ISO) $(DISK)
	@bash tests/boot/run-fsreplay-test.sh $(ISO) $(DISK)

# Host unit test for TCP out-of-order reassembly (white-box: #includes tcp.c).
# Stub headers in tests/unit/tcpstub let tcp.c compile on the host (no x86 asm).
test-tcp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/tcp_test tests/unit/tcp_test.c -Itests/unit/tcpstub -Ic/net/transport
	@./$(BUILD)/tcp_test

# Host protocol tests for IPv4 validation/reassembly, UDP checksums, ICMP
# echo matching and error routing, and the DNS waiter's error path.
test-net-proto:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/net_proto_test tests/unit/net_proto_test.c \
		-Ic/net/core -Ic/net/link -Ic/net/ip -Ic/net/transport -Ic/net/dns \
		-Ic/drivers/timer -Ic/kernel/core
	@./$(BUILD)/net_proto_test

test-net: test-tcp-host test-net-proto test-dhcp-host

test-dhcp-host: $(BUILD)
	$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/dhcp_test tests/unit/dhcp_test.c -Ic/net/core -Ic/net/transport -Ic/drivers/timer -Ic/kernel/core
	$(BUILD)/dhcp_test

# End-to-end e1000 -> IPv4 -> TCP -> HTTP transfer against a host-local server.
test-net-os: $(ISO) $(DISK)
	@bash tests/boot/run-net-test.sh $(ISO) $(DISK)

test-dhcp-os: $(ISO) $(DISK)
	@bash tests/boot/run-dhcp-test.sh $(ISO) $(DISK)

# Live-Internet smoke: needs outbound access from the host; not part of test-net.
test-https-smoke: $(ISO) $(DISK)
	@bash tests/boot/run-https-smoke.sh $(ISO) $(DISK)

# On-Logit AetherScript test: boots and runs /bin/as on the /usr/as examples.
test-as-os: check-asops check-abi $(ISO) $(DISK)
	@sh tests/boot/run-as-test.sh $(ISO) $(DISK)

# mini-libc on-target test battery: boots Logit, runs /bin/libctest, asserts LIBC_OK.
test-libc: $(ISO) $(DISK)
	@sh tests/boot/run-libc-test.sh $(ISO) $(DISK)

# M25 SMP concurrency proof: boots -smp 4, runs /bin/smptest, asserts SMP_TEST_OK
# (no cross-core corruption + genuine parallelism across >=2 cores).
test-smp: $(ISO) $(DISK)
	@sh tests/boot/run-smp-test.sh $(ISO) $(DISK)

# AetherScript host unit test: the language core (lexer/compiler/vm/value/object)
# is portable C, so it builds and runs natively -- no QEMU. Asserts print output
# for arithmetic/control-flow/recursion incl. fib(20).
# Derived from AS_C (the wildcard that builds /bin/as) minus the two files that
# aren't part of the core: as.c owns main(), and complete.c is the self-contained
# completion engine (own -DAS_COMPLETE_TEST target, doesn't include as.h). This
# used to be a hand-written list, so a new core .c built into /bin/as fine and
# then failed to link every host test until someone remembered to add it here.
AS_CORE := $(filter-out c/apps/as/as.c c/apps/as/complete.c,$(AS_C))
test-as: check-asops check-abi
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/as_test tests/unit/as_test.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_test

# The opcode/token/builtin numbers are hand-copied into three implementations
# (as.h -> asc.as, lexer.h -> aslex.as, vm.c -> complete.c). A drift in the first
# two is a SILENT miscompile: the self-hosted compiler emits an instruction the C
# VM decodes as a different one -- and NOTHING else catches it. Verified: setting
# OP_RET to 99 in asc.as still leaves test-as and test-as-gcstress at 254/254
# green. So every as-facing target depends on this; being a phony prerequisite is
# why it stays off file targets like $(ASC) (that would force a rebuild each run).
# Read-only: it never rewrites asc.as/aslex.as.
check-asops:
	@python3 tools/gen_as_opcodes.py --check

# The kernel struct layouts AetherScript reads (fsroot/as/lib/abi.as) are
# generated from include/abi/logit_abi.h, and every offset in them is ALSO
# emitted as a _Static_assert that as_native.c compiles -- so a struct the
# kernel reorders fails the build rather than leaving a script reading the wrong
# bytes. That leaves one gap the asserts cannot see: a field RENAMED at the same
# offset. This closes it by regenerating and diffing. Read-only, like
# check-asops: it never rewrites the generated files (use --write for that).
check-abi:
	@python3 tools/gen_abi.py --check

# as_native.c #includes the generated asserts; rebuild it when they change, or a
# stale object would keep vouching for the old layout (cf. the roots_bundle.inc
# gotcha, where a missing dep silently kept the old CA roots in the kernel).
$(BUILD)/asobj/c/apps/as/as_native.o: c/apps/as/abi_layout.inc
$(BUILD)/c/apps/as/as_native.o: c/apps/as/abi_layout.inc

# libcomplete host unit tests: the completion engine is self-contained C, so it
# builds and runs natively -- no QEMU.
test-complete:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_COMPLETE_TEST -o $(BUILD)/complete_test tests/unit/complete_test.c c/apps/as/complete.c -Ic/apps/as
	@$(BUILD)/complete_test

# Framebuffer clip is per-target (struct surface), not global: this builds the
# real c/kernel/gui/fb.c host-side and asserts a clip set on one app's surface
# does NOT bleed into a draw on another's (the "white Terminal" cross-app leak).
test-fb-clip:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/fb_clip_test tests/unit/fb_clip_test.c c/kernel/gui/fb.c $(HOST_INCDIRS)
	@$(BUILD)/fb_clip_test

# GC stress: collect before EVERY allocation -> any missing GC root becomes a crash
# or wrong output. Runs the same host unit suite under -DAS_GC_STRESS.
test-as-gcstress: check-asops check-abi
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_GC_STRESS -o $(BUILD)/as_test_gcstress tests/unit/as_test.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_test_gcstress

# Robustness suite: deep recursion, huge allocations, many locals, boundary
# values -- the paths that a runtime rewrite breaks first. Uses only the public
# API (as_interpret/as_capture/as_gc_live), so it survives representation changes.
test-as-stress: check-asops check-abi
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/as_stress tests/unit/as_stress.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_stress

# Host `asc`: the as core + the as.c entry built natively (no --target -> arm64
# host binary), used at `make` time to precompile the stdlib .as to .la. `-c`
# mode never invokes the syscall path, so the arm64 as_ll.c stub is fine. Host
# and target share AS_CORE/as.h, so AS_BC_VERSION + the opcode enum match and a
# host-produced .la loads on Logit.
ASC := $(BUILD)/asc
# as.h carries AS_BC_VERSION + the opcode enum; list it so a version bump or
# opcode change forces asc (and therefore every .la) to rebuild. Without this
# dep a bumped AS_BC_VERSION silently keeps stale .la files that the kernel's
# as_load then rejects (cf. the roots_bundle.inc dep gotcha).
$(ASC): $(AS_CORE) c/apps/as/as.c c/apps/as/as.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -o $@ c/apps/as/as.c $(AS_CORE) -Ic/apps/as -Iinclude/abi

# Precompile the LibLogit library modules (fsroot/as/lib/*.as) to .la (compiled
# bytecode). -c is compile-only (no run), so even a lib with module-mate calls
# (mathx) is fine; packed to /usr/as/lib/.
$(BUILD)/%.la: fsroot/as/lib/%.as $(ASC)
	$(ASC) -c $< -o $@

# M21-P3 self-hosting S1: the AetherScript lexer (lib/aslex.as) must emit a
# token stream byte-identical to the C lexer over the whole in-tree corpus.
test-selfhost-lex: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-selfhost-lex.sh $(BUILD)/asc

# S2/S3: programs compiled by the self-hosted compiler (lib/asc.as) run identically.
test-selfhost-compile: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-selfhost-compile.sh $(BUILD)/asc

# S4: the self-hosting fixpoint -- the compiler compiled by itself reproduces itself.
test-selfhost-fixpoint: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-selfhost-fixpoint.sh $(BUILD)/asc

test-selfhost: test-selfhost-lex test-selfhost-compile test-selfhost-fixpoint

# Bytecode stability: the runtime-rewrite milestone changes how values, objects,
# the GC and lookups are represented -- but NOT what the compiler emits. Hashes
# every compiled stdlib module against a checked-in baseline, so a slice that
# accidentally perturbs codegen is caught at the module level (and long before
# the fixpoint test would notice a 37 KB binary moved).
test-as-bcstable: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-bcstable.sh $(BUILD)/asc

# The runtime rewrite replaces the allocator and object headers: a chunk overrun
# corrupts a DIFFERENT object, so the crash lands far from the cause. The target
# can't run a sanitizer (freestanding, no runtime), but the host can -- use it.
# Slow (gcstress x asan), so it is not part of test-as-fast.
test-as-asan: check-asops check-abi
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/as_test_asan tests/unit/as_test.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_test_asan
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DAS_GC_STRESS \
	    -o $(BUILD)/as_stress_asan tests/unit/as_stress.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_stress_asan

# The gate every runtime slice must pass before it is committed: unit + GC stress
# + robustness + completion + the three self-hosting stages + the bytecode
# baseline. All host, ~1 minute. `test-as-os` (QEMU) is the separate slow gate --
# a host-green slice can still break on the 24 MiB static arena.
test-as-fast: test-as test-as-gcstress test-as-stress test-complete \
              test-selfhost-lex test-selfhost-compile test-selfhost-fixpoint test-as-bcstable

# kheap host test: compiles the real kheap.c against stub pmm/spinlock/kprintf
# headers (tests/unit/kheapstub/ shadows the kernel ones via -I order) and asserts
# the no-two-live-allocations-overlap invariant -- including across injected
# pmm_alloc_contig failures (the grow() double-accounting bug class).
test-kheap:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address -o $(BUILD)/kheap_test tests/unit/kheap_test.c c/kernel/mm/kheap.c \
	    -Itests/unit/kheapstub -Ic/kernel/mm
	@$(BUILD)/kheap_test

# --- test-browser: host unit tests for the ring-3 browser render pipeline ---
# The tests self-stub kmalloc/kfree/img_* so they link the real pipeline
# sources (dom/css_engine/css_vars/layout/js_dom) on the host. LibCSS is
# archived once per build tree (libcss_host.a) and shared by the CSS tests.
BTEST_INC := -Ic/apps/browser -Ic/lib/image -Ic/net/http -Ic/lib/text
CSSHOST_OBJ := $(patsubst %.c,$(BUILD)/csshost/%.o,$(CSS_SRC))

$(BUILD)/csshost/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) -c $< -o $@

$(BUILD)/libcss_host.a: $(CSSHOST_OBJ)
	@ar rcs $@ $^

test-browser: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/dom_test tests/unit/dom_test.c c/apps/browser/dom.c $(BUILD)/libcss_host.a
	@$(BUILD)/dom_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/dom_api_test tests/unit/dom_api_test.c c/apps/browser/dom.c $(BUILD)/libcss_host.a
	@$(BUILD)/dom_api_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/var_test tests/unit/var_test.c c/apps/browser/css_vars.c
	@$(BUILD)/var_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/parse_fuzz tests/unit/parse_fuzz.c c/net/http/url.c c/lib/text/utf8.c
	@$(BUILD)/parse_fuzz
	@$(CC) -O2 -w $(HOST_INCDIRS) -o $(BUILD)/http_dechunk_test tests/unit/http_dechunk_test.c c/net/http/url.c
	@$(BUILD)/http_dechunk_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_engine_test tests/unit/css_engine_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/dom.c $(BUILD)/libcss_host.a
	@$(BUILD)/css_engine_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_extra_test tests/unit/css_extra_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c c/apps/browser/dom.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/css_extra_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_test tests/unit/layout_test.c \
	    c/apps/browser/layout.c c/apps/browser/dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/layout_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/page_test tests/unit/page_test.c \
	    c/apps/browser/layout.c c/apps/browser/dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/page_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/table_list_test tests/unit/table_list_test.c \
	    c/apps/browser/layout.c c/apps/browser/dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/table_list_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/pipeline_stress tests/unit/pipeline_stress.c \
	    c/apps/browser/layout.c c/apps/browser/dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/pipeline_stress
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_svg_test tests/unit/layout_svg_test.c \
	    c/apps/browser/layout.c c/apps/browser/dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c tests/unit/rust_host_shim.c \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -Ic/kernel/mm
	@$(BUILD)/layout_svg_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_dom_test \
	    tests/unit/js_dom_test.c c/apps/browser/js_dom.c c/apps/browser/dom.c $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/js_dom_test
	@echo "test-browser: ALL PASS"

# PNG decoder host test: PIL generates a matrix of cases (colour types, bit depths,
# Adam7, tRNS) as ground truth; our decoder must match byte-for-byte. Needs PIL.
test-png: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/pngtest
	@python3 tests/unit/png_gen.py $(BUILD)/pngtest
	@$(CC) -O2 -o $(BUILD)/png_test tests/unit/png_test.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST) \
	    -Ic/lib/image -Ic/kernel/mm
	@$(BUILD)/png_test $(BUILD)/pngtest

# JPEG baseline decoder host test: PIL encodes baseline JPEGs (grayscale + colour,
# 4:4:4 / 4:2:2 / 4:2:0), and we decode the IDENTICAL bytes with libjpeg djpeg
# (-nosmooth = box chroma upsample, matching ours) as the reference. JPEG is lossy,
# so we compare two decoders of the same bytes within a tight per-channel tolerance,
# never against the original pixels. Also asserts progressive/CMYK fail gracefully.
# Needs PIL + djpeg. (Ad-hoc tests tests/unit/img_test.c, tests/unit/img_fuzz.c have no
# target; if run by hand, add c/lib/image/jpeg.c + c/lib/image/svg.c to their source list.)
test-jpeg: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/jpegtest
	@python3 tests/unit/jpeg_gen.py $(BUILD)/jpegtest
	@$(CC) -O2 -o $(BUILD)/jpeg_test tests/unit/jpeg_test.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST) \
	    -Ic/lib/image -Ic/kernel/mm
	@$(BUILD)/jpeg_test $(BUILD)/jpegtest

# SVG rasterizer host test: embedded cases (real GitHub octicon mark path,
# rect/circle/ellipse, g fill inheritance, fill-rule evenodd, opacity, xml
# sniffing) plus truncation/garbage robustness checks. No asset generation.
test-svg: $(RUST_LIB_HOST)
	@$(CC) -O2 -o $(BUILD)/svg_test tests/unit/svg_test.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST) \
	    -Ic/lib/image -Ic/kernel/mm
	@$(BUILD)/svg_test

# H.264 baseline decoder host test: tools/genvideo.sh generates the stream
# matrix with ffmpeg/libx264 and the reference YUV with ffmpeg's own decoder.
# H.264 reconstruction is exactly specified integer arithmetic, so a correct
# decoder matches ffmpeg byte-for-byte -- any mismatch is our bug, reported
# with frame/plane/pixel. Needs ffmpeg. (genvideo.sh is idempotent; rerun it
# by hand to refresh the matrix.)
H264_SRC := c/lib/video/h264.c c/lib/video/h264_nal.c c/lib/video/h264_cavlc.c \
            c/lib/video/h264_pred.c c/lib/video/h264_mc.c c/lib/video/h264_deblock.c
test-h264:
	@mkdir -p $(BUILD)/h264ref
	@bash tools/genvideo.sh $(BUILD)/h264ref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_test tests/unit/h264test.c $(H264_SRC) -Ic/lib/video
	@for f in $(BUILD)/h264ref/*.h264; do \
	    $(BUILD)/h264_test $$f $${f%.h264}.ref.yuv || exit 1; \
	done
	@echo "all H.264 host cases bit-exact"
	@# The committed fixture, checked against a CRC pinned in the tree. The
	@# generated matrix above re-encodes with whatever x264 is installed, so it
	@# is not a fixed target; this one is, and it is also the only stream we
	@# have that quantises finely enough to reach a chroma qP below 6. The same
	@# CRC is what the on-device check prints, which is how a decode inside
	@# LogitOS gets compared with a decode on the host.
	@crc=`$(BUILD)/h264_test tests/fixtures/video/sample.h264 | awk '{print $$2}'`; \
	 want=`cat tests/fixtures/video/sample.crc32`; \
	 if [ "$$crc" != "$$want" ]; then \
	     echo "H264-FIXTURE-FAIL crc $$crc want $$want"; exit 1; fi; \
	 echo "H264-OK fixture crc $$crc (tests/fixtures/video/sample.h264)"

# Per-case byte counts over the WHOLE stream instead of stopping at the first
# bad pixel. "the first mismatch moved" says nothing about whether a change
# helped; a total does, and it is what makes bisecting the decoder possible.
test-h264-diff:
	@mkdir -p $(BUILD)/h264ref
	@bash tools/genvideo.sh $(BUILD)/h264ref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_diff tests/unit/h264_diff.c $(H264_SRC) -Ic/lib/video
	@rc=0; for f in $(BUILD)/h264ref/*.h264; do \
	    printf '%-24s ' "$$(basename $$f .h264)"; \
	    $(BUILD)/h264_diff $$f $${f%.h264}.ref.yuv | tail -1 || rc=1; \
	done; exit $$rc

# The module unit tests. These existed but were wired to nothing, so they had
# never run -- and two of their expectations disagreed with the spec (intra 4x4
# vertical-left indexed p[x+y] instead of p[x+(y>>1)]; chroma DC averaged both
# edges in all four quadrants). Both were corrected against 8.3.1.2.8 / 8.3.4.1
# and then confirmed the honest way: with the decoder fixed to match, a real
# stream decodes byte-identically to ffmpeg.
#
# h264_cavlc_test is NOT here on purpose. Its roundtrip section encodes with a
# CAVLC *encoder* written inside the test, and that encoder disagrees with the
# decoder about level coding. The decoder is the one that is right: it decodes
# i-only-160x120 bit-exactly for all 60 frames, which exercises coeff_token,
# level escapes, total_zeros and run_before across thousands of blocks. Fixing
# the test's encoder is its own task; wiring a known-wrong test into a gate
# would only teach people to ignore the gate.
test-h264-units:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_pred_test tests/unit/h264_pred_test.c \
	    c/lib/video/h264_pred.c -Ic/lib/video
	@$(BUILD)/h264_pred_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_mc_test tests/unit/h264_mc_test.c \
	    c/lib/video/h264_mc.c -Ic/lib/video
	@$(BUILD)/h264_mc_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_deblock_test tests/unit/h264_deblock_test.c \
	    c/lib/video/h264_deblock.c -Ic/lib/video
	@$(BUILD)/h264_deblock_test

clean:
	rm -rf $(BUILD)

# Header-dependency fragments emitted by -MMD (kernel AND app objects). A stale
# object compiled against an old struct layout is memory corruption at runtime.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
