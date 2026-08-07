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
# -fno-omit-frame-pointer: the kernel's stack BACKTRACE depends on it and there
# is no other way to get one here. At -O2 clang treats rbp as a general-purpose
# register, so the "frame chain" a panic would walk is whatever integers the
# code left on the stack -- and a backtrace that silently prints plausible
# garbage is worse than printing none. DWARF unwinding is not an option either:
# linker.ld /DISCARD/s .eh_frame, and a ring-0 unwinder that parses CFI is a
# large amount of code to run in the one situation where the machine is already
# broken. The cost is one register on x86-64 and a percent or so of code size.
# Turn it off to see the difference: `make FPO=1` (see the knobs below) -- that
# is the negative control for tests/boot/run-panic-test.sh.
CFLAGS  := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie -fno-omit-frame-pointer \
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
#   make FPO=1     drop frame pointers -- the NEGATIVE CONTROL for the panic
#                  backtrace. Build with this and run-panic-test.sh's frame
#                  assertions fail, which is how "the backtrace is real" is
#                  demonstrated rather than asserted.
ifeq ($(FPO),1)
CFLAGS += -fomit-frame-pointer
endif
#   make KLOGUNSAFE=1  build klog WITHOUT its interrupt guard and per-CPU line
#                  buffers, i.e. the naive logger. The negative control for the
#                  interrupt-context claim: `echo irqstorm > /dev/ktrigger`
#                  then reports torn records instead of torn=0.
ifeq ($(KLOGUNSAFE),1)
CFLAGS += -DKLOG_UNSAFE
endif
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -T linker.ld

# Userland (ring 3) build flags.
#
# -DNDEBUG: mini-libc's <assert.h> is now the conformant one -- it honours
# NDEBUG instead of deleting every assertion unconditionally, which is not a
# header's decision to make about programs it did not write. Release userland is
# where "no assertions" belongs, and defining it here keeps every shipped .aex
# behaving exactly as it did before that header changed (notably the ~320
# asserts in third_party/{quickjs,css}). Drop it from one rule to build that
# component with its assertions live.
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 -DNDEBUG \
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
#
#
# RING3_NET is excluded for exactly the same reason as c/lib/video: it is the
# ring-3 HTTP client (connection pool, cookie jar, HTTP/1.1 framing) that runs on
# top of the non-blocking socket syscalls, and it allocates with malloc/free and
# uses strchr/strcmp/memchr from mini-libc -- none of which exist in the kernel.
# It shares c/net/http with the kernel's own blocking client (http.c/url.c), so
# it cannot be excluded by directory; the find below was dragging it into the
# kernel link, where it failed with six undefined symbols. Its consumers link it
# with LIBC_OBJS, like the video decoder does.
RING3_NET := c/net/http/cookies.c c/net/http/http1.c c/net/http/hpool.c
C_SRC   := $(filter-out c/lib/image/inflate.c c/lib/image/png.c $(wildcard c/lib/video/*.c) $(RING3_NET),$(shell find c/kernel c/drivers c/lib c/fs c/net c/crypto -name '*.c'))
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

.PHONY: test-webapi test-webapi-asan test-webapi-page test-webapi-page-control test-fetch-ui all run shot debug test test-durability test-barrier test-fscrash test-hugefile test-fsreplay test-h264 test-h264-units test-h264-diff test-browser test-css-asan test-css-fidelity test-nvme test-part test-part-asan test-ahci test-ahci-raw test-ahci-mbr test-ahci-gpt test-ahci-two test-selfhost test-selfhost-lex test-selfhost-compile test-selfhost-fixpoint clean test-as test-as-gcstress test-as-stress test-as-asan test-as-fast check-asops check-abi test-as-bcstable test-shell test-video test-evq test-clock test-input test-html5lib test-html5lib-tok test-html5lib-asan test-js-dom-asan test-live-page test-as-os test-smp test-net test-net-os test-sock test-sock-ui test-tcp-host test-tcp-negctl test-net-proto test-dhcp-host test-dhcp-os test-https-smoke test-complete test-libc test-fb-clip test-kheap test-malloc test-png test-jpeg test-svg test-crypto test-crypto-diff test-libc-diff test-x509-fuzz test-http-fuzz check-ring3-net test-modules test-handshakes test-time-host test-time-negctl test-time test-time-smp test-klog test-klog-control test-panic test-panic-log test-stream test-stream-control test-stream-asan test-cookie-cors test-cookie-cors-asan test-sse-page test-sse-page-control test-stream test-stream-control test-stream-asan test-cookie-cors test-cookie-cors-asan test-sse-page test-sse-page-control

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

# -Map: the linker map is what a backtrace's hex addresses are READ with. It is
# free to emit, it changes nothing in the binary, and without it the panic
# output on an unfamiliar machine is a column of numbers nobody can resolve.
# tests/boot/run-panic-test.sh checks a real frame against it.
$(KERNEL): $(OBJ) $(RUST_LIB) linker.ld
	$(LD) $(LDFLAGS) -Map=$(BUILD)/kernel.map -o $@ --start-group $(OBJ) $(RUST_LIB) --end-group

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

CLI := sh echo ls cat pwd wc head true false sleep mkdir rm touch clear uname net cp mv smptest socktest
$(foreach c,$(CLI),$(eval $(call CLI_RULE,$(c))))
CLI_AEX := $(foreach c,$(CLI),$(BUILD)/$(c).aex)

AEX  := $(foreach a,$(APPS),$(BUILD)/$(a).aex) $(BUILD)/browser.aex $(CLI_AEX) $(BUILD)/as.aex
# Which browser goes on the disk. Overridable so a test can pack a deliberately
# crippled build instead -- see test-webapi-page-control, which is how "this
# assertion fails without the change" is demonstrated rather than asserted.
BROWSER_AEX ?= $(BUILD)/browser.aex

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
BROWSER_PIPE := c/apps/browser/dom.c c/apps/browser/html_tokenizer.c \
                c/apps/browser/html_tree.c c/apps/browser/dom_serialize.c \
                c/apps/browser/layout.c \
                c/apps/browser/browser_rt.c c/apps/browser/browser_paint.c \
                c/apps/browser/css_vars.c c/apps/browser/css_extra.c c/net/http/url.c \
                c/net/http/http1.c c/net/http/hpool.c c/net/http/cookies.c \
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

# The app's own TUs that touch QuickJS headers, so they build with JS_CF (and
# not with the plain browser flags, which lack -Ithird_party/quickjs).
# Globbed, not listed: the JS/DOM side is being extended by several parallel
# lines at once (module loader, DOM bindings, web APIs) and a hand-kept list
# makes this one line the thing they all have to edit.
BROWSER_JS_SRC := c/apps/browser/browser.c $(sort $(wildcard c/apps/browser/js_*.c))
BROWSER_JS_OBJ := $(patsubst %.c,$(BUILD)/jsobj/%.o,$(BROWSER_JS_SRC))

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

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
$(BUILD)/asobj/tests/unit/libctest_main.o: tests/unit/libctest_main.c tests/unit/libctest_more.h
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

# --- ring-3 HTTP client, built for the target -----------------------------
# Same shape and same reasoning as VID_OBJ above: RING3_NET is filtered out of
# the kernel's C_SRC (see the comment at the top) and compiled here with the
# userland flags instead, against mini-libc. Consumers link R3NET_OBJ the way
# Preview and /bin/vidcheck link VID_OBJ. Nothing links it yet -- browser.c is
# wired up in a later slice -- so `check-ring3-net` exists to keep it building
# with the REAL freestanding flags rather than only under the host compiler,
# which is where a stray <stdio.h> or a long-long division helper would hide.
R3NET_SRC  := $(RING3_NET)
R3NET_HDRS := c/net/http/http1.h c/net/http/cookies.h c/net/http/hpool.h
R3NET_OBJ  := $(patsubst %.c,$(BUILD)/r3netobj/%.o,$(R3NET_SRC))

$(BUILD)/r3netobj/%.o: %.c $(R3NET_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

check-ring3-net: $(R3NET_OBJ)
	@echo "check-ring3-net: http1/cookies/hpool build freestanding"

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
	    $(foreach a,$(APPS),$(BUILD)/$(a).aex:$(a).aex) $(BROWSER_AEX):browser.aex \
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
# Modern GPU; the kernel drives the scanout. xres/yres set the EDID preferred
# mode, which the driver reads once at boot. This used to be a CAGE: without a
# scale factor the desktop's geometry was measured in raw device pixels, so
# 1280x800 was simultaneously the resolution AND the layout, and any other number
# either shrank every control or pushed windows off-screen. Now it is only a
# DEFAULT. The kernel treats app geometry as points and picks a backing scale
# from the mode (fb.c pick_scale), holding the logical desktop at >= 1280x800 and
# spending the surplus pixels on density -- so 1920x1200 is the same desk space
# at 1.5x, drawn with 2.25x the pixels, and text/icons are re-rasterized rather
# than magnified. Override freely; the UI follows:
#   make run QEMU_GPU="-vga none -device virtio-gpu-pci,xres=2560,yres=1600"   # 2x
#   make run QEMU_GPU="-vga none -device virtio-gpu-pci,xres=1280,yres=800"    # 1x
# (A not-yet-realized QEMU window still reports 640x480; virtio_gpu.c refuses
# that and programs the default rather than locking the desktop to it.)
QEMU_GPU  := -vga none -device virtio-gpu-pci,xres=1920,yres=1200
QEMU_NET  := -netdev user,id=n0 -device e1000,netdev=n0 \
             -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

# Display backend. QEMU picks gtk by default on Linux, which is the right choice
# on a normal desktop -- but under WSLg a window can appear in the taskbar and
# then never paint, with QEMU reporting no error at all.
#
# Diagnose that with `make shot` FIRST: it screendumps over QMP with no host
# window in the path, so a correct image proves the guest is fine and the fault
# is host-side. Seen once and worth writing down, because no amount of changing
# DISP fixes it -- the title bar read "WARN(copy mode)" and /mnt/wslg/weston.log
# said:
#     RDP backend: enable_copy_warning_title = 1
#     rdp_allocate_shared_memory: Failed to open "/mnt/shared_memory/{...}":
#         Input/output error
# WSLg hands window surfaces to Windows through that shared-memory channel; when
# it fails it falls back to copying every frame over RDP, and a 1280x800 guest
# display does not survive the fallback. /dev/dxg and libd3d12 were both present
# -- the GPU was fine, the channel was not. The fix is on the Windows side
# (`wsl --shutdown`, then `wsl --update`), not in this repo, and every -display
# backend fails identically because they all sit above that channel.
#
# For the ordinary case where a different backend does help:
#     make run DISP=gtk,gl=off     GTK without OpenGL (WSLg's GL is virtualised)
#     make run DISP=sdl            a different toolkit altogether
#     make run DISP=none           no window; drive it with tests/qmp/*.py
DISP ?=
QEMU_DISP := $(if $(DISP),-display $(DISP),)

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) $(QEMU_DISP) -serial stdio -no-reboot -qmp unix:/tmp/logit-qmp.sock,server,nowait

# What is the guest ACTUALLY drawing? Boots headless, screendumps over QMP and
# writes a PNG. This is the check that separates "the OS is broken" from "the
# window is not painting" -- it reads the scanout the guest produced, with no
# host window involved.
shot: $(ISO) $(DISK)
	@bash tools/shot.sh

debug: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) $(QEMU_DISK) $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(QEMU_NET) -serial stdio -no-reboot -s -S

test: test-crypto test-net $(ISO) $(DISK)
	@sh tests/boot/run-test.sh $(ISO) $(DISK)

# Host-side crypto known-answer tests: 90 vectors for SHA/HMAC/HKDF/AEAD/
# X25519/ECDSA/RSA (tests/unit/crypto_vec_test.c + crypto_vectors.h, generated
# by crypto_vec_gen.py), plus the ecdsa modmul and rsa modexp batteries.
# c/kernel/cpu/cpufeat.c rides along: the AES-GCM backend dispatch asks it
# whether this CPU has AES-NI + PCLMULQDQ, and it is deliberately free of
# kernel dependencies so it can be linked here. CRYPTO_INC is what every
# host-side crypto build needs on its include path.
CRYPTO_SRC := $(shell find c/crypto/aead c/crypto/hash c/crypto/pubkey -name '*.c') \
              c/kernel/cpu/cpufeat.c
CRYPTO_INC := -Ic/crypto -Ic/crypto/aead -Ic/kernel/cpu
test-crypto: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_vec_test tests/unit/crypto_vec_test.c $(CRYPTO_SRC) $(CRYPTO_INC) -Itests/unit
	$(BUILD)/crypto_vec_test
	@$(MAKE) --no-print-directory test-cpufeat
	@$(MAKE) --no-print-directory test-aes-ni
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/ecdsa_test tests/unit/ecdsa_test.c c/crypto/pubkey/ecdsa.c -Ic/crypto
	$(BUILD)/ecdsa_test
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/rsa_test tests/unit/rsa_test.c c/crypto/pubkey/rsa.c -Ic/crypto
	$(BUILD)/rsa_test
	@# rngstub MUST come first on the include path: rng.c now includes
	@# cpufeat.h from c/kernel/cpu, and that directory also holds the REAL
	@# spinlock.h/kprintf.h. Listed after, they shadow the stubs and the link
	@# fails on spin_lock_irqsave.
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/rng_test tests/unit/rng_test.c c/kernel/core/rng.c c/crypto/hash/sha256.c c/kernel/cpu/cpufeat.c -Itests/unit/rngstub -Ic/crypto -Ic/kernel/core -Ic/kernel/cpu
	$(BUILD)/rng_test
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/ecdh_test tests/unit/ecdh_test.c c/crypto/pubkey/ecdsa.c -Ic/crypto
	$(BUILD)/ecdh_test
	@$(MAKE) --no-print-directory check-roots

# The trust store must not lie about its own size. genroots.py silently drops
# any root whose key type the kernel cannot verify (P-521, Ed25519); until this
# target existed, that produced a bundle smaller than tools/roots/ with nothing
# in the build output to say so. This regenerates into a scratch file, prints
# the skip list, and fails if the committed bundle is stale -- which also
# catches the classic "edited tools/roots/ and forgot to regenerate".
check-roots: $(BUILD)
	@python3 tools/genroots.py tools/roots $(BUILD)/roots_bundle.check 2>$(BUILD)/roots.log; \
	 grep -c 'ROOT_RSA\|ROOT_EC' $(BUILD)/roots_bundle.check | xargs -I{} echo "roots: {} compiled in"; \
	 if grep -q '^!!' $(BUILD)/roots.log; then grep '^!!' $(BUILD)/roots.log; fi; \
	 if ! cmp -s $(BUILD)/roots_bundle.check c/crypto/trust/roots_bundle.inc; then \
	   echo "FAIL: c/crypto/trust/roots_bundle.inc is stale -- re-run tools/genroots.py"; exit 1; \
	 fi

# TLS 1.3 interop against a real `openssl s_server`: HelloRetryRequest, each
# key-exchange group, both AEADs, EC and RSA leaves, ALPN, and the rejections.
# Not in `make test` because it spawns servers and takes ~2 min under ASan; it
# is the test that proves the reachability claims, so run it on any TLS change.
test-tls-interop: $(BUILD)
	@bash tests/unit/run-tls-interop.sh

# Randomized differential tests: a self-checked pure-Python reference
# (tests/unit/crypto_diff_gen.py) emits ~127k random vectors; the C asserter
# (tests/unit/crypto_diff_test.c) replays them against the C implementations
# and requires byte-identical output. Long-running; not part of `make test`.
# Every AES-GCM vector now runs through BOTH backends (accelerated + portable)
# and they must agree with the reference AND with each other -- see run_aead().
test-crypto-diff: $(BUILD)
	python3 tests/unit/crypto_diff_gen.py $(BUILD)/crypto_diff_vec.txt
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_diff_test tests/unit/crypto_diff_test.c $(CRYPTO_SRC) $(CRYPTO_INC)
	$(BUILD)/crypto_diff_test $(BUILD)/crypto_diff_vec.txt

# --- CPUID decode + SIMD dispatch, host-side -----------------------------
# Declared in their own .PHONY line rather than appended to the big one at the
# top: several workstreams edit that line, and a separate declaration means no
# merge conflict for a target list that make is happy to see twice.
.PHONY: test-cpufeat test-aes-ni test-aes-ni-control

# test-cpufeat cross-checks c/kernel/cpu/cpufeat.c against /proc/cpuinfo (an
# independent decode of the same bits) in both directions, so a mis-numbered
# feature bit fails here rather than at the point where a dispatch picks an
# implementation the CPU cannot run.
test-cpufeat: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/cpufeat_test tests/unit/cpufeat_test.c \
	    c/kernel/cpu/cpufeat.c -Ic/kernel/cpu
	$(BUILD)/cpufeat_test

# test-aes-ni proves the three things the accelerated crypto path needs: the
# AES-NI and portable backends produce identical bytes on 20k primitive and 4k
# full-AEAD random cases, published vectors pass under EACH backend, and the
# dispatch really selected the accelerated one (rather than quietly testing the
# C path twice). No timing is measured -- see the file header for why a TCG or
# same-process number would not mean anything.
test-aes-ni: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/aes_ni_test tests/unit/aes_ni_test.c \
	    $(CRYPTO_SRC) $(CRYPTO_INC)
	$(BUILD)/aes_ni_test

# Negative control for test-aes-ni, run by hand rather than in CI because it is
# meant to FAIL: it forces the dispatch to pick the portable backend on a CPU
# that has AES-NI, so the "the dispatch actually dispatches" assertion must
# fire. If this passes, that assertion is vacuous and the suite proves nothing.
test-aes-ni-control: $(BUILD)
	$(CC) -O2 -Wall -Wextra -DAESNI_CONTROL_NO_ACCEL -o $(BUILD)/aes_ni_control \
	    tests/unit/aes_ni_test.c $(CRYPTO_SRC) $(CRYPTO_INC)
	@if $(BUILD)/aes_ni_control > $(BUILD)/aes_ni_control.log 2>&1; then \
	    echo "CONTROL FAILED: the crippled build passed -- the dispatch assertions are vacuous"; \
	    exit 1; \
	else \
	    echo "control ok: crippled build fails as intended:"; \
	    grep '^FAIL' $(BUILD)/aes_ni_control.log | head -5; \
	fi

# ASan/UBSan fuzz of the X.509 DER parser (attacker-controlled input on every
# HTTPS handshake) against a real cert. Long-running; not part of `make test`.
test-x509-fuzz: $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -o $(BUILD)/x509_fuzz tests/unit/x509_fuzz.c c/net/tls/x509.c $(CRYPTO_SRC) c/crypto/trust/roots.c -Ic/net/tls $(CRYPTO_INC) -Ic/crypto/trust
	$(BUILD)/x509_fuzz tests/unit/cert.der

# Same smoke test, but attach the disk via NVMe -- proves the from-scratch NVMe
# driver brings up a controller and logitfs mounts + reads off it (M24 bare-metal).
test-nvme: $(ISO) $(DISK)
	@BLK=nvme sh tests/boot/run-test.sh $(ISO) $(DISK)

# Partition-table parsing (MBR incl. the extended chain, GPT incl. both CRC32s),
# on the host against synthetic sector images. This is where nearly all the risk
# in the storage widening lives: every field comes off a disk somebody else
# formatted, and none of it needs a controller or a boot to exercise. The cases
# that matter are the malformed ones -- a corrupted entry-array CRC, an extended
# chain that points at itself, an entry that runs off the end of the device.
test-part:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/part_test tests/unit/part_test.c \
	    c/drivers/block/part.c c/drivers/block/crc32.c -Ic/drivers/block
	@$(BUILD)/part_test

# The same under ASan/UBSan. The parser reads attacker-shaped sector images into
# fixed buffers with offsets taken from those same images, so an out-of-bounds
# read is the failure mode to look for, and it is invisible without this.
test-part-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/part_asan tests/unit/part_test.c \
	    c/drivers/block/part.c c/drivers/block/crc32.c -Ic/drivers/block
	@UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/part_asan

# AHCI/SATA on device. Three boots, each asserting a different claim over the
# serial log -- enumeration alone is not the claim, "the OS booted off it" is:
#   raw  the disk is attached to an ich9-ahci controller with no partition table
#        (today's image, new transport): the controller is found, the port
#        signature says SATA disk, and logitfs mounts and reads a file off it.
#   mbr  the same filesystem inside partition 1 of an MBR-partitioned disk.
#   gpt  the same, inside a GPT partition behind a protective MBR.
# The mbr/gpt runs are the ones that prove a partition table is understood: the
# filesystem no longer starts at LBA 0, so a kernel that cannot read a partition
# table cannot find its own root.
test-ahci: $(ISO) $(DISK)
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) raw
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) mbr
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) gpt
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) two

test-ahci-raw: $(ISO) $(DISK)
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) raw
test-ahci-mbr: $(ISO) $(DISK)
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) mbr
test-ahci-gpt: $(ISO) $(DISK)
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) gpt
test-ahci-two: $(ISO) $(DISK)
	@bash tests/boot/run-ahci-test.sh $(ISO) $(DISK) two


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

# The DOM and its parser are one unit now: dom_parse() IS html_parse(), so
# every host test that links dom.c links the tree builder and tokenizer with
# it. dom_serialize.c rides along because js_dom.c's innerHTML calls it.
HTML_PARSER_SRC := c/apps/browser/dom.c c/apps/browser/html_tree.c \
                   c/apps/browser/html_tokenizer.c c/apps/browser/dom_serialize.c

test-html5lib: $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/html5lib_test tests/unit/html5lib_test.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/html5lib_test third_party/html5lib-tests/tree-construction \
	    $(if $(V),-v $(V),) $(if $(BASELINE),--write-baseline,) $(if $(STRICT),--strict,)

# The same corpus under ASan/UBSan/LeakSanitizer, plus a fuzz pass that feeds
# truncations and mutations of every case through 12 fragment contexts. That is
# what drives the adoption agency's reparenting loop against unbalanced stacks,
# which is where a hand-written tree builder fails by use-after-free.
# STRICT makes it exit non-zero on a regression against the expected-fail list.
test-html5lib-asan: $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/html5lib_asan tests/unit/html5lib_test.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	    $(BUILD)/html5lib_asan third_party/html5lib-tests/tree-construction --strict
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/html5lib_fuzz tests/unit/html5lib_fuzz.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	    $(BUILD)/html5lib_fuzz third_party/html5lib-tests/tree-construction

# Does the H.264 decoder work on LogitOS, not just on the host? make test-h264
# proves it bit-exact against ffmpeg, but that is a glibc build on Linux. This
# boots the OS, runs /bin/vidcheck on the stream packed into the disk image,
# and requires the CRC32 to match the pinned one -- so mini-libc's malloc, the
# 24 MiB arena, boot-time SSE and LogitFS are all in the loop.
test-video: $(ISO) $(DISK)
	@bash tests/boot/run-video-test.sh $(ISO) $(DISK)

# The per-window event ring, split out of wm.c so it can be tested on the host:
# FIFO order, motion coalescing under flood, and -- the property that matters --
# that a click is never merged away by the motion samples around it.
test-evq:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/evq_test tests/unit/evq_test.c \
	    c/kernel/gui/evq.c -Ic/kernel/gui -Iinclude/abi
	@$(BUILD)/evq_test

# Does the monotonic clock actually advance, at the rate it claims? Cross-checked
# on device against the CMOS RTC, which is an independent timer -- so a tick/ms
# confusion or a wrong PIT mode fails instead of agreeing with itself.
test-clock: $(ISO) $(DISK)
	@bash tests/boot/run-clock-test.sh $(ISO) $(DISK)

# Drives real PS/2 input over QMP: move, press, release, right button, wheel
# both ways, shift held, window-local coordinates -- then floods the ring and
# reads the kernel's own queued/merged/dropped counters back.
test-input: $(ISO) $(DISK)
	@bash tests/boot/run-input-test.sh $(ISO) $(DISK)

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

# Host unit test for the TCP state machines (white-box: #includes tcp.c).
# Stub headers in tests/unit/tcpstub let tcp.c compile on the host (no x86 asm).
# Covers reassembly, option negotiation (window scale / SACK / timestamps, and
# the fallback when the peer refuses each), PAWS, Nagle, delayed ACK, PMTU, and
# the RFC 5681 congestion window driven against a synthetic loss pattern.
test-tcp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/tcp_test tests/unit/tcp_test.c -Itests/unit/tcpstub -Ic/net/transport
	@./$(BUILD)/tcp_test

# The negative control for the congestion-control assertions. Builds the SAME
# test with the controller's response to loss neutralised (in the test file --
# tcp.c carries no test hooks) and REQUIRES the run to fail. A cwnd test that
# still passes with the window reduction removed is not testing anything.
test-tcp-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DTCP_NEGATIVE_CONTROL -o $(BUILD)/tcp_test_negctl tests/unit/tcp_test.c \
		-Itests/unit/tcpstub -Ic/net/transport
	@if ./$(BUILD)/tcp_test_negctl >$(BUILD)/tcp_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without congestion control"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/tcp_negctl.log) checks fail without the loss response"; \
	fi

# Host protocol tests for IPv4 validation/reassembly, UDP checksums, ICMP
# echo matching and error routing, and the DNS waiter's error path.
test-net-proto:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/net_proto_test tests/unit/net_proto_test.c \
		-Ic/net/core -Ic/net/link -Ic/net/ip -Ic/net/transport -Ic/net/dns \
		-Ic/drivers/timer -Ic/kernel/core
	@./$(BUILD)/net_proto_test

test-net: test-tcp-host test-tcp-negctl test-net-proto test-dhcp-host

# ---- Terminal / shell ------------------------------------------------------
# LRT/1 framing (c/apps/coreutils/logit_rich.h): round trip at every chunk size,
# resync past garbage, truncated frames, impossible lengths, payload underflow,
# and 50 KiB of random noise that must never yield an unbounded frame.
test-term-proto:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/term_proto_test tests/unit/term_proto_test.c \
		-Ic/apps/coreutils -Ic/apps -Iinclude/abi
	@./$(BUILD)/term_proto_test

# /bin/sh itself. The test #includes sh.c and links it against a pipe model with
# honest reader/writer refcounts (tests/unit/sh_hoststub.h), so the job-control
# and control-channel paths are the ones the OS runs.
test-sh:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/sh_edit_test tests/unit/sh_edit_test.c \
		-Itests/unit -Ic/apps/coreutils -Ic/apps -Iinclude/abi
	@./$(BUILD)/sh_edit_test

# Negative control: rebuild the SAME test with the shell's environment filter
# bypassed (the naive "give every child the rich channel" design) and REQUIRE it
# to fail. A compatibility assertion that still passes without the filter is not
# testing the thing that keeps protocol bytes out of a redirected file.
test-sh-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DSH_NEGATIVE_CONTROL -o $(BUILD)/sh_edit_negctl tests/unit/sh_edit_test.c \
		-Itests/unit -Ic/apps/coreutils -Ic/apps -Iinclude/abi
	@if ./$(BUILD)/sh_edit_negctl >$(BUILD)/sh_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with the rich-channel filter removed"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/sh_negctl.log) checks fail without the filter"; \
	fi

test-term-host: test-term-proto test-sh test-sh-negctl

# On-device: rich output judged by PIXELS (an image at the right size, a drawn
# progress bar, a ruled table) plus the compatibility claim -- the same commands
# redirected to a file must leave a file with no protocol bytes in it.
test-term-ui: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_rich_term.py $(ISO) $(DISK) $(BUILD)/richterm.ppm

test-dhcp-host: $(BUILD)
	$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/dhcp_test tests/unit/dhcp_test.c -Ic/net/core -Ic/net/transport -Ic/drivers/timer -Ic/kernel/core
	$(BUILD)/dhcp_test

# End-to-end e1000 -> IPv4 -> TCP -> HTTP transfer against a host-local server.
test-net-os: $(ISO) $(DISK)
	@bash tests/boot/run-net-test.sh $(ISO) $(DISK)

# The non-blocking socket ABI. Kernel sockets cannot be host-tested -- they are
# a syscall over a real stack over a real NIC -- so this boots LogitOS and runs
# /bin/socktest against a host server. It does not merely check that four
# transfers succeed (a secretly-serial implementation would too): it requires
# that the LAST connection came up before the FIRST one finished, and that the
# per-socket events interleave more than a sequential run could produce.
test-sock: $(ISO) $(DISK)
	@bash tests/boot/run-sock-test.sh $(ISO) $(DISK)

# The other half, and the reason any of this was worth doing: does the DESKTOP
# still respond while a transfer runs? Injects real clicks over QMP during a
# deliberately slow four-connection fetch and measures how long each takes to
# reach an app. Runs the same injection against the old blocking SYS_HTTP_GET
# for contrast -- if the control does not freeze, the measurement proves nothing.
test-sock-ui: $(ISO) $(DISK)
	@bash tests/boot/run-sock-ui-test.sh $(ISO) $(DISK)

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

# --- mini-libc <-> glibc differential test --------------------------------
#
# The `test-crypto-diff` idiom applied to the C library: build the mini-libc
# sources UNMODIFIED with every symbol renamed to mini_* (tests/unit/
# libc_rename.h, force-included), link them beside glibc in one process, and
# require identical results -- value, endptr, errno and formatted bytes -- over
# a large randomized and adversarial corpus. This is the only kind of test that
# catches the failures that matter in a libc, because a strtod that misrounds
# the last bit and a printf that rounds 2.5 the wrong way are both silent.
#
# The mini-libc TUs are compiled -nostdinc against their own headers plus
# clang's freestanding ones, exactly as they are for the target -- otherwise
# glibc's <features.h> and mini-libc's collide (the same reason HOST_INCDIRS
# exists). Each source is a separate object because several define a static
# helper of the same name.
#
# The suite also builds a SABOTAGED copy of itself (one strtod result in a
# thousand perturbed by one ulp, tests/unit/libc_sabotage.c) and REQUIRES it to
# fail. A suite that has never failed is not known to be able to.
LIBCDIFF_SRC  := $(filter-out c/apps/libc/src/malloc.c c/apps/libc/src/io.c c/apps/libc/src/runtime.c,\
                   $(wildcard c/apps/libc/src/*.c))
LIBCDIFF_INC  := -nostdinc -isystem $(shell $(CC) -print-resource-dir)/include \
                 -Ic/apps/libc/include -Ic/apps/libc/src -Iinclude/abi
LIBCDIFF_SAN  := -fsanitize=address,undefined -fno-sanitize-recover=all -g
LIBCDIFF_OBJ  := $(patsubst %.c,$(BUILD)/libcdiff/%.o,$(LIBCDIFF_SRC))
LIBCDIFF_SOBJ := $(patsubst %.c,$(BUILD)/libcdiff-sab/%.o,$(LIBCDIFF_SRC))

$(BUILD)/libcdiff/%.o: %.c tests/unit/libc_rename.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O1 -w $(LIBCDIFF_SAN) $(LIBCDIFF_INC) \
	      -include tests/unit/libc_rename.h -c $< -o $@
$(BUILD)/libcdiff-sab/%.o: %.c tests/unit/libc_rename.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O1 -w -DLIBC_SABOTAGE $(LIBCDIFF_SAN) $(LIBCDIFF_INC) \
	      -include tests/unit/libc_rename.h -c $< -o $@

test-libc-diff: $(LIBCDIFF_OBJ) $(LIBCDIFF_SOBJ) tests/unit/libc_diff_test.c tests/unit/libc_sabotage.c
	@$(CC) -std=gnu11 -O1 -g $(LIBCDIFF_SAN) -o $(BUILD)/libc_diff_test \
	    tests/unit/libc_diff_test.c $(LIBCDIFF_OBJ) -lm
	@$(CC) -std=gnu11 -O1 -g $(LIBCDIFF_SAN) -DLIBC_DIFF_NEGATIVE_CONTROL \
	    -o $(BUILD)/libc_diff_sabotaged \
	    tests/unit/libc_diff_test.c tests/unit/libc_sabotage.c $(LIBCDIFF_SOBJ) -lm
	@echo "--- mini-libc vs glibc"
	@$(BUILD)/libc_diff_test $(LIBCDIFF_ITERS) $(LIBCDIFF_SEED)
	@echo "--- negative control (must detect a 1-ulp strtod regression)"
	@$(BUILD)/libc_diff_sabotaged $(LIBCDIFF_ITERS) $(LIBCDIFF_SEED)

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

# --- the kernel log ring, host-side ---------------------------------------
# Compiles the REAL c/kernel/core/klog.c + kprintf.c against tests/unit/klogstub
# (which shadows the interrupt guard, the spinlock, per-CPU identity, the timer
# and the two console sinks) and asserts wraparound, truncation, level
# filtering, full-ring behaviour, two producers interleaving mid-line, and the
# rendered form `cat /dev/kmsg` serves.
# --- the same, on the machine ---------------------------------------------
# test-panic-log: the ring survives the events that wrote it, is readable from
#   userland (`cat /dev/kmsg`), overwrites instead of blocking when full, and
#   -- the claim that matters -- takes records from a real interrupt handler
#   driven by asynchronous IPIs without tearing or deadlocking.
# test-panic: a deliberate panic prints a reason, registers, a backtrace and
#   the log, then halts without rebooting. Every backtrace frame is resolved
#   against build/kernel.map and required to land on the functions that really
#   called panic(). Negative control: `make FPO=1` (no frame pointers).
test-panic-log: $(ISO) $(DISK)
	@bash tests/boot/run-panic-log-test.sh $(ISO) $(DISK)

test-panic: $(ISO) $(DISK)
	@bash tests/boot/run-panic-test.sh $(ISO) $(DISK)

KLOG_TEST_SRC := tests/unit/log_test.c c/kernel/core/klog.c c/kernel/core/kprintf.c
KLOG_TEST_INC := -Itests/unit/klogstub -Ic/kernel/core
test-klog:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(KLOG_TEST_INC) -o $(BUILD)/log_test $(KLOG_TEST_SRC)
	@$(BUILD)/log_test

# THE NEGATIVE CONTROL, and the reason test-klog is evidence rather than
# decoration. -DKLOG_UNSAFE removes klog's interrupt guard and its per-CPU line
# buffers -- the logger you write if you do not think about interrupt context.
# This target REQUIRES the suite to fail; if it ever passes, test-klog is
# measuring something other than the property it claims to.
test-klog-control:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -w -DKLOG_UNSAFE $(KLOG_TEST_INC) \
	    -o $(BUILD)/log_test_unsafe $(KLOG_TEST_SRC)
	@if $(BUILD)/log_test_unsafe > $(BUILD)/log_test_unsafe.out 2>&1; then \
	    echo "CONTROL FAILED: the naive logger passed the interleaving test"; \
	    cat $(BUILD)/log_test_unsafe.out; exit 1; \
	 else \
	    echo "control ok: -DKLOG_UNSAFE fails as expected --"; \
	    grep -E '^  FAIL|failures' $(BUILD)/log_test_unsafe.out | head -8; \
	 fi

# kheap host test: compiles the real kheap.c against stub pmm/spinlock/kprintf
# headers (tests/unit/kheapstub/ shadows the kernel ones via -I order) and asserts
# the no-two-live-allocations-overlap invariant -- including across injected
# pmm_alloc_contig failures (the grow() double-accounting bug class).
test-kheap:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address -o $(BUILD)/kheap_test tests/unit/kheap_test.c c/kernel/mm/kheap.c \
	    -Itests/unit/kheapstub -Ic/kernel/mm
	@$(BUILD)/kheap_test

# mini-libc allocator host test: asserts the SCALING of c/apps/libc/src/malloc.c,
# not a duration -- doubling the number of live blocks must roughly double the
# time, which the pre-2026-08 first-fit-from-the-arena-base allocator failed by a
# factor of four per doubling. malloc.c's entry points are renamed on the command
# line (a host process cannot have two mallocs) and the arena is enlarged to
# 64 MiB so the sweep can hold 240k blocks live. See the file header for the
# before/after curve and for how to run the negative control.
MALLOC_TEST_ARENA := 67108864u
test-malloc:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -DARENA_SIZE=$(MALLOC_TEST_ARENA) -Dmalloc=lmalloc -Dfree=lfree \
	    -Drealloc=lrealloc -Dcalloc=lcalloc -Dmalloc_usable_size=lmalloc_usable_size \
	    -c c/apps/libc/src/malloc.c -o $(BUILD)/malloc_under_test.o
	@$(CC) -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -DARENA_SIZE=$(MALLOC_TEST_ARENA) -o $(BUILD)/malloc_test \
	    tests/unit/malloc_test.c $(BUILD)/malloc_under_test.o
	@$(BUILD)/malloc_test

# --- test-browser: host unit tests for the ring-3 browser render pipeline ---
# The tests self-stub kmalloc/kfree/img_* so they link the real pipeline
# sources (dom/css_engine/css_vars/layout/js_dom) on the host. LibCSS is
# archived once per build tree (libcss_host.a) and shared by the CSS tests.
BTEST_INC := -Ic/apps/browser -Ic/lib/image -Ic/net/http -Ic/lib/text
# The painter draws through logit.h's `int 0x80` wrappers, which a host process
# cannot execute. tests/unit/painthost/logit.h shadows them with recorders, so
# paint_test links the REAL browser_paint.c and asserts on the draw ops. It must
# come first on the include path (same shape as tests/unit/kheapstub).
PAINT_INC := -Itests/unit/painthost
CSSHOST_OBJ := $(patsubst %.c,$(BUILD)/csshost/%.o,$(CSS_SRC))

# -MMD -MP so a change to a vendored LibCSS HEADER rebuilds the objects that
# include it. Without them this rule tracked .c files only, and LibCSS keeps
# real code in headers -- select/mq.h is where every @media block in every page
# is actually matched. Patching it and rebuilding produced a byte-identical
# archive and the fix silently never reached any test binary. Same shape as the
# roots_bundle.inc gotcha in CLAUDE.md, one directory over.
$(BUILD)/csshost/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER -MMD -MP $(CSS_INC) -c $< -o $@

$(BUILD)/libcss_host.a: $(CSSHOST_OBJ)
	@ar rcs $@ $^

test-browser: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/dom_test tests/unit/dom_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/dom_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/dom_api_test tests/unit/dom_api_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/dom_api_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/var_test tests/unit/var_test.c \
	    tests/unit/css_hostmm.c c/apps/browser/css_vars.c c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/var_test
# css_vars_test: the cascade half of the same file -- WHICH declaration wins,
# rather than whether substitution happens. It links css_engine.c because the
# @media verdict is now LibCSS's own (css_select_ctx_media_matches), which is
# the entire point: there is one evaluator, not one per caller.
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_vars_test tests/unit/css_vars_test.c \
	    tests/unit/css_hostmm.c c/apps/browser/css_vars.c c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/css_vars_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/parse_fuzz tests/unit/parse_fuzz.c c/net/http/url.c c/lib/text/utf8.c
	@$(BUILD)/parse_fuzz
	@$(CC) -O2 -w $(HOST_INCDIRS) -o $(BUILD)/http_dechunk_test tests/unit/http_dechunk_test.c c/net/http/url.c
	@$(BUILD)/http_dechunk_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_engine_test tests/unit/css_engine_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/css_engine_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_extra_test tests/unit/css_extra_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c $(HTML_PARSER_SRC) \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/css_extra_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_test tests/unit/layout_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/layout_test
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(BUILD)/paint_test tests/unit/paint_test.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(HTML_PARSER_SRC) \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c $(BUILD)/libcss_host.a
	@$(BUILD)/paint_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/page_test tests/unit/page_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/page_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/table_list_test tests/unit/table_list_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/table_list_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/pipeline_stress tests/unit/pipeline_stress.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/pipeline_stress
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_svg_test tests/unit/layout_svg_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c tests/unit/rust_host_shim.c \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -Ic/kernel/mm
	@$(BUILD)/layout_svg_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_dom_test \
	    tests/unit/js_dom_test.c c/apps/browser/js_dom.c c/apps/browser/js_page.c \
	    c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/js_dom_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/http1_test tests/unit/http1_test.c \
	    c/net/http/http1.c tests/unit/rust_host_shim.c $(RUST_LIB_HOST)
	@$(BUILD)/http1_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/cookie_test tests/unit/cookie_test.c c/net/http/cookies.c
	@$(BUILD)/cookie_test
	@$(CC) -O2 -w $(BTEST_INC) -o $(BUILD)/hpool_test tests/unit/hpool_test.c c/net/http/hpool.c
	@$(BUILD)/hpool_test
	@echo "test-browser: ALL PASS"

# --- test-webapi: the Web API surface outside the DOM, host-side ----------
# fetch/XHR/Storage/history/location/URL/URLSearchParams/matchMedia. It links
# the REAL js_webapi.c, the real HTTP/1.1 parser and the real URL parser, and
# injects an in-memory server through the transport vtable -- so the whole
# request/response state machine (short writes, 7-byte reads, redirects,
# chunked bodies, concurrent requests) is exercised without QEMU.
# -DWEBAPI_HOST keeps logit.h's `int 0x80` wrappers out of the host binary.
WEBAPI_TEST_SRC := tests/unit/webapi_test.c c/apps/browser/js_webapi.c \
                   c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
                   tests/unit/rust_host_shim.c
test-webapi: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/webapi_test $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/webapi_test

# Same, under ASan/UBSan with the leak checker on. Every fetch holds two
# promise resolvers, a request buffer and a parsed response; the failure mode
# for all of it is a leak or a use-after-free on the abandon path, neither of
# which shows up as a wrong answer.
test-webapi-asan: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/webapi_asan $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/webapi_asan

# --- test-webapi-page: the on-device proof that fetch() reaches the pixels --
# Boots the OS, loads a fixture page from a host server, and requires the text
# a fetch() wrote into the DOM to appear in a screendump -- plus location, URL,
# Storage, history/popstate and matchMedia over the serial log.
test-webapi-page: $(ISO) $(DISK)
	python3 tests/qmp/qmp_webapi_page.py $(ISO) $(DISK)

# The negative control: the SAME harness against a browser.aex linked without
# js_webapi.o (the weak declarations in js_page.c make that link cleanly and
# come up with no fetch at all). It asserts the positive run's assertions all
# fail. If this ever passes the positive checks, test-webapi-page is measuring
# something other than this change.
NOFETCH_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_webapi.o,$(BROWSER_JS_OBJ))
$(BUILD)/browser-nofetch.elf: $(ENGINE_OBJ) $(NOFETCH_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOFETCH_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(BUILD)/browser-nofetch.aex: $(BUILD)/browser-nofetch.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nofetch.elf $@ Browser - 'B' 120 130 240

test-webapi-page-control: $(ISO) $(BUILD)/browser-nofetch.aex
	@$(MAKE) DISK=$(BUILD)/disk-nofetch.img BROWSER_AEX=$(BUILD)/browser-nofetch.aex $(BUILD)/disk-nofetch.img
	python3 tests/qmp/qmp_webapi_page.py $(ISO) $(BUILD)/disk-nofetch.img --expect-no-webapi

# --- test-fetch-ui: is the desktop still responsive DURING a page's fetch? --
# The same instrument as test-sock-ui, pointed at the browser: real clicks are
# injected while a page's fetch() is mid-transfer and timed to a ring-3 app,
# with the old blocking SYS_HTTP_GET run through the identical instrument as
# the control.
test-fetch-ui: $(ISO) $(DISK)
	python3 tests/qmp/qmp_fetch_ui.py $(ISO) $(DISK)

# --- test-stream: streaming, SSE framing, EventSource, abort ---------------
# Same in-memory transport as test-webapi, with one addition that is the whole
# point: the fake server can RELEASE a response in pieces, so a test can assert
# the page held the first tokens while the response was still open. See the
# header of tests/unit/stream_net.h.
STREAM_TEST_SRC := c/apps/browser/js_webapi.c c/net/http/http1.c c/net/http/url.c \
                   c/net/http/cookies.c tests/unit/rust_host_shim.c
test-stream: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/stream_test tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@$(BUILD)/stream_test

# The negative control. The SAME test file against js_webapi.c built with
# -DWEBAPI_NO_STREAM, which is the buffer-until-complete fetch this change
# replaced: the partial-delivery assertions are inverted and must hold. If this
# ever shows partial delivery, test-stream is measuring something else.
test-stream-control: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -DWEBAPI_HOST -DWEBAPI_NO_STREAM \
	    -o $(BUILD)/stream_control tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@$(BUILD)/stream_control

test-stream-asan: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/stream_asan tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/stream_asan

# --- test-cookie-cors: which requests carry the session, and which
# cross-origin responses a page may read. Every refusal is paired with the
# permitted case and with an assertion about what went on the wire, because a
# browser that simply ignored CORS would pass the permitted half.
test-cookie-cors: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/cookie_cors_test tests/unit/cookie_cors_test.c $(STREAM_TEST_SRC) \
	    $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/cookie_cors_test

test-cookie-cors-asan: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/cookie_cors_asan tests/unit/cookie_cors_test.c $(STREAM_TEST_SRC) \
	    $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/cookie_cors_asan

# --- test-sse-page: the on-device proof, TIMED -----------------------------
# "It streams" is a claim about WHEN bytes become visible, so the host server
# emits SSE tokens with deliberate gaps and the harness screenshots between
# them: partial content must be on the framebuffer while the response is still
# open, with timestamps. A fully buffered browser passes a final-text check and
# fails this one.
test-sse-page: $(ISO) $(DISK)
	python3 tests/qmp/qmp_sse_page.py $(ISO) $(DISK)

# The device negative control: the same harness against a browser.aex whose
# js_webapi.c was built with -DWEBAPI_NO_STREAM.
NOSTREAM_OBJ := $(BUILD)/nostream/js_webapi.o
$(NOSTREAM_OBJ): c/apps/browser/js_webapi.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(JS_INC) -DWEBAPI_NO_STREAM -c $< -o $@
NOSTREAM_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_webapi.o,$(BROWSER_JS_OBJ)) $(NOSTREAM_OBJ)
$(BUILD)/browser-nostream.elf: $(ENGINE_OBJ) $(NOSTREAM_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOSTREAM_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(BUILD)/browser-nostream.aex: $(BUILD)/browser-nostream.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nostream.elf $@ Browser - 'B' 120 130 240

test-sse-page-control: $(ISO) $(BUILD)/browser-nostream.aex
	@$(MAKE) DISK=$(BUILD)/disk-nostream.img BROWSER_AEX=$(BUILD)/browser-nostream.aex $(BUILD)/disk-nostream.img
	python3 tests/qmp/qmp_sse_page.py $(ISO) $(BUILD)/disk-nostream.img --expect-buffered

# --- test-stream: streaming, SSE framing, EventSource, abort ---------------
# Same in-memory transport as test-webapi, with one addition that is the whole
# point: the fake server can RELEASE a response in pieces, so a test can assert
# the page held the first tokens while the response was still open. See the
# header of tests/unit/stream_net.h.
STREAM_TEST_SRC := c/apps/browser/js_webapi.c c/net/http/http1.c c/net/http/url.c \
                   c/net/http/cookies.c tests/unit/rust_host_shim.c
test-stream: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/stream_test tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@$(BUILD)/stream_test

# The negative control. The SAME test file against js_webapi.c built with
# -DWEBAPI_NO_STREAM, which is the buffer-until-complete fetch this change
# replaced: the partial-delivery assertions are inverted and must hold. If this
# ever shows partial delivery, test-stream is measuring something else.
test-stream-control: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -DWEBAPI_HOST -DWEBAPI_NO_STREAM \
	    -o $(BUILD)/stream_control tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@$(BUILD)/stream_control

test-stream-asan: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/stream_asan tests/unit/stream_test.c $(STREAM_TEST_SRC) $(QJS_SRC) \
	    $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/stream_asan

# --- test-cookie-cors: which requests carry the session, and which
# cross-origin responses a page may read. Every refusal is paired with the
# permitted case and with an assertion about what went on the wire, because a
# browser that simply ignored CORS would pass the permitted half.
test-cookie-cors: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/cookie_cors_test tests/unit/cookie_cors_test.c $(STREAM_TEST_SRC) \
	    $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/cookie_cors_test

test-cookie-cors-asan: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
	    -o $(BUILD)/cookie_cors_asan tests/unit/cookie_cors_test.c $(STREAM_TEST_SRC) \
	    $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/cookie_cors_asan

# --- test-sse-page: the on-device proof, TIMED -----------------------------
# "It streams" is a claim about WHEN bytes become visible, so the host server
# emits SSE tokens with deliberate gaps and the harness screenshots between
# them: partial content must be on the framebuffer while the response is still
# open, with timestamps. A fully buffered browser passes a final-text check and
# fails this one.
test-sse-page: $(ISO) $(DISK)
	python3 tests/qmp/qmp_sse_page.py $(ISO) $(DISK)

# The device negative control: the same harness against a browser.aex whose
# js_webapi.c was built with -DWEBAPI_NO_STREAM.
NOSTREAM_OBJ := $(BUILD)/nostream/js_webapi.o
$(NOSTREAM_OBJ): c/apps/browser/js_webapi.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(JS_INC) -DWEBAPI_NO_STREAM -c $< -o $@
NOSTREAM_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_webapi.o,$(BROWSER_JS_OBJ)) $(NOSTREAM_OBJ)
$(BUILD)/browser-nostream.elf: $(ENGINE_OBJ) $(NOSTREAM_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOSTREAM_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(BUILD)/browser-nostream.aex: $(BUILD)/browser-nostream.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nostream.elf $@ Browser - 'B' 120 130 240

test-sse-page-control: $(ISO) $(BUILD)/browser-nostream.aex
	@$(MAKE) DISK=$(BUILD)/disk-nostream.img BROWSER_AEX=$(BUILD)/browser-nostream.aex $(BUILD)/disk-nostream.img
	python3 tests/qmp/qmp_sse_page.py $(ISO) $(BUILD)/disk-nostream.img --expect-buffered

# --- test-http-fuzz: ASan+UBSan fuzz for the ring-3 HTTP client -----------
# The layer this replaces (c/net/http/http.c) parses attacker-chosen bytes in
# ring 0 and has never been fuzzed. Beyond not crashing, each phase asserts a
# property: that a response parses identically however the bytes are split
# across reads, that a built request cannot carry an injected header, that the
# cookie jar never answers a host the RFC rules forbid, and that the pool never
# breaches its caps. ~1 s per scale unit, so it is cheap enough to run often;
# `make test-http-fuzz SCALE=20 SEED=0x1234` goes deeper.
SCALE ?= 6
SEED  ?= 0x243F6A8885A308D3
test-http-fuzz: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w $(BTEST_INC) \
	    -o $(BUILD)/http1_fuzz tests/unit/http1_fuzz.c $(RING3_NET) \
	    tests/unit/rust_host_shim.c $(RUST_LIB_HOST)
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/http1_fuzz $(SCALE) $(SEED)

# Same test under ASan+UBSan. The event system is where a JSValue can outlive its
# runtime and a listener can outlive its node, and neither of those shows up as a
# wrong answer -- only as corrupted memory some events later. So it gets its own
# instrumented run rather than riding on the -O2 build's silence.
test-js-dom-asan: $(BUILD)/libcss_host.a
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_dom_asan \
	    tests/unit/js_dom_test.c c/apps/browser/js_dom.c c/apps/browser/js_page.c \
	    c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/js_dom_asan

# The CSS engine + layout under ASan/UBSan, and -- for the engine -- LeakSanitizer.
#
# The leak check is the point of the first half. LibCSS builds a css_node_data
# (an ancestor bloom filter plus refs to the element's selection results) for
# every element on every css_apply and hands it to the client's
# set_libcss_node_data. Ours used to be a no-op, so all of it was dropped on the
# floor: roughly 70 bytes per element per re-style, and a page re-styles three
# or four times as its external sheets arrive. css_engine_test frees its own
# documents at the end, so anything LSan still reports is LibCSS's.
#
# layout.c gets ASan/UBSan without the leak check: the layout tests deliberately
# leave their documents allocated, and what matters there is that the display
# list's index arithmetic (the flex line/range bookkeeping, the z-index sort)
# stays in bounds.
test-css-asan: $(BUILD)/libcss_host.a
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_engine_asan tests/unit/css_engine_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/css_engine_asan >/dev/null
	@echo "css_engine_test: ASan + UBSan + LeakSanitizer clean"
# The custom-property scanner walks attacker-controlled stylesheet bytes with
# raw index arithmetic (it looks BACKWARDS from a declaration start, trims
# spans, and skips comments and strings that may run off the end of the
# buffer). A wrong answer is what css_vars_test measures; an out-of-bounds read
# is what these two measure.
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_vars_asan tests/unit/css_vars_test.c \
	    tests/unit/css_hostmm.c c/apps/browser/css_vars.c c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/css_vars_asan >/dev/null
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_vars_fuzz tests/unit/css_vars_fuzz.c \
	    tests/unit/css_hostmm.c c/apps/browser/css_vars.c c/apps/browser/css_engine.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/css_vars_fuzz $(FUZZ_N)
	@echo "css_vars: ASan + UBSan clean over the unit cases and the fuzz corpus"
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_asan tests/unit/layout_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c \
	    c/apps/browser/css_vars.c $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/layout_asan >/dev/null
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(BUILD)/paint_asan tests/unit/paint_test.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(HTML_PARSER_SRC) \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/paint_asan >/dev/null
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/table_list_asan tests/unit/table_list_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c \
	    c/apps/browser/css_vars.c $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/table_list_asan >/dev/null
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/page_asan tests/unit/page_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c \
	    c/apps/browser/css_vars.c $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/page_asan >/dev/null
	@echo "test-css-asan: ALL PASS"

# --- test-css-fidelity: the on-device proof that the CSS reaches the pixels ---
# Host tests assert on the display list, which is one step short: a box can have
# the right geometry in `struct item` and still never be painted. This boots the
# OS, serves a fixture from the host and measures the screendump -- border-box
# vs content-box painted widths, a <pre> that keeps its blank line and its
# indentation, and a flex row that wraps and honours justify-content.
test-css-fidelity: $(ISO) $(DISK)
	python3 tests/qmp/qmp_css_fidelity.py $(ISO) $(DISK)

# --- test-live-page: the on-device proof that a loaded page stays alive ---
# Boots the OS, serves a fixture page from the host, loads it in the Browser and
# then CLICKS it: a handler mutates the DOM, a setTimeout fires seconds after the
# load, and a link's preventDefault suppresses the navigation (with an identical
# handler-free link as the control). Screenshots + the host server's request log
# are the evidence; see the docstring in tests/qmp/qmp_live_page.py.
test-live-page: $(ISO) $(DISK)
	python3 tests/qmp/qmp_live_page.py $(ISO) $(DISK)

# --- test-modules: ES modules, on the machine, in the pixels ---
# Boots the OS and serves a fixture whose <script type="module"> imports a
# second file. The decisive assertion is that TWO modules importing the SAME
# specifier "./lib.mjs" get two DIFFERENT files, because resolution is against
# the importing module's URL and not against the document -- checked from the
# page's console, from the host server's request log, and from the screen (each
# module paints a colour that appears in no stylesheet on the page).
# On a build without the module loader this fails at the first module: QuickJS
# reports `SyntaxError: expecting '('` on the import statement.
test-modules: $(ISO) $(DISK)
	python3 tests/qmp/qmp_module_page.py $(ISO) $(DISK)

# --- test-handshakes: what one real page costs in TLS handshakes ---
# `grep -c 'chain of'` on the guest serial log IS the handshake count: the TLS
# layer prints one line per verified chain and nothing in the browser can forge
# it. en.wikipedia.org/wiki/Operating_system cost 14 before the connection pool
# was wired in and 4 after. Also screendumps the page, because a handshake count
# that fell because the page stopped loading is not an improvement.
#   make test-handshakes URL=https://example.com/ MAXHS=3
# The gate is 12, not the best observed number. The count depends on how eagerly
# the far-side CDN drops keep-alive connections, and repeated runs against
# wikipedia landed on 4, 7, 9, 9 and 10 against a 14 baseline -- so a gate at
# the best run would be a flaky test rather than a stricter one. Read the
# printed number; the gate only catches a regression to the old behaviour.
URL   ?= https://en.wikipedia.org/wiki/Operating_system
MAXHS ?= 12
test-handshakes: $(ISO) $(DISK)
	python3 tests/qmp/qmp_handshakes.py $(ISO) $(DISK) '$(URL)' $(MAXHS) build/handshakes.ppm

# --- test-dom-device: the on-device proof for the same bindings ---
# The host test asserts on `struct node`; this one boots the OS, serves a page
# whose script builds a subtree entirely through the new bindings, and asserts
# on the SCREENDUMP -- three colours that only a className= can produce, in the
# order insertBefore asked for, at the geometry getBoundingClientRect reported.
# See the docstring in tests/qmp/qmp_dom_bindings.py.
test-dom-device: $(ISO) $(DISK)
	python3 tests/qmp/qmp_dom_bindings.py $(ISO) $(DISK)

# --- test-dom-bindings: the Node half of the JS bindings, against real layout ---
# Separate from js_dom_test (which links neither layout nor the codecs) because
# getBoundingClientRect can only be checked against the display list layout.c
# actually produced -- the point is that what a script MEASURES and what the
# painter DRAWS are the same numbers.
test-dom-bindings: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -o $(BUILD)/dom_bindings_test tests/unit/dom_bindings_test.c \
	    c/apps/browser/js_dom.c c/apps/browser/layout.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/dom_bindings_test

# Same test under ASan+UBSan. The insertion helpers move nodes between parents
# and the attribute removal shifts a live array in place; both are the shape
# where a wrong index is silent until much later.
test-dom-bindings-asan: $(BUILD)/libcss_host.a
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -o $(BUILD)/dom_bindings_asan tests/unit/dom_bindings_test.c \
	    c/apps/browser/js_dom.c c/apps/browser/layout.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/dom_bindings_asan

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

# --- device model / PCI (c/drivers/core + c/kernel/pci) ---------------------
# Host tests run the bus driver's pure logic against a synthetic configuration
# space (tests/unit/pcistub stubs out port I/O, vmm and kprintf), which is how
# BAR sizing, capability-chain walking and bridge recursion get tested without
# hardware. -DLOGIT_HOST_TEST drops the linker-section driver registry, which
# only exists in the kernel link.
.PHONY: test-pci test-devmodel test-devmodel-a test-devmodel-b
PCI_HOST_INC := -Ic/drivers/core -Ic/kernel/pci -Itests/unit/pcistub
test-pci: $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_HOST_TEST \
	    -o $(BUILD)/pci_test tests/unit/pci_test.c c/kernel/pci/pci.c \
	    c/drivers/core/device.c $(PCI_HOST_INC)
	@$(BUILD)/pci_test
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_HOST_TEST \
	    -o $(BUILD)/devmodel_test tests/unit/devmodel_test.c \
	    c/drivers/core/device.c $(PCI_HOST_INC)
	@$(BUILD)/devmodel_test
	@# pcistub MUST come before c/kernel/cpu here: pci_msi.c reaches the real
	@# io.h otherwise and the "config space" writes go to real x86 ports.
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_HOST_TEST \
	    -o $(BUILD)/pci_msi_test tests/unit/pci_msi_test.c c/kernel/pci/pci_msi.c \
	    c/kernel/pci/pci.c c/drivers/core/device.c \
	    -Itests/unit/pcistub -Ic/drivers/core -Ic/kernel/pci -Ic/kernel/cpu
	@$(BUILD)/pci_msi_test

# Two DIFFERENT QEMU machines and device sets against the same kernel: set 'a'
# is i440fx with an e1000 + QEMU's `edu` device (asserts an MSI and a legacy
# INTx actually reached a handler); set 'b' is q35 with ECAM, an rtl8139
# instead of the e1000, an AHCI controller, and an xHCI behind a PCIe root
# port -- i.e. on a bus a bus-0-only scan cannot reach.
test-devmodel-a: $(ISO) $(DISK)
	@SET=a bash tests/boot/run-devmodel-test.sh $(ISO) $(DISK)
test-devmodel-b: $(ISO) $(DISK)
	@SET=b bash tests/boot/run-devmodel-test.sh $(ISO) $(DISK)
test-devmodel: test-devmodel-a test-devmodel-b


# ---- M28 time subsystem -----------------------------------------------------
# test-time-host  the isolable logic: the timer heap under insert/cancel/expiry
#                 (including 64 timers on ONE deadline), the ns arithmetic, a
#                 32-bit counter WRAPPING, the cross-core monotonicity clamp,
#                 and the 2x-tick negative control. Compiles the real
#                 c/kernel/core/ktime.c with a settable fake cycle counter --
#                 a test of a copy of the code proves things about the copy.
# test-time-negctl  the same suite with the cross-check's tolerance removed.
#                 REQUIRED TO FAIL: it is the proof that test-time-host's 2x
#                 assertions can fail at all.
test-time-host:
	@mkdir -p $(BUILD)
	@$(CC) -DLOGIT_TIME_HOST -O1 -g -Wall -Wextra -o $(BUILD)/time_test \
	    tests/unit/time_test.c c/kernel/core/ktime.c -Ic/kernel/core -Iinclude/abi
	@$(BUILD)/time_test

test-time-negctl:
	@mkdir -p $(BUILD)/negctl
	@sed 's|int64_t tol = 250 + 20 / (int64_t)(rtc_seconds ? rtc_seconds : 1);|int64_t tol = 1000000;|' \
	    c/kernel/core/ktime.c > $(BUILD)/negctl/ktime.c
	@cp c/kernel/core/ktime.h $(BUILD)/negctl/
	@$(CC) -DLOGIT_TIME_HOST -O1 -o $(BUILD)/time_negctl tests/unit/time_test.c \
	    $(BUILD)/negctl/ktime.c -I$(BUILD)/negctl -Iinclude/abi
	@if $(BUILD)/time_negctl >$(BUILD)/negctl/out.txt 2>&1; then \
	    echo "FAIL: with the cross-check guard disabled the suite still PASSED --"; \
	    echo "      the 2x assertions cannot fail, so they prove nothing."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): guard disabled -> the 2x assertions failed, as required"; \
	    grep 'FAIL' $(BUILD)/negctl/out.txt | sed 's/^/       /'; \
	 fi

# On device. test-time asserts the five claims the kernel prints on every boot
# (source chosen + calibrated, cross-checked against the RTC *and* the PIT
# interrupt count, the 2x guard rejecting, the PIT fallback actually switched
# onto and back off again, timers firing with a reported distribution).
test-time: $(ISO) $(DISK)
	@bash tests/boot/run-time-test.sh $(ISO) $(DISK)

# The monotonic clock across cores: one probe thread per core, and a read on one
# core after a read on another must never be smaller. Also covers -smp 1.
test-time-smp: $(ISO) $(DISK)
	@bash tests/boot/run-time-smp-test.sh $(ISO) $(DISK)


clean:
	rm -rf $(BUILD)

# Header-dependency fragments emitted by -MMD (kernel AND app objects). A stale
# object compiled against an old struct layout is memory corruption at runtime.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# NIC driver test targets (test-nic, test-nic-drv, test-nic-e1000/virtio/
# rtl8139, test-nic-none). Kept in their own fragment so they could be added
# while several agents were editing this file concurrently.
-include tests/nic.mk

# Audio test targets (test-audio, test-audio-pcm, test-audio-wav/mix/underrun/
# none) and /bin/sndtest. Same reason, same shape.
-include tests/audio.mk

# USB test targets (test-usb, test-usb-host/ring/desc/hid, test-usb-os,
# test-usb-none, test-usb-negctl). Same reason, same shape. The DRIVER needs no
# Makefile change at all -- C_SRC globs c/drivers and it registers itself
# through the device model's linker section.
-include tests/usb.mk
