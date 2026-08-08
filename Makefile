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
# The shaping font. Vendored unmodified (see third_party/fonts/README.md):
# the Noto subsets above carry no Arabic, no Hebrew and -- because subsetting
# stripped them -- no GSUB/GPOS at all, so the shaper would have nothing to do.
FONT_TEXT   := third_party/fonts/DejaVuSans.ttf
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

# Colocated headers resolve via -I across every source dir. That works only
# while header basenames are unique, and it needs the -I ORDER to be a function
# of the sources rather than of the disk. Both properties were quietly broken:
#
#  1. `find` emits directories in filesystem traversal order, and this repo is
#     built from an NTFS working tree AND from ext4 clones. The two orders
#     differ, so `-Ic/apps/libc/include/sys` sorted before or after
#     `-Ic/kernel/core` depending on where you stood. $(sort) makes the command
#     line a function of the tree. (Same failure as tools/genroots.py's sort
#     key, fixed in 912175a: a build input that depended on the filesystem.)
#
#  2. c/apps/libc/include/sys must NOT be on the include path. Its headers are
#     reached as <sys/wait.h> through -Ic/apps/libc/include, exactly as C code
#     expects; adding the directory itself also makes them reachable as bare
#     "wait.h", which collides with c/kernel/core/wait.h -- the kernel's wait
#     queues. Combined with (1) the symptom was a kernel file including
#     "wait.h", getting POSIX waitpid instead, and failing with `call to
#     undeclared function 'sched_sleep_ms'` -- on some machines and not others,
#     from identical sources. Nothing includes those headers by bare name; all
#     nine uses in the tree are already <sys/...>.
#
#  3. SECOND OCCURRENCE (2026-08): c/apps/libc/include/sched.h collided with
#     c/kernel/sched/sched.h the exact same way -- but sched.h cannot be
#     reached as <sched.h> through a subdirectory the way <sys/wait.h> is
#     (POSIX requires it be the bare name), so the (2) fix does not apply
#     directly. It now lives in c/apps/libc/include/uonly/, filtered out of
#     this flat scan below and added back ONLY to UCFLAGS (userland), so the
#     kernel's own sched.h is the only one any kernel file's bare
#     `#include "sched.h"` can ever see. Put any FUTURE userland-only header
#     whose basename collides with a kernel header here, not at the top level
#     of c/apps/libc/include -- see UCFLAGS below for the matching -I.
INCDIRS := $(addprefix -I,$(filter-out %/include/sys %/include/uonly,$(sort $(shell find c include -type d))))
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
#   make NOSHAPE=1 build the text layer WITHOUT shaping -- one glyph per code
#                  point straight out of cmap, advances summed, no GSUB and no
#                  GPOS. The NEGATIVE CONTROL for tests/qmp/qmp_shape.py: build
#                  with this and the on-device Arabic assertion fails, because
#                  the letters stop joining and every word gets wider. The same
#                  define drives the host differential (make test-shape-negctl).
ifeq ($(NOSHAPE),1)
CFLAGS += -DSHAPE_NEGATIVE_CONTROL
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
# -Ic/apps/libc/include/uonly: userland-only headers that would shadow a
# kernel header of the same bare name if they sat in the flat INCDIRS scan
# (today: <sched.h> vs c/kernel/sched/sched.h) -- see the INCDIRS comment
# above. CFLAGS (kernel) deliberately does NOT get this -I. IT MUST COME
# BEFORE $(INCDIRS): clang's angle-bracket search is left-to-right priority
# order, and INCDIRS already contains -Ic/kernel/sched -- appended after it,
# this -I would never be reached (sched.c would silently get the KERNEL's
# sched.h instead, which is exactly what happened the first time this line
# was written with the order the other way around).
UCFLAGS := --target=$(ARCH)-elf -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mno-mmx -msse -msse2 -DNDEBUG \
           -std=c11 -Wall -Wextra -O2 -MMD -MP -Ic/apps/libc/include/uonly $(INCDIRS)

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
RING3_NET := c/net/http/cookies.c c/net/http/http1.c c/net/http/hpool.c \
             c/net/http/hpack.c c/net/http/http2.c
#
# c/lib/gfx -- Open Logit, the 2D rendering engine -- is excluded for the same
# reason. It is a RING-3 library: the widget toolkit and the browser's painter
# link it, and both run in ring 3, which is where the one per-pixel-alpha
# primitive (SYS_GUI_BLIT) is reachable. Nothing in the kernel fills a path --
# fb.c's shapes are hard-edged by design and its ONE antialiased primitive is
# the M14 glyph rasterizer, a different and much narrower job. Linking the
# engine into the kernel would put a rasterizer for attacker-shaped geometry
# (a page's border-radius, an SVG's path data) at the highest privilege in the
# machine, under the BKL, to serve no ring-0 caller. See GFX_OBJ below for who
# does link it.
#
# c/lib/audio is excluded for exactly the reason c/lib/video is. An image is
# decoded once, so the image codecs can sit behind SYS_IMG_DECODE; audio is
# decoded continuously for the length of a track, holds hundreds of kilobytes
# of live state per stream, and allocates with malloc/free -- names that do not
# exist in the kernel. In ring 0 it would hold the BKL for the duration of
# playback and put a parser of attacker-chosen bytes at the highest privilege
# in the machine. See the AUD_OBJ rules further down for who does link it.
#
# c/lib/media is excluded for the same reason, and more so. It is the container
# demuxers (ISO-BMFF/MP4 including fragmented, and Matroska/WebM). A container
# is the most attacker-shaped input in this system: it comes off the network,
# and it is a tree of nested lengths -- every one of them written by a stranger
# -- that a parser walks with a pointer, turning those numbers into allocation
# sizes and raw file offsets. It allocates continuously with malloc/realloc/free
# as it builds a sample index. Putting that in ring 0 under the BKL is not a
# thing to do. See MED_OBJ in tests/demux.mk for who does link it.
C_SRC   := $(filter-out c/lib/image/inflate.c c/lib/image/png.c $(wildcard c/lib/video/*.c) $(wildcard c/lib/audio/*.c) $(wildcard c/lib/media/*.c) $(wildcard c/lib/gfx/*.c) $(RING3_NET),$(shell find c/kernel c/drivers c/lib c/fs c/net c/crypto -name '*.c'))
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

.PHONY: test-img test-img-still test-img-anim test-img-exif test-img-fuzz test-img-fuzz-negctl test-imgcheck
.PHONY: test-fs test-fs-boot probe-webapi test-platform test-platform-control test-platform-asan test-platform-page test-platform-page-control test-webapi test-webapi-asan test-webapi-page test-webapi-page-control test-fetch-ui all run shot debug test test-durability test-barrier test-fscrash test-hugefile test-fsreplay test-fs-cache test-fs-journal test-fs-crash test-fsck test-fs-format test-fs-host test-fsmount test-h264 test-h264-units test-h264-diff test-browser test-css-asan test-css-fidelity test-nvme test-part test-part-asan test-ahci test-ahci-raw test-ahci-mbr test-ahci-gpt test-ahci-two test-selfhost test-selfhost-lex test-selfhost-compile test-selfhost-fixpoint clean test-as test-as-gcstress test-as-stress test-as-asan test-as-fast check-asops check-abi test-as-bcstable test-shell test-video test-evq test-clock test-input test-html5lib test-html5lib-tok test-html5lib-asan test-js-dom-asan test-live-page test-as-os test-smp test-net test-net-os test-sock test-sock-ui test-tcp-host test-tcp-negctl test-net-proto test-ip6 test-ip6-dns test-ip6-dns-negctl test-ip6-host test-ip6-negctl test-nd-host test-nd-negctl test-ip6-fallback test-ip6-fallback-negctl test-ip6-os test-dhcp-host test-dhcp-os test-https-smoke test-browser-https test-complete test-libc test-fb-clip test-kheap test-malloc test-png test-jpeg test-svg test-crypto test-crypto-diff test-tls-interop test-tls-resume-control test-p521 test-p521-control test-tls-psk test-tls-psk-control test-libc-diff test-x509-fuzz test-http-fuzz test-font test-font-otl test-font-color test-font-fuzz test-font-control test-h2 test-h2-fuzz test-h2-control test-h2-os check-ring3-net test-modules test-handshakes test-time-host test-time-negctl test-time test-time-smp test-klog test-klog-control test-panic test-panic-log test-stream test-stream-control test-stream-asan test-cookie-cors test-cookie-cors-asan test-sse-page test-sse-page-control

.PHONY: test-aui-mask test-aui test-aui-negctl bench-aui
.PHONY: test-monitor test-monitor-negctl
.PHONY: test-mm test-mm-os test-swap test-swap-negctl test-leak test-leak-os

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
# --- Open Logit: the 2D rendering engine (c/lib/gfx) ------------------------
# Paths (line/quad/cubic), a scanline coverage rasterizer, both fill rules, an
# affine transform, four paints and src-over compositing. RING 3 (see the
# C_SRC note above), compiled once with UCFLAGS and linked into everything
# that draws a shape: every GUI app through aui, plus the browser's painter.
# It links no libc -- clock.aex is crt0 + aui + this and nothing else -- so it
# has to stay that way; a stray memset would break six apps at link time.
GFX_SRC := $(sort $(wildcard c/lib/gfx/*.c))
GFX_OBJ := $(patsubst c/lib/gfx/%.c,$(BUILD)/apps/gfx_%.o,$(GFX_SRC))
$(BUILD)/apps/gfx_%.o: c/lib/gfx/%.c c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $< -o $@

# the aui widget toolkit (immediate-mode), compiled once + linked into every GUI app
$(BUILD)/apps/aui.o: $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $(GUIDIR)/aui.c -o $@

define APP_RULE
$(BUILD)/$(1).elf: $(GUIDIR)/$(1).c $(APPDIR)/crt0.asm $(APPDIR)/logit.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(GFX_OBJ)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/$(1).crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/$(1).c -o $(BUILD)/apps/$(1).o
	$(LD) -nostdlib -e _start -Ttext=$(strip $(2)) -o $$@ $(BUILD)/apps/$(1).crt0.o $(BUILD)/apps/$(1).o $(BUILD)/apps/aui.o $(GFX_OBJ)
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ '$(3)' $(4) '$(5)' $(6) $(7) $(8)
endef

#                     name      base       display  ext icon r   g   b   ('-' ext = none)
$(eval $(call APP_RULE,clock,   0x40000000,Clock,-,C,100,160,255))
$(eval $(call APP_RULE,textedit,0x41000000,TextEdit,txt,T,90,200,120))
$(eval $(call APP_RULE,monitor, 0x42000000,Monitor,-,M,255,100,100))
# Terminal is NOT built by APP_RULE any more -- it links the H.264/H.265
# decoders and mini-libc so a video frame can play inside the scrollback. Its
# rule lives with the VID_OBJ definitions further down, next to Preview's.
$(eval $(call APP_RULE,widgets, 0x46000000,Widgets,-,W,150,120,230))
$(eval $(call APP_RULE,files,   0x47000000,Finder,-,F,120,190,140))
# Gallery: every aui widget in every state. It is the toolkit's demo AND its
# regression test (tests/qmp/qmp_gallery.py asserts against its pixels), which is
# why it ships on the disk rather than living behind a build flag -- a visual
# test you have to opt into is a visual test nobody runs.
$(eval $(call APP_RULE,gallery, 0x4A000000,Gallery,-,G,120,140,250))
# Settings: the window where the machine's memory of its user is editable.
# Packed AFTER gallery for the same reason gallery is packed after browser --
# see the APPS note below.
$(eval $(call APP_RULE,settings,0x4B000000,Settings,-,S,140,150,165))
# Preview is NOT built by APP_RULE -- it links the H.264 decoder and mini-libc,
# so its rule lives with the VID_OBJ definitions further down.
# Code Studio links the AetherScript completion engine (complete.o) for IntelliSense.
$(BUILD)/apps/complete.o: c/apps/as/complete.c c/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c c/apps/as/complete.c -o $@
$(BUILD)/studio.elf: $(GUIDIR)/studio.c $(APPDIR)/crt0.asm $(APPDIR)/logit.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(GFX_OBJ) $(BUILD)/apps/complete.o c/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/studio.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/studio.c -o $(BUILD)/apps/studio.o -Ic/apps/as
	$(LD) -nostdlib -e _start -Ttext=0x49000000 -o $@ $(BUILD)/apps/studio.crt0.o $(BUILD)/apps/studio.o $(BUILD)/apps/aui.o $(GFX_OBJ) $(BUILD)/apps/complete.o
$(BUILD)/studio.aex: $(BUILD)/studio.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/studio.elf $@ 'Code Studio' as '{' 200 160 250

# browser is multi-file (links QuickJS) -- defined below, not via APP_RULE.
# (Network app removed -- its ping/dns/ifconfig moved to the `net` coreutil.)
APPS := clock textedit monitor terminal widgets files preview studio
# Gallery is packed AFTER browser, not appended to APPS, and that placement is
# load-bearing: the Dock's order is the order the .aex files land in the LogitFS
# root, and tests/qmp/qmp_ui.py's BROWSER_SLOT names browser's index in it.
# Appending to APPS would insert gallery BEFORE browser and silently move the
# icon every existing driver clicks. (NAPPS there still has to go 9 -> 10: the
# dock is centred, so one more app shifts every icon.)
GALLERY_AEX := $(BUILD)/gallery.aex

# Which Activity Monitor goes on the disk. Overridable for the same reason
# BROWSER_AEX is: test-monitor-negctl packs a deliberately crippled build (one
# that ignores the kernel's LOGIT_PROC_PROTECTED flag and aims its kill at a
# pid that does not exist) and requires the SAME assertions to fail against it.
# monitor keeps its slot in APPS -- the Dock's order is the order the .aex files
# land in the LogitFS root, and tests/qmp/qmp_monitor.py's MONITOR_SLOT names
# its index, so it is substituted in place rather than appended.
MONITOR_AEX := $(BUILD)/monitor.aex
# Settings goes AFTER gallery for the identical reason, one slot further along:
# appending `settings` to APPS would insert it before browser AND before
# gallery, moving two icons that two existing QMP drivers click by index.
# Packed last, BROWSER_SLOT stays 8 and GALLERY_SLOT stays 9; the new app is
# SETTINGS_SLOT 10 and NAPPS goes 10 -> 11 in tests/qmp/qmp_ui.py, which every
# driver recomputes its x from because the dock is centred.
SETTINGS_AEX := $(BUILD)/settings.aex

# --- CLI programs (sh + coreutils): exec'able ring-3 programs, all linked at a
# common base inside the private user region (0x40000000..0x7FFFFFFF). They are
# packed under /bin (not scanned by the Dock) and launched via fork+execve. ---
define CLI_RULE
$(BUILD)/$(1).elf: $(CLIDIR)/$(1).c $(APPDIR)/crt0_cli.asm $(APPDIR)/clib.h $(CLIDIR)/logit_rich.h $(CLIDIR)/logit_sniff.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$(CC) $(UCFLAGS) -c $(CLIDIR)/$(1).c -o $(BUILD)/apps/$(1).cli.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $$@ $(BUILD)/apps/$(1).crt0c.o $(BUILD)/apps/$(1).cli.o
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
endef

CLI := sh echo ls cat pwd wc head true false sleep mkdir rm touch clear uname net cp mv smptest socktest \
       show dir chart prog clip notify
$(foreach c,$(CLI),$(eval $(call CLI_RULE,$(c))))
CLI_AEX := $(foreach c,$(CLI),$(BUILD)/$(c).aex)

AEX  := $(foreach a,$(APPS),$(BUILD)/$(a).aex) $(BUILD)/browser.aex $(GALLERY_AEX) $(SETTINGS_AEX) $(CLI_AEX) $(BUILD)/as.aex
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
                c/apps/browser/tabs.c \
                c/apps/browser/css_vars.c c/apps/browser/css_extra.c c/net/http/url.c \
                c/net/http/http1.c c/net/http/hpool.c c/net/http/cookies.c \
                c/net/http/http2.c c/net/http/hpack.c \
                c/lib/image/gif.c c/lib/image/jpeg.c c/lib/image/svg.c \
                c/lib/image/exif.c c/lib/image/img.c
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

$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

# The browser's heap. The shared libc arena (24 MiB, sized as a JS heap) ran dry
# while LibCSS parsed github.com's ~3 MiB of stylesheets: malloc returned NULL
# mid-sheet, the tail of the CSS was silently dropped and the page rendered
# unstyled. That was answered by giving the browser its own malloc with a 96 MiB
# arena -- and, because the arena was a .bss array and elf_load commits every
# page of p_memsz, by making the machine hold 96 MiB of RAM for it from launch,
# against a heap that actually peaks in the single-digit MiB on most pages.
#
# The arena is now SYS_MMAP'd, so these two numbers mean different things and
# only the second one costs memory:
#   ARENA_SIZE    address space reserved. Free until touched, so it is set by
#                 what a page could conceivably need, not by what RAM allows.
#                 The kernel's mmap window (MM_MMAP_BASE..MM_MMAP_TOP) is
#                 496 MiB, so this is most of it and still leaves room.
#   ARENA_COMMIT  the ceiling on what may actually be occupied. On a 512 MiB
#                 machine the binding constraint is not this number but the live
#                 free-RAM check in malloc.c (ARENA_RAM_RESERVE); this is the
#                 backstop for a machine large enough that RAM never bites.
# Every other app keeps the 24 MiB default, which is now also 24 MiB of reserved
# address space rather than 24 MiB of resident RAM.
$(BUILD)/browserobj/malloc_big.o: c/apps/libc/src/malloc.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DARENA_SIZE=402653184u -DARENA_COMMIT=335544320u -c $< -o $@

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
R3NET_HDRS := c/net/http/http1.h c/net/http/cookies.h c/net/http/hpool.h \
              c/net/http/hpack.h c/net/http/http2.h
R3NET_OBJ  := $(patsubst %.c,$(BUILD)/r3netobj/%.o,$(R3NET_SRC))

$(BUILD)/r3netobj/%.o: %.c $(R3NET_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

check-ring3-net: $(R3NET_OBJ)
	@echo "check-ring3-net: http1/cookies/hpool/hpack/http2 build freestanding"

# /bin/h2check -- HTTP/2 against a real server, on the device, with the
# before/after numbers. It links the REAL clients of both protocols (http2.c +
# hpack.c and http1.c + hpool.c) against mini-libc, exactly the way the browser
# will, so the comparison it prints is between two real implementations rather
# than one real one and a strawman. Driven by tests/boot/run-h2-smoke.sh.
$(BUILD)/r3netobj/c/apps/net/h2check.o: c/apps/net/h2check.c $(R3NET_HDRS) $(APPDIR)/logit.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/h2check.elf: $(BUILD)/r3netobj/c/apps/net/h2check.o $(R3NET_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/h2check.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/h2check.crt0c.o $(BUILD)/r3netobj/c/apps/net/h2check.o \
	    $(R3NET_OBJ) $(LIBC_OBJS) --end-group
$(BUILD)/h2check.aex: $(BUILD)/h2check.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/h2check.elf $@ h2check - 'H' 90 140 200

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

# --- image decoders, built for the target ----------------------------------
# The same c/lib/image sources the kernel compiles, built again with the
# USERLAND flags for a ring-3 consumer. /bin/imgcheck is that consumer: it
# decodes the fixtures off LogitFS on the device and prints the same digest the
# host build of the identical source prints, which is what turns "the decoders
# are byte-exact" from a claim about a glibc build on Linux into a claim about
# the machine -- mini-libc's arena allocator, -ffreestanding -msse2, a 32 KiB
# stack, and the x86_64-unknown-none build of the Rust staticlib.
IMGCHK_SRC := c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c \
              c/lib/image/svg.c c/lib/image/exif.c
IMGCHK_OBJ := $(patsubst %.c,$(BUILD)/imgobj/%.o,$(IMGCHK_SRC))

$(BUILD)/imgobj/%.o: %.c c/lib/image/img.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/imgobj/imgcheck.o: tests/unit/imgcheck.c c/lib/image/img.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/imgcheck.elf: $(BUILD)/imgobj/imgcheck.o $(IMGCHK_OBJ) $(RUST_LIB) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/imgcheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/imgcheck.crt0c.o $(BUILD)/imgobj/imgcheck.o $(IMGCHK_OBJ) \
	    $(RUST_LIB) $(LIBC_OBJS) --end-group
$(BUILD)/imgcheck.aex: $(BUILD)/imgcheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/imgcheck.elf $@ imgcheck - 'I' 200 140 90

# --- audio decoders, built for the target --------------------------------
# Same shape and the same reasoning as VID_OBJ above: c/lib/audio is a RING-3
# library (WAV, FLAC, MP3), filtered out of the kernel's C_SRC on purpose, and
# compiled here with the userland flags against mini-libc. It uses no libm --
# every transcendental is a constant in the generated mp3_tables.h and the two
# run-time nonlinearities are computed from scratch -- so it links with nothing
# but LIBC_OBJS.
AUD_SRC  := $(wildcard c/lib/audio/*.c)
AUD_HDRS := $(wildcard c/lib/audio/*.h)
AUD_OBJ  := $(patsubst %.c,$(BUILD)/audobj/%.o,$(AUD_SRC))

$(BUILD)/audobj/%.o: %.c $(AUD_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# /bin/audiocheck -- decodes a file on-device and prints the same CRC32 the
# host build prints, which is what turns "the audio decoders also work on
# LogitOS" into a comparison rather than a claim. FLAC additionally reports its
# own STREAMINFO MD5 check, which needs no reference at all. Driven by
# tests/boot/run-audio-test.sh.
$(BUILD)/audiocheck.elf: $(BUILD)/audobj/c/apps/audio/audiocheck.o $(AUD_OBJ) \
                         $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/audiocheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/audiocheck.crt0c.o \
	    $(BUILD)/audobj/c/apps/audio/audiocheck.o $(AUD_OBJ) $(LIBC_OBJS)
$(BUILD)/audiocheck.aex: $(BUILD)/audiocheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/audiocheck.elf $@ audiocheck - 'A' 150 150 150

# Preview: the windowed half. Same link base and the same GUI crt0 as any other
# app, but it links VID_OBJ + mini-libc instead of going through APP_RULE --
# the browser already proves logit.h and mini-libc coexist in one .aex.
#
# It also links the IMAGE codecs now ($(IMGCHK_OBJ) -- the ring-3 build of
# c/lib/image that /bin/imgcheck already uses -- plus $(RUST_LIB) for PNG/APNG,
# BMP, ICO and WebP). It used to decode stills through SYS_IMG_DECODE, which
# hands back ONE canvas; an animation is a LIST of canvases with a delay each,
# and img_decode_anim is the entry point that returns them. Widening the kernel
# image ABI to carry an animation would have put a per-frame decode of
# attacker-chosen bytes under the big lock; linking the same sources into the
# app is what every other media path here already does.
#
# THE EXTENSION IT CLAIMS IS NOT HOW IT GETS FILES ANY MORE. `h264` is kept so
# nothing that relied on the registry match regresses, but the association now
# runs on content -- see opens_in_preview() in c/kernel/gui/wm.c.
$(BUILD)/preview.elf: $(GUIDIR)/preview.c $(APPDIR)/logit.h $(VID_HDRS) \
                      c/lib/image/img.h c/apps/coreutils/logit_sniff.h \
                      $(BUILD)/apps/crt0.o $(VID_OBJ) $(IMGCHK_OBJ) $(RUST_LIB) \
                      $(LIBC_OBJS)
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) $(PREVIEW_CF) -c $(GUIDIR)/preview.c -o $(BUILD)/apps/preview.o
	$(LD) -nostdlib -e _start -Ttext=0x48000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(BUILD)/apps/preview.o $(VID_OBJ) $(MED_OBJ) \
	    $(AUD_OBJ) $(IMGCHK_OBJ) $(RUST_LIB) $(LIBC_OBJS) --end-group
$(BUILD)/preview.aex: $(BUILD)/preview.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/preview.elf $@ Preview h264 'P' 200 150 110

# Terminal: same shape as Preview, and for the same reason. It links VID_OBJ +
# mini-libc because an RT_T_VIDEO frame names a PATH and the terminal is what
# decodes it -- there is no RGBA-over-the-wire path in LRT/1, and inventing one
# would mean 300 KB per frame through a pipe with a 16 KiB payload limit. The
# decode is ring-3 for the same reason Preview's is: megabytes of live reference
# frames and an untrusted-input parser do not belong under the big lock.
$(BUILD)/terminal.elf: $(GUIDIR)/terminal.c $(APPDIR)/logit.h $(CLIDIR)/logit_rich.h \
                       $(CLIDIR)/logit_sniff.h $(VID_HDRS) $(APPDIR)/crt0.asm \
                       $(BUILD)/apps/aui.o $(GFX_OBJ) $(VID_OBJ) $(LIBC_OBJS)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/terminal.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/terminal.c -o $(BUILD)/apps/terminal.o
	$(LD) -nostdlib -e _start -Ttext=0x43000000 -o $@ --start-group \
	    $(BUILD)/apps/terminal.crt0.o $(BUILD)/apps/terminal.o $(BUILD)/apps/aui.o $(GFX_OBJ) \
	    $(VID_OBJ) $(LIBC_OBJS) --end-group
$(BUILD)/terminal.aex: $(BUILD)/terminal.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/terminal.elf $@ Terminal - '>' 70 80 100

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

# The rich-terminal pixel test needs an image whose colour appears nowhere else
# on the screen. Generated rather than committed: .gitignore excludes *.png, the
# same reason the wallpaper is generated.
$(BUILD)/dot.png: tests/unit/dot_gen.py
	@mkdir -p $(BUILD)
	@python3 tests/unit/dot_gen.py $@ 60 40

# The image fixtures ride on the disk so /bin/imgcheck decodes the SAME bytes
# the host tests do -- a guest-only fixture would compare two different files.
IMG_FIXTURES := $(sort $(wildcard tests/fixtures/image/*))
IMG_FIXTURES_ON_DISK := $(foreach f,$(IMG_FIXTURES),$(f):/media/img/$(notdir $(f)))
$(DISK): $(FS_FILES) $(AS_EXAMPLES) $(AS_LA) $(FONTS) $(FONT_TEXT) $(RELEASE_NOTICES) $(AEX) $(BUILD)/libctest.aex $(BUILD)/vidcheck.aex $(BUILD)/audiocheck.aex $(BUILD)/h2check.aex $(BUILD)/dot.png tools/mkfs.py $(BUILD)/imgcheck.aex $(IMG_FIXTURES)
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(DISK) $(FS_FILES) fsroot/readme.txt:/docs/readme.txt \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(FONT_TEXT):/fonts/text.ttf \
	    LICENSE:/licenses/README.txt LICENSING.md:/licenses/Logit-LICENSING.md \
	    LICENSES/GPL-3.0-or-later.txt:/licenses/GPL-3.0-or-later.txt \
	    LICENSES/MIT.txt:/licenses/MIT.txt THIRD_PARTY.md:/licenses/THIRD_PARTY.md \
	    third_party/fonts/OFL-NotoSansSC.txt:/licenses/fonts/OFL-NotoSansSC.txt \
	    third_party/fonts/OFL-NotoSansMono.txt:/licenses/fonts/OFL-NotoSansMono.txt \
	    third_party/fonts/README.md:/licenses/fonts/SOURCES.md \
	    third_party/fonts/LICENSE-DejaVu.txt:/licenses/fonts/LICENSE-DejaVu.txt \
	    $(foreach a,$(APPS),$(if $(filter monitor,$(a)),$(MONITOR_AEX),$(BUILD)/$(a).aex):$(a).aex) \
	    $(BROWSER_AEX):browser.aex \
	    $(GALLERY_AEX):gallery.aex $(SETTINGS_AEX):settings.aex \
	    $(foreach c,$(CLI),$(BUILD)/$(c).aex:/bin/$(c)) $(BUILD)/as.aex:/bin/as $(BUILD)/libctest.aex:/bin/libctest \
	    $(BUILD)/vidcheck.aex:/bin/vidcheck $(BUILD)/h2check.aex:/bin/h2check \
	    $(BUILD)/audiocheck.aex:/bin/audiocheck \
	    $(BUILD)/imgcheck.aex:/bin/imgcheck \
	    $(IMG_FIXTURES_ON_DISK) \
	    $(JSBENCH_PACK) \
	    tests/fixtures/video/sample.h264:/media/sample.h264 \
	    $(BUILD)/dot.png:/media/dot.png \
	    tests/fixtures/audio/sample.mp3:/media/sample.mp3 \
	    tests/fixtures/audio/sample.flac:/media/sample.flac \
	    tests/fixtures/audio/sample.wav:/media/sample.wav \
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

# --- test-tls-resume-control: the assertion that must FAIL without the fix ---
# Rebuilds the interop client with LOGIT_PSK_BREAK_TRANSCRIPT, which derives
# the resumption_master_secret from the transcript WITHOUT the client Finished.
# That is not a strawman: it is the specific mistake RFC 8446 7.1 invites,
# because every OTHER secret in the schedule really is keyed on the shorter
# transcript. The defect is invisible to every other test in this tree -- the
# handshake completes, the chain verifies, tickets are still issued and stored.
# It is only visible as "resumption silently never happens", which is exactly
# the failure mode the resumption cases exist to catch.
#
# The script INVERTS its verdict under TLS_INTEROP_BREAK, so this target passes
# only when the resumption cases go red.
test-tls-resume-control: $(BUILD)
	@TLS_INTEROP_BREAK=LOGIT_PSK_BREAK_TRANSCRIPT TLS_INTEROP_ONLY=resume \
	  bash tests/unit/run-tls-interop.sh

# --- ECDSA over P-521 -------------------------------------------------------
# Two independent sources, because our own generator agreeing with our own
# verifier would prove only self-consistency:
#   RFC 6979 A.2.7  -- an IETF-published P-521/SHA-512 vector, compiled in.
#   openssl         -- fresh keys and signatures over random messages, made by
#                      tests/unit/p521_gen.sh, which we only get to check.
# Plus the rejections (r=0, s=0, r=n, s=n, tampered r/s, swapped r/s, wrong
# message, off-curve key, (0,0) key, and the same signature offered under the
# wrong curve id) -- a verifier that returns 1 unconditionally passes every
# positive case above and none of these.
test-p521: $(BUILD)
	@bash tests/unit/p521_gen.sh $(BUILD)/p521_vectors.txt 12
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/p521_test tests/unit/ecdsa_p521_test.c \
	  c/crypto/pubkey/ecdsa.c c/crypto/hash/sha384.c -Ic/crypto
	$(BUILD)/p521_test $(BUILD)/p521_vectors.txt

# The control for the P-521 WIRING (the unit test above covers the arithmetic).
# LOGIT_P521_BREAK_FLEN makes tls.c compute the field length as curve/8 rather
# than ceil(curve/8): correct for P-256 and P-384, one byte short for P-521.
# Nothing else in the tree changes behaviour, and the P-521 interop cases must
# go red -- if they do not, they are not testing the curve they name.
test-p521-control: $(BUILD)
	@TLS_INTEROP_BREAK=LOGIT_P521_BREAK_FLEN TLS_INTEROP_ONLY=p521 \
	  bash tests/unit/run-tls-interop.sh

# --- the ticket cache, at the unit level ------------------------------------
# The interop suite CANNOT catch what this catches. Both bugs pinned here were
# invisible to `openssl s_server` -- it resumes happily with an
# obfuscated_ticket_age ten times too small, and it never issues enough tickets
# at once for a one-per-host cache to matter. They showed up against a real
# production server (www.kimi.com: eight tickets per handshake, single-use),
# where two of three pooled connections were refused. The only signal interop
# gets is "resumed or not", and a lenient server resumes either way -- so these
# are asserted on the bytes we emit and the state of the cache instead.
PSK_TEST_SRC := tests/unit/tls_psk_test.c c/net/tls/tls_psk.c \
                c/crypto/hash/sha256.c c/crypto/hash/sha384.c c/crypto/hash/hmac_hkdf.c
PSK_TEST_INC := -Ic/crypto -Ic/net/tls -Ic/drivers/timer -Ic/kernel/core

test-tls-psk: $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -o $(BUILD)/tls_psk_test $(PSK_TEST_SRC) $(PSK_TEST_INC)
	$(BUILD)/tls_psk_test

# Two controls, each restoring one of the two shipped bugs. Both must FAIL.
#   LOGIT_PSK_BREAK_AGE_UNIT    read timer_ticks() as ms (10x too small)
#   LOGIT_PSK_BREAK_SINGLE_USE  leave the offered ticket in the cache
test-tls-psk-control: $(BUILD)
	@for d in LOGIT_PSK_BREAK_AGE_UNIT LOGIT_PSK_BREAK_SINGLE_USE; do \
	   $(CC) -O1 -g -w -D$$d -o $(BUILD)/tls_psk_ctl $(PSK_TEST_SRC) $(PSK_TEST_INC) || exit 1; \
	   if $(BUILD)/tls_psk_ctl > $(BUILD)/tls_psk_ctl.log 2>&1; then \
	     echo "CONTROL FAILED: $$d changed nothing -- these tests prove nothing"; \
	     cat $(BUILD)/tls_psk_ctl.log; exit 1; \
	   else \
	     echo "ok   control $$d was detected"; \
	   fi; \
	 done
	@echo "PASS: ticket-cache negative controls"

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

# VFS path resolution, on the host. Every case in here has been a real CVE
# somewhere: ".." at the root, ".." across a mount point, a looping symlink
# chain, a symlink to an absolute path, a path exactly at the buffer limit, the
# empty path. None of them needs a disk or a boot, and all of them are silent
# in an end-to-end test -- a truncated path is still a valid path, to the wrong
# file. c/fs/vfs_path.c is kept free of kernel headers precisely so the code
# under test here is the code that ships.
# A .PHONY line of its own rather than an entry on the big one at the top: that
# line is being appended to by a dozen lines at once, and a merge conflict in a
# list of names is a poor trade for one line of tidiness.
.PHONY: test-vfs-path test-vfs-path-asan

test-vfs-path:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/vfs_path_test tests/unit/vfs_path_test.c \
	    c/fs/vfs_path.c -Ic/fs
	@$(BUILD)/vfs_path_test

# The same under ASan/UBSan: the walker splices symlink targets into a scratch
# buffer while it is reading from it, which is exactly the shape of bug that
# passes every assertion and corrupts memory.
test-vfs-path-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/vfs_path_asan tests/unit/vfs_path_test.c c/fs/vfs_path.c -Ic/fs
	@UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/vfs_path_asan

# The VFS layer itself: the mount table, permission enforcement, hard and
# symbolic links. Links the REAL c/fs/vfs.c -- only the kernel's synthetic-file
# providers and "the credential of the current process" are stubbed, so the
# code deciding every permission here is the code that decides them on the
# machine. Two filesystems really are mounted and a file really is read from
# each; the device test then proves the same refusals reach a ring-3 process.
VFS_TEST_SRC := c/fs/vfs.c c/fs/vfs_meta.c c/fs/vfs_path.c c/fs/ramfs.c
.PHONY: test-vfs-mount test-vfs-mount-asan test-vfs test-vfs-os

test-vfs-mount:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/vfs_mount_test tests/unit/vfs_mount_test.c \
	    $(VFS_TEST_SRC) -Ic/fs -Ic/kernel/core
	@$(BUILD)/vfs_mount_test

test-vfs-mount-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/vfs_mount_asan tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) \
	    -Ic/fs -Ic/kernel/core
	@UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/vfs_mount_asan

# THE NEGATIVE CONTROL. The same suite against a build where the mode, the
# owner and the group are all stored, all readable back through stat, and never
# once consulted -- which is precisely what an unenforced permission model looks
# like from outside, and the thing this layer has to be distinguishable from.
# Every refusal assertion must fail here. If they do not, they were never
# testing enforcement, and this target failing IS the finding.
.PHONY: test-vfs-negctl
test-vfs-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DVFS_NEGCTL_STORE_ONLY -o $(BUILD)/vfs_negctl \
	    tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) -Ic/fs -Ic/kernel/core
	@if $(BUILD)/vfs_negctl > $(BUILD)/vfs_negctl.log 2>&1; then \
	    echo "CONTROL FAILED: a build that never checks the mode passed the suite"; \
	    exit 1; \
	else \
	    echo "control ok: with the mode stored but never checked, these fail:"; \
	    grep FAIL $(BUILD)/vfs_negctl.log | head -8; \
	    tail -1 $(BUILD)/vfs_negctl.log; \
	fi

# Everything the VFS layer can prove without a machine.
test-vfs: test-vfs-path test-vfs-path-asan test-vfs-mount test-vfs-mount-asan test-vfs-negctl

# The SECOND filesystem, on its own disk. A tiny independent LogitFS image
# built by the same tools/mkfs.py that builds the root one -- the markers live
# in files here rather than in the test's shell script because the serial
# console echoes what is typed, and a marker in a command line would show up in
# the log whether or not anything read it.
$(BUILD)/disk2.img: tools/mkfs.py
	@mkdir -p $(BUILD)/vfs2
	@printf 'VFS_DISK2_OK\n'     > $(BUILD)/vfs2/disk2.txt
	@printf 'VFS_SECRET_BYTES\n' > $(BUILD)/vfs2/secret.txt
	@printf 'VFS_PUBLIC_BYTES\n' > $(BUILD)/vfs2/public.txt
	@printf 'VFS_LINK_BYTES\n'   > $(BUILD)/vfs2/link.txt
	python3 tools/mkfs.py $@ $(BUILD)/vfs2/disk2.txt:/disk2.txt \
	    $(BUILD)/vfs2/secret.txt:/secret.txt $(BUILD)/vfs2/public.txt:/public.txt \
	    $(BUILD)/vfs2/link.txt:/link.txt

# The VFS on the machine: two filesystems on two devices, an unprivileged
# process refused, links, and dup sharing an offset. See the header of
# tests/boot/run-vfs-test.sh for what each assertion is worth.
test-vfs-os: $(ISO) $(DISK) $(BUILD)/disk2.img
	@bash tests/boot/run-vfs-test.sh $(ISO) $(DISK) $(BUILD)/disk2.img

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

# Memory management: physical-frame refcounting and poisoning, the copy-on-write
# and demand-paging fault-decision table, and the VMA/mmap arithmetic. Both
# scripts drive the REAL pmm.c/vmm.c/fault.c/vma.c compiled for the host
# (-DMM_HOSTTEST, with mmhost.h as the only seam), under ASan+UBSan.
#
# The line that wrote these could not add its own targets -- the Makefile was
# being edited by a dozen other lines at the time -- so it left both scripts
# runnable directly and said so. They have been runnable and unrun since.
test-mm:
	@sh tests/unit/mm_run.sh $(BUILD)

test-mm-os: $(ISO) $(DISK)
	@bash tests/boot/run-mm-test.sh $(ISO) $(DISK)

# --- reclaim + swap: surviving a machine that is too small ------------------
# The mechanism that lets the kernel keep running when physical memory runs out:
# find pages nobody is using, get them out of RAM, hand the frames back. The
# host half is inside `test-mm` above (mm_rmap_test + mm_reclaim_test, plus two
# negative-control builds that are REQUIRED to fail). These two are the on-device
# half, and they exist because reclaim that has only ever run on a simulator is
# not reclaim.
#
# There is nothing on this machine that is short of memory -- the full desktop
# peaks at 229 MiB of 511 -- so the pressure is MANUFACTURED: the same kernel and
# the same disk, booted with a fraction of the RAM, running a ring-3 program that
# maps more than physically exists and writes a per-page pattern to all of it.
# Then it reads every page back and requires it to be byte-identical.
#
#   test-swap         with a swap device: the workload must complete, every page
#                     must come back unchanged, and the kernel's own counters
#                     must show reclaim actually ran -- both tiers of it.
#   test-swap-negctl  THE NEGATIVE CONTROL, same everything, no swap device. The
#                     data pages hold data so the free drop-tier cannot take
#                     them; the workload MUST fail. If it passes, the positive
#                     run proved nothing, because the pressure was never real.
#
# Tunable: `make test-swap SWAP_RAM=128 SWAP_SIZE=huge`, and SWAPBUS=ahci to
# exercise the slow polled path instead of NVMe.
SWAP_RAM  ?= 192
SWAP_SIZE ?= mid

test-swap: $(ISO) $(DISK)
	@bash tests/boot/run-swap-test.sh $(ISO) $(DISK) swap $(SWAP_RAM) $(SWAP_SIZE)

test-swap-negctl: $(ISO) $(DISK)
	@bash tests/boot/run-swap-test.sh $(ISO) $(DISK) noswap $(SWAP_RAM) $(SWAP_SIZE)

# --- leak hunting: does opening and closing apps give the memory back? ------
# test-mm/test-mm-os cover fork+exec, and they are clean -- 240 shell commands
# with zero frame drift. The workload they do NOT cover is the one the machine
# is used for: opening and closing windowed apps, which allocates a multi-MB
# .aex load buffer and a cw*ch*4 window surface per launch through the KERNEL
# HEAP. kheap takes frames from the PMM and never gives them back, so a heap
# that cannot reuse what it frees consumes physical memory forever while every
# PMM invariant holds and pmm_audit() stays clean. That is a leak no frame-level
# test can see, and these two are where it is measured.
#
#   test-leak     host, under ASan/UBSan: the real c/kernel/mm/kheap.c driven
#                 through app open/close cycles, asserting the arena stops
#                 growing -- plus TWO negative controls, each a compiled build
#                 with one half of the fix removed, both required to FAIL.
#   test-leak-os  the same question on the machine, driven by the WM's own
#                 churn stress. REQUIRES A CHURN BUILD:
#                     make CHURN=1 && make CHURN=1 build/disk.img && make test-leak-os
#                 (the harness fails loudly, not silently, if the driver is absent)
LEAK_SECS ?= 120

test-leak:
	@sh tests/unit/leak_run.sh $(BUILD)

test-leak-os: $(ISO) $(DISK)
	@bash tests/boot/run-leak-apps.sh $(ISO) $(DISK) $(LEAK_SECS)

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

# ---------------------------------------------------------------------------
# Performance. "It got laggier" is not a number; these targets produce numbers.
#
#   make test-perf         boot the machine N times and print, per metric, a
#                          median and its spread: time to LOGIT_BOOT_OK and to
#                          `desktop live`, a shell round trip, a 2.2 MB read
#                          off logitfs, loading a large ring-3 image, a fetch
#                          of a fixed host-served page, and what a moving
#                          pointer costs concurrent work. Exits 0 whatever the
#                          numbers are -- it reports, it does not judge.
#
#   make test-perf-gate    the same measurement as a gate. Needs an explicit
#                          PERF_METRIC and PERF_THRESHOLD, because a threshold
#                          nobody chose is a threshold nobody will defend:
#                            PERF_METRIC=read_net_ms PERF_THRESHOLD=1400 \
#                              make test-perf-gate
#                          Exit 125 (not 1) if the metric could not be measured
#                          at all -- which is also `git bisect`'s skip code.
#
# Across history, tools/perf/sweep.py drives the same harness over a list of
# commits, and tools/perf/bisect.sh is the `git bisect run` form. Both keep the
# harness OUTSIDE the tree being measured, so a bisect cannot end up measuring
# changes to its own instrument. See tools/perf/README.md.
test-perf: $(ISO) $(DISK)
	@bash tests/boot/run-perf-test.sh $(ISO) $(DISK)

test-perf-gate: $(ISO) $(DISK)
	@bash tests/boot/run-perf-gate.sh $(ISO) $(DISK)

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

# --- LogitFS host tests -----------------------------------------------------
# These compile the REAL c/fs sources against a simulated device (fs_sim.h in
# tests/unit/fsstub) whose defining feature is a volatile write cache: a write
# is accepted but not on media until blk_flush(), and a power cut lands an
# arbitrary subset of what was pending. A stub that wrote straight through would
# make every barrier a no-op and every one of these tests vacuous.
FS_STUB   := -Itests/unit/fsstub -Ic/fs -Ic/drivers/block
FS_CORE   := c/fs/logitfs.c c/fs/bcache.c c/fs/fsck.c c/drivers/block/crc32.c
FS_CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -Wno-type-limits              -fsanitize=address,undefined -fno-omit-frame-pointer

# The buffer cache's contract: reads are served without a device round trip,
# writes are deferred, a dirty buffer evicted is WRITTEN not dropped, and after
# a sync everything written before it is on media and a barrier was issued.
test-fs-cache:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_cache_test tests/unit/fs_cache_test.c c/fs/bcache.c $(FS_STUB)
	@$(BUILD)/fs_cache_test

# The commit record's framing and the replay rules: a complete record replays
# idempotently, a torn one (every possible sector prefix) does not, a record
# standing over a LATER transaction's bodies does not, and a forged target is
# refused. Carries the negative control: the pre-checksum rule accepted that
# stale record, and installing what it pointed at destroyed live data.
test-fs-journal:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_journal_test tests/unit/fs_journal_test.c 	    c/fs/fsck.c c/drivers/block/crc32.c $(FS_STUB)
	@$(BUILD)/fs_journal_test

# Crash injection at EVERY device write of write/mkdir/delete/rename/overwrite,
# three loss patterns each, plus the repeated-clean-boot regression from
# CLAUDE.md and the fsync evidence. After every cut: mounts, fsck-clean,
# bystanders byte-for-byte, interrupted file whole-or-absent, and no block
# handed out twice. `-q` is the fast subset.
test-fs-crash:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_crash_test tests/unit/fs_crash_test.c $(FS_CORE) $(FS_STUB)
	@$(BUILD)/fs_crash_test

# fsck against deliberately damaged images: torn and stale journals, a bitmap
# disagreeing with the inodes in both directions, a directory loop, both kinds
# of bad link count, dangling dirents, a block claimed twice (must REFUSE), an
# unusable superblock, a root that is not a directory. Every case also asserts
# that the files it did not aim at are still byte-for-byte correct afterwards.
test-fsck:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_fsck_test tests/unit/fs_fsck_test.c $(FS_CORE) $(FS_STUB)
	@$(BUILD)/fs_fsck_test

# The on-disk format has a C definition (c/fs/logitfs_fmt.h) and a Python one
# (tools/mkfs.py). This reads the REAL image the kernel boots with the C one and
# asserts every offset, so a field that moved on one side is caught here rather
# than by a kernel reading inodes at the wrong offsets.
test-fs-format: $(DISK)
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_format_test tests/unit/fs_format_test.c 	    c/fs/fsck.c c/drivers/block/crc32.c $(FS_STUB)
	@$(BUILD)/fs_format_test $(DISK)

# "The device was asked fewer times for the same bytes" -- an exact number, on
# the host, against the simulated device's own command counter. Carries the
# NEGATIVE CONTROL for the whole bulk-read change: the COALESCING assertion
# bounds a 900-block read at 14 device commands, which the one-command-per-block
# filesystem this replaced cannot meet and could never meet.
test-bulkread:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_bulkread_test tests/unit/fs_bulkread_test.c $(FS_CORE) $(FS_STUB)
	@$(BUILD)/fs_bulkread_test

# The negative control, and it is a real one: the SAME test binary, linked
# against the whole-file read path as it stood before this change (pulled out of
# git, not a re-implementation of it), which must FAIL. A test that cannot fail
# measures nothing, and the specific thing being proved here is that the
# COALESCING assertion is sensitive to the code it names -- not to the simulated
# device, not to the cache, not to the test's own arithmetic.
#
# NEGCTL_REV is the commit whose logitfs.c reads a block at a time. Only
# inode_read/imap moved, so the old file compiles unchanged against everything
# else in this tree.
NEGCTL_REV ?= b9b33ef
test-bulkread-negctl:
	@mkdir -p $(BUILD)/negctl
	@git show $(NEGCTL_REV):c/fs/logitfs.c > $(BUILD)/negctl/logitfs.c
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/fs_bulkread_negctl tests/unit/fs_bulkread_test.c \
	    $(BUILD)/negctl/logitfs.c c/fs/bcache.c c/fs/fsck.c c/drivers/block/crc32.c $(FS_STUB) -Ic/fs
	@if $(BUILD)/fs_bulkread_negctl > $(BUILD)/negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the pre-change read path PASSED the coalescing test"; \
	    cat $(BUILD)/negctl.log; exit 1; \
	 else \
	    echo "negative control OK -- the $(NEGCTL_REV) read path fails it:"; \
	    grep -E "COALESCING|carried|BYPASS" $(BUILD)/negctl.log || true; \
	 fi

test-fs-host: test-fs-cache test-fs-journal test-fs-crash test-fsck test-fs-format \
              test-bulkread test-bulkread-negctl

# The durability harnesses as ONE name. Each of these was written as its own
# target with a comment explaining that it is slow and therefore belongs to no
# suite -- which is how a durability guarantee ends up never executed. They are
# slow (real QEMU boots, no -snapshot, tens of minutes all told) and that is a
# reason to run them deliberately, not a reason for there to be no way to run
# them all. `make test-fs` is host + boot, i.e. the whole answer to "does the
# disk keep what it is given".
test-fs-boot: test-fsmount test-durability test-fscrash test-fsreplay \
              test-hugefile test-barrier

test-fs: test-fs-host test-fs-boot

# Every mount now runs a read-only fsck, so this boots twice WITHOUT -snapshot
# and demands the kernel's own checker say "clean" both times -- once on the
# image mkfs built, and once on an image the kernel has written to and rebooted.
test-fsmount: $(ISO) $(DISK)
	@bash tests/boot/run-fsmount-test.sh $(ISO) $(DISK)

# The storage-path stopwatch: what opening an app costs, by phase, on the real
# machine. Not an assertion -- a measurement. `make bench-fs BLK=ahci` runs the
# same table against a polled AHCI disk instead of virtio-blk, which is how
# "polled AHCI costs X" becomes a number rather than a worry.
# BENCH_BLK, not BLK: `make test-nvme` already passes BLK=nvme into a recipe's
# environment, and a make variable of the same name is the kind of collision
# that surfaces months later as one harness quietly testing the wrong device.
BENCH_BLK  ?= virtio
BENCH_REPS ?= 5
.PHONY: bench-fs bench-launch test-bulkread test-bulkread-negctl
bench-fs: $(ISO) $(DISK)
	@bash tests/boot/run-fsbench.sh $(ISO) $(DISK) $(BENCH_REPS) $(BENCH_BLK)

# The other half of the launch table: what the phases this line does NOT own
# cost, attributed by kprof's sampler over a real Dock click. No source change
# in c/kernel is needed or made -- that is the entire reason the sampler exists.
bench-launch: $(ISO) $(DISK)
	@bash tests/boot/run-launch-profile.sh $(ISO) $(DISK) $(if $(APP),$(APP),browser) 4

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

# TCP receive throughput over SLIRP. A MEASUREMENT, not a gate: it prints a
# number and always exits 0 if the fetch completed. Not in `make test` -- the
# useful form is running it against two builds of c/net/transport/tcp.c on the
# same host and comparing, and any single number it prints is a TCG number
# (the bottleneck is the host CPU emulating x86, not the link). The script
# documents what SLIRP structurally cannot measure.
test-tcp-throughput: $(ISO) $(DISK)
	@bash tests/boot/run-tcp-throughput.sh $(ISO) $(DISK) $(if $(REPS),$(REPS),5) $(if $(BYTES),$(BYTES),917504)

# Host protocol tests for IPv4 validation/reassembly, UDP checksums, ICMP
# echo matching and error routing, and the DNS waiter's error path.
test-net-proto:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/net_proto_test tests/unit/net_proto_test.c \
		c/net/ip/ip6_addr.c \
		-Ic/net/core -Ic/net/link -Ic/net/ip -Ic/net/transport -Ic/net/dns \
		-Ic/drivers/timer -Ic/kernel/core
	@./$(BUILD)/net_proto_test

# ---- IPv6 -----------------------------------------------------------------
#
# The pure half: address text form (RFC 5952 both directions), classification,
# prefix arithmetic, the RFC 6724 policy table, source selection and
# destination ordering. Driven from a table of cases, because the way an IPv6
# stack fails is not "no packets" -- it is one wrong precedence comparison that
# shows up months later as "sometimes the page does not load".
test-ip6-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/ip6_addr_test \
		tests/unit/ip6_addr_test.c -Ic/net/ip -Ic/net/link
	@./$(BUILD)/ip6_addr_test

# Negative control for the claim the whole dual-stack story rests on: a
# destination with no USABLE source must sort below IPv4. ip6_select_source
# enforces that by refusing a candidate whose scope is smaller than the
# destination's; drop that one line (IP6_NEGCTL_NO_SCOPE_GUARD) and a
# link-local-only host starts sourcing fe80:: for a global destination, so the
# v6 address stops being demoted and the host strands every connection instead
# of falling back. The suite MUST fail without it.
test-ip6-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DIP6_NEGCTL_NO_SCOPE_GUARD \
		-o $(BUILD)/ip6_addr_negctl tests/unit/ip6_addr_test.c -Ic/net/ip -Ic/net/link
	@if ./$(BUILD)/ip6_addr_negctl >$(BUILD)/ip6_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without the source-scope guard"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ip6_negctl.log) checks fail without the source-scope guard"; \
	fi

# ICMPv6 / Neighbour Discovery / DAD / SLAAC, white-box: the test #includes
# ip6.c and nd.c and stubs only eth_send, the clock and the console, so every
# byte asserted here is a byte the kernel would put on the wire.
test-nd-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/nd_test tests/unit/nd_test.c \
		-Ic/net/ip -Ic/net/link -Ic/net/core -Ic/net/transport -Ic/net/dns \
		-Ic/drivers/timer -Ic/kernel/core
	@./$(BUILD)/nd_test

# Negative control for Duplicate Address Detection. The easy way to write DAD
# is to send the probe and then assign the address regardless of who answers --
# which passes every "does it configure an address" test ever written.
# IP6_NEGCTL_NO_DAD makes dad_conflict() ignore a defence; the suite MUST fail.
test-nd-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DIP6_NEGCTL_NO_DAD -o $(BUILD)/nd_negctl \
		tests/unit/nd_test.c -Ic/net/ip -Ic/net/link -Ic/net/core \
		-Ic/net/transport -Ic/net/dns -Ic/drivers/timer -Ic/kernel/core
	@if ./$(BUILD)/nd_negctl >$(BUILD)/nd_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with DAD ignoring a defence"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/nd_negctl.log) checks fail when DAD does not refuse"; \
	fi

# The dual-stack socket state machine (c/net/core/sock.c) driven against a
# model TCP: does a preferred-but-black-holed IPv6 destination actually end up
# fetching over IPv4, and does a v4-only answer behave EXACTLY as it did before
# IPv6 existed (one connection, no race, same order)?
test-ip6-fallback:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/ip6_fallback_test \
		tests/unit/ip6_fallback_test.c -Itests/unit -Ic/net/core -Ic/net/ip \
		-Ic/net/link -Ic/net/transport -Ic/net/dns -Ic/net/tls -Ic/net/http \
		-Ic/drivers/timer -Ic/drivers/char -Ic/kernel/core -Iinclude/abi
	@./$(BUILD)/ip6_fallback_test

# Negative control for the fallback itself: SOCK_NEGCTL_NO_FALLBACK makes a
# failed destination fail the socket instead of advancing to the next one --
# i.e. the pre-IPv6 behaviour, which is exactly what a client that "prefers
# IPv6" and cannot fall back does. The suite MUST fail.
test-ip6-fallback-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DSOCK_NEGCTL_NO_FALLBACK \
		-o $(BUILD)/ip6_fallback_negctl tests/unit/ip6_fallback_test.c \
		-Itests/unit -Ic/net/core -Ic/net/ip -Ic/net/link -Ic/net/transport \
		-Ic/net/dns -Ic/net/tls -Ic/net/http -Ic/drivers/timer -Ic/drivers/char \
		-Ic/kernel/core -Iinclude/abi
	@if ./$(BUILD)/ip6_fallback_negctl >$(BUILD)/ip6_fb_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with no fallback at all"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ip6_fb_negctl.log) checks fail without the fallback"; \
	fi

# The dual-stack resolver: two transactions for one name, merged and ordered.
# Covers the parts a packet capture of a healthy network cannot show -- the
# grace period that stops a filtered AAAA stalling the A answer, the records a
# name is NOT allowed to resolve to (::1, multicast, v4-mapped), the txid and
# source guards, and the byte count on a v4-only wire.
test-ip6-dns:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/ip6_dns_test \
		tests/unit/ip6_dns_test.c -Ic/net/dns -Ic/net/ip -Ic/net/link \
		-Ic/net/core -Ic/net/transport -Ic/drivers/timer -Ic/kernel/core
	@./$(BUILD)/ip6_dns_test

# Negative control for the one claim that protects every existing IPv4 test in
# this tree: on a network with no usable IPv6, not one extra byte goes on the
# wire. IP6_NEGCTL_ALWAYS_AAAA removes the ip6_up() gate; the suite MUST fail.
test-ip6-dns-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DIP6_NEGCTL_ALWAYS_AAAA \
		-o $(BUILD)/ip6_dns_negctl tests/unit/ip6_dns_test.c -Ic/net/dns \
		-Ic/net/ip -Ic/net/link -Ic/net/core -Ic/net/transport \
		-Ic/drivers/timer -Ic/kernel/core
	@if ./$(BUILD)/ip6_dns_negctl >$(BUILD)/ip6_dns_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with AAAA asked for unconditionally"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ip6_dns_negctl.log) checks fail when AAAA is unconditional"; \
	fi

test-ip6: test-ip6-host test-ip6-negctl test-nd-host test-nd-negctl \
          test-ip6-dns test-ip6-dns-negctl \
          test-ip6-fallback test-ip6-fallback-negctl

# On device, against real libslirp: link-local + DAD, a router advertisement,
# SLAAC, a neighbour resolved through NS/NA, and a real 32 KiB HTTP body
# fetched over IPv6 -- each asserted separately, plus the v4-unchanged control.
test-ip6-os: $(ISO) $(DISK)
	@bash tests/boot/run-ip6-test.sh $(ISO) $(DISK)

test-net: test-tcp-host test-tcp-negctl test-net-proto test-dhcp-host test-ip6

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

# The content sniffer (c/apps/coreutils/logit_sniff.h): magic identification for
# every format this OS can be handed, the text-vs-binary judgement, and the
# STREAM GUARD that is what stops `cat /fonts/mono.ttf` painting 2.2 MB of sfnt
# onto the character grid. Includes the case that matters -- a binary whose
# bytes contain the LRT/1 magic -- and the UTF-8 case, because a guard that
# suppressed Chinese would be a worse bug than the one it fixes.
test-sniff:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/sniff_test tests/unit/sniff_test.c \
		-Ic/apps/coreutils -Ic/apps -Iinclude/abi
	@./$(BUILD)/sniff_test

# Negative control: the SAME suite with the guard's latch compiled out
# (-DSNIFF_NEGATIVE_CONTROL, i.e. the old "print whatever arrives" behaviour)
# must FAIL. An assertion that passes either way is not testing the guard.
test-sniff-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DSNIFF_NEGATIVE_CONTROL -o $(BUILD)/sniff_negctl tests/unit/sniff_test.c \
		-Ic/apps/coreutils -Ic/apps -Iinclude/abi
	@if ./$(BUILD)/sniff_negctl >$(BUILD)/sniff_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with the binary guard removed"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/sniff_negctl.log) checks fail without the guard"; \
	fi

test-term-host: test-term-proto test-sh test-sh-negctl test-sniff test-sniff-negctl

# On-device: rich output judged by PIXELS (an image at the right size, a drawn
# progress bar, a ruled table) plus the compatibility claim -- the same commands
# redirected to a file must leave a file with no protocol bytes in it.
test-term-ui: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_rich_term.py $(ISO) $(DISK) $(BUILD)/richterm.ppm

# The older GUI-terminal smoke test. It had no make target at all, which is
# half of why nobody noticed it exited 0 whatever its verdict was; a test that
# nothing runs cannot fail either.
test-term-gui: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_term.py $(ISO) $(DISK) $(BUILD)/term.ppm

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

# The same network, the OTHER client. test-https-smoke drives the kernel's
# blocking SYS_HTTP_GET; the Browser uses bfetch + http1.c in ring 3, and the
# two diverged badly enough that baidu passed the smoke test while the Browser
# could not open it. This boots the machine, drives the real Browser over QMP,
# and asserts the page loaded inside a budget a stalled response cannot fit in
# AND that its bytes reached the pixels. Live Internet, like the smoke test.
test-browser-https: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_browser_https.py $(ISO) $(DISK)

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
                 -Ic/apps/libc/include -Ic/apps/libc/include/uonly -Ic/apps/libc/src -Iinclude/abi
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
# -Ic/lib/gfx: browser_paint.c draws through Open Logit, so every host build
# that links the painter needs the engine's header AND $(GFX_SRC) in its
# source list. The two travel together -- adding the include without the
# sources fails at link with five undefined gfx_* symbols.
BTEST_INC := -Ic/apps/browser -Ic/lib/image -Ic/net/http -Ic/lib/text -Ic/lib/gfx
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
# css_perf_test: that the author sheet is parsed once per SHEET and not once
# per mutation -- asserted with counters rather than times, and paired with the
# stale-stylesheet checks that are the real risk of caching it.
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_perf_test tests/unit/css_perf_test.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/css_perf_test
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/layout_test tests/unit/layout_test.c \
	    c/apps/browser/layout.c $(HTML_PARSER_SRC) c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    $(BUILD)/libcss_host.a
	@$(BUILD)/layout_test
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(BUILD)/paint_test tests/unit/paint_test.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(GFX_SRC) $(HTML_PARSER_SRC) \
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
	    $(IMG_HOST_SRC) \
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

# Web-platform targets: probe-webapi (the global-miss instrument over the
# committed real-page corpus) and test-platform / -control / -asan for
# js_platform.c + js_select.c. Own fragment; see the file.
-include tests/webapi_platform.mk

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

# --- HTTP/2 -------------------------------------------------------------
# test-h2       host: HPACK against the RFC 7541 vectors, then the frame layer
#               and the stream state machine against an in-memory server.
# test-h2-fuzz  the same two modules under ASan+UBSan with -fno-sanitize-recover.
# test-h2-os    on the device, against real HTTP/2 servers on the Internet.
#
# Both host targets are built with the sanitizers ON even for the plain run:
# HPACK is a decompressor whose output is LARGER than its input (a Huffman
# string expands by up to 8/5), so "reserve the encoded length" is a heap
# overflow that produces perfectly correct-looking headers right up until it
# corrupts something else. A silent -O2 pass would not notice.
H2_SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
test-h2:
	@mkdir -p $(BUILD)
	@$(CC) $(H2_SAN) -w $(BTEST_INC) -o $(BUILD)/hpack_test \
	    tests/unit/hpack_test.c c/net/http/hpack.c
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/hpack_test
	@$(CC) $(H2_SAN) -w $(BTEST_INC) -o $(BUILD)/h2_test \
	    tests/unit/h2_test.c c/net/http/http2.c c/net/http/hpack.c c/net/http/hpool.c
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/h2_test
	@echo "test-h2: ALL PASS"

# `make test-h2-fuzz SCALE=300 SEED=0x1234` goes deeper; scale 6 is ~1 s.
test-h2-fuzz:
	@mkdir -p $(BUILD)
	@$(CC) $(H2_SAN) -w $(BTEST_INC) -o $(BUILD)/hpack_fuzz \
	    tests/unit/hpack_fuzz.c c/net/http/hpack.c c/net/http/http2.c
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/hpack_fuzz $(SCALE) $(SEED)

# The negative control for test-h2: the same host suite built against an HPACK
# whose dynamic table evicts the NEWEST entry instead of the oldest. Every
# header still decodes to a real string, every length still checks out, and
# nothing crashes -- the tables simply drift apart from the peer's. If
# test-h2 cannot tell that apart from a correct implementation then it is not
# testing the thing that actually breaks, so this target must FAIL.
test-h2-control:
	@mkdir -p $(BUILD)
	@sed 's/int k = (t->head + t->count - 1) % HPACK_MAX_ENTRIES;/int k = t->head;/' \
	    c/net/http/hpack.c > $(BUILD)/hpack_sabotage.c
	@cmp -s c/net/http/hpack.c $(BUILD)/hpack_sabotage.c && \
	    { echo "test-h2-control: the sabotage did not apply -- the control is vacuous"; exit 1; } || true
	@$(CC) -O1 -g -w $(BTEST_INC) -o $(BUILD)/hpack_test_ctl \
	    tests/unit/hpack_test.c $(BUILD)/hpack_sabotage.c
	@if $(BUILD)/hpack_test_ctl >$(BUILD)/hpack_ctl.log 2>&1; then \
	    echo "test-h2-control: FAIL -- the suite passed with a broken eviction rule"; \
	    exit 1; \
	else \
	    echo "test-h2-control: PASS -- evicting the wrong end is caught:"; \
	    grep -m3 '^FAIL' $(BUILD)/hpack_ctl.log || true; \
	fi

# On-device, against the live Internet. Needs outbound access from the host.
test-h2-os: $(ISO) $(DISK)
	@bash tests/boot/run-h2-smoke.sh $(ISO) $(DISK)

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
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(GFX_SRC) $(HTML_PARSER_SRC) \
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

# --- bench-css: the per-phase cost of a REAL page, host-side ---------------
# "The page repaints slowly" is not actionable until it is attributed. This
# links the real pipeline (dom -> css_engine -> css_vars -> css_extra ->
# layout -> browser_paint, the painter through the tests/unit/painthost
# recorder) over a fixture captured from a live site, and times each phase
# separately with the network removed. See the header of tests/unit/css_bench.c
# for what it does and does not model.
#   make bench-css                      # the deepseek.com fixture
#   make bench-css BENCH_ITERS=25
#   make bench-css BENCH_PAGE=... BENCH_CSS="a.css b.css"
BENCH_PAGE  ?= tests/fixtures/cssperf/deepseek.html
BENCH_CSS   ?= $(wildcard tests/fixtures/cssperf/ds-*.css)
BENCH_ITERS ?= 9
$(BUILD)/css_bench: tests/unit/css_bench.c $(BUILD)/libcss_host.a \
                    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                    c/apps/browser/css_extra.c c/apps/browser/layout.c \
                    c/apps/browser/browser_paint.c $(HTML_PARSER_SRC)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ tests/unit/css_bench.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a

bench-css: $(BUILD)/css_bench
	@$(BUILD)/css_bench --iters=$(BENCH_ITERS) $(BENCH_PAGE) $(BENCH_CSS)

# --- bench-repaint: what a repaint costs ON THE MACHINE ---------------------
# bench-css measures the render phases on the host, where a paint is a recorder
# and a text run is a stub. This boots the OS, serves a fixture carrying
# deepseek.com's real 66 KiB stylesheet, and has the PAGE time itself: a chained
# setTimeout mutates one leaf per tick, so each tick is a separate scoped
# re-style + layout + paint, and Date.now() around them is guest wall-clock for
# a repaint with nothing else in it.
#   make bench-repaint                              # 40 timed repaints
#   make bench-repaint CSSPERF_PAGE=wikipedia.html  # real page, load + picture
# The host is shared, so run it several times and read the median; interleave
# the two builds you are comparing rather than running one set then the other.
.PHONY: bench-repaint
bench-repaint: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_css_repaint.py $(ISO) $(DISK) $(or $(TAG),run)


# Every host image test links the same set: the C decoders that are still C,
# the Rust staticlib (PNG/BMP/ICO/WebP), and the eh_personality shim. One
# variable, because five copies of a source list is five chances for a newly
# added decoder to be missing from the test that would have caught its bug.
IMG_HOST_SRC := c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c \
                c/lib/image/svg.c c/lib/image/exif.c tests/unit/rust_host_shim.c
IMG_HOST_INC := -Ic/lib/image -Ic/kernel/mm

# PNG decoder host test: PIL generates a matrix of cases (colour types, bit depths,
# Adam7, tRNS) as ground truth; our decoder must match byte-for-byte. Needs PIL.
test-png: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/pngtest
	@python3 tests/unit/png_gen.py $(BUILD)/pngtest
	@$(CC) -O2 -o $(BUILD)/png_test tests/unit/png_test.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
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
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@$(BUILD)/jpeg_test $(BUILD)/jpegtest

# SVG rasterizer host test: embedded cases (real GitHub octicon mark path,
# rect/circle/ellipse, g fill inheritance, fill-rule evenodd, opacity, xml
# sniffing) plus truncation/garbage robustness checks. No asset generation.
test-svg: $(RUST_LIB_HOST)
	@$(CC) -O2 -o $(BUILD)/svg_test tests/unit/svg_test.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@$(BUILD)/svg_test

# Still-image decoders that are neither PNG nor JPEG: BMP, ICO and
# WebP-lossless. All three are LOSSLESS, so the bar is byte-for-byte against
# a reference decode of the identical file -- PIL for everything it can read
# (its BMP/ICO readers and its libwebp binding are independent code), and for
# the two variants PIL cannot read (RLE4; 32bpp BMP with a real alpha mask,
# which PIL drops) a trivially-invertible encoder from known source pixels.
# Which reference each case used is printed by the generator.
test-img-still: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/imgstill
	@python3 tests/unit/img_still_gen.py $(BUILD)/imgstill
	@$(CC) -O2 -o $(BUILD)/img_still_test tests/unit/img_still_test.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@$(BUILD)/img_still_test $(BUILD)/imgstill

# Animation: GIF and APNG. "It decoded N frames" is not the assertion -- the
# per-frame DELAY and the DISPOSAL are, because disposal is where every naive
# implementation is wrong and the symptom (frame 3 keeps frame 2's pixels in
# the corner it should have cleared) survives any "it animates" eyeball test.
# Reference: PIL seeks each frame and reports the composited canvas + info.
test-img-anim: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/imganim
	@python3 tests/unit/img_anim_gen.py $(BUILD)/imganim
	@$(CC) -O2 -o $(BUILD)/img_anim_test tests/unit/img_anim_test.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@$(BUILD)/img_anim_test $(BUILD)/imganim

# EXIF orientation. A phone photo is stored in the sensor's frame plus a tag
# saying how to turn it upright; ignoring the tag is wrong on essentially
# every portrait photo. Reference: PIL's ImageOps.exif_transpose, per case,
# for all eight orientation values on a deliberately non-square image.
test-img-exif: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/imgexif
	@python3 tests/unit/img_exif_gen.py $(BUILD)/imgexif
	@$(CC) -O2 -o $(BUILD)/img_exif_test tests/unit/img_exif_test.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@$(BUILD)/img_exif_test $(BUILD)/imgexif

# Fuzz every image decoder under ASan+UBSan with -fno-sanitize-recover=all,
# so undefined behaviour ABORTS instead of printing a line the run then
# reports as clean. Seeds are the generated corpora; the harness mutates,
# truncates and splices them. IMG_FUZZ_ITERS controls the budget.
IMG_FUZZ_ITERS ?= 20000
test-img-fuzz: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/imgstill $(BUILD)/imganim $(BUILD)/imgexif
	@python3 tests/unit/img_still_gen.py $(BUILD)/imgstill >/dev/null
	@python3 tests/unit/img_anim_gen.py $(BUILD)/imganim >/dev/null
	@python3 tests/unit/img_exif_gen.py $(BUILD)/imgexif >/dev/null
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $(BUILD)/img_fuzz tests/unit/img_fuzz.c \
	    $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	    $(BUILD)/img_fuzz $(IMG_FUZZ_ITERS) $(BUILD)/imgstill $(BUILD)/imganim $(BUILD)/imgexif

# The negative control for the fuzz harness ITSELF. A fuzz target that cannot
# fail is a green light wired to nothing, so this compiles the same harness
# with a deliberate bug and requires the run to FAIL.
#
# Two sabotages, because the harness has two halves that fail differently:
#   1  kmalloc hands back a buffer one byte short, so every decoder overruns
#      its own allocation -- ADDRESS sanitizer must abort.
#   2  plain signed overflow -- UNDEFINED-behaviour sanitizer must abort. This
#      is the one that silently passes when -fno-sanitize-recover=all is
#      missing, because UBSan then prints the diagnostic, returns, and the
#      process still exits 0 with the run reported clean.
test-img-fuzz-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/imgstill
	@python3 tests/unit/img_still_gen.py $(BUILD)/imgstill >/dev/null
	@rc=0; for mode in 1 2; do \
	    $(CC) -O1 -g -DIMG_SABOTAGE=$$mode -fsanitize=address,undefined -fno-sanitize-recover=all \
	        -o $(BUILD)/img_fuzz_sab$$mode tests/unit/img_fuzz.c \
	        $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC); \
	    if ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	          $(BUILD)/img_fuzz_sab$$mode 4000 $(BUILD)/imgstill >$(BUILD)/img_fuzz_sab$$mode.log 2>&1; then \
	        echo "NEGATIVE CONTROL FAILED: sabotage $$mode fuzzed CLEAN"; rc=1; \
	    else \
	        echo "negative control $$mode ok: $$(grep -m1 -ohE 'runtime error: [a-z ]+|ERROR: [A-Za-z]+' $(BUILD)/img_fuzz_sab$$mode.log || echo aborted)"; \
	    fi; \
	done; exit $$rc

# Do the image decoders work on LogitOS, not just on the host? The host tests
# above prove them byte-exact against PIL, libwebp and ffmpeg -- of a glibc
# build on Linux. This boots the machine, runs /bin/imgcheck on the fixtures
# packed into the disk image, and requires every digest to equal the one the
# HOST build of the identical source prints for the identical bytes. So
# mini-libc's arena allocator, -ffreestanding -msse2, the 32 KiB stack, LogitFS
# and the x86_64-unknown-none build of the Rust staticlib are all in the loop.
$(BUILD)/imgcheck_host: tests/unit/imgcheck.c $(IMGCHK_SRC) $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -o $@ tests/unit/imgcheck.c $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)

test-imgcheck: $(ISO) $(DISK) $(BUILD)/imgcheck_host
	@bash tests/boot/run-img-test.sh $(ISO) $(DISK) $(BUILD)/imgcheck_host

# Everything above, in one go.
test-img: test-png test-jpeg test-svg test-img-still test-img-anim test-img-exif \
          test-img-fuzz test-img-fuzz-negctl

# ============================================================================
# Font parsing (c/lib/text + the rasterizer). Two references, because a font
# bug does not look like a crash: a subtly wrong charstring interpreter draws a
# letter that is still a letter and still wrong, so "it rendered something"
# proves nothing.
#
#   test-font          every glyph's OUTLINE compared integer-for-integer with
#                      fontTools' own interpreter, plus the rasterized coverage
#                      compared with FreeType to a stated tolerance. This is the
#                      H.264 bar applied to fonts: the path is exactly specified
#                      arithmetic, so any disagreement there is our bug. The
#                      coverage is not exactly specified (FreeType computes exact
#                      area on a 26.6 grid, we sample 16 sub-scanlines on a 24.8
#                      one), so it gets a tolerance and the tolerance is stated.
#   test-font-otl      GSUB/GPOS/GDEF/kern access against fontTools' decompiler.
#                      This API is a shaping line's interface, so the questions
#                      are the ones a shaper asks.
#   test-font-color    COLR/CPAL layers (enumerated AND composited), CBDT/CBLC
#                      and sbix image location.
#   test-font-fuzz     ASan/UBSan over truncated + mutated real fonts, plus named
#                      rejection cases. A font is untrusted input the moment a
#                      page uses @font-face.
#   test-font-control  the NEGATIVE CONTROL. Two crippled builds that must FAIL.
#
# Needs python3 with fontTools and freetype-py (pip install freetype-py). The
# fixtures under tests/fixtures/fonts/ are real, redistributable fonts; see the
# README there for provenance.
FONT_SRC   := c/lib/text/ttf.c c/lib/text/cff.c c/lib/text/otlayout.c \
              c/lib/text/fontcolor.c
FONT_INC   := -Ic/lib/text -Ic/kernel/gui
FONT_FIX   := tests/fixtures/fonts
# The committed fixtures plus the fonts we actually ship on the disk image --
# a regression that only shows up in ui.ttf is still a regression.
FONT_CASES := $(FONT_FIX)/SourceSans3-Regular.otf $(FONT_FIX)/cid-cff-subset.otf \
              $(FONT_FIX)/colr-emoji-subset.ttf $(FONT_FIX)/kern-subset.ttf \
              fsroot/fonts/ui.ttf fsroot/fonts/mono.ttf
FONT_OTL_CASES := $(FONT_FIX)/SourceSans3-Regular.otf $(FONT_FIX)/cid-cff-subset.otf \
                  $(FONT_FIX)/kern-subset.ttf $(FONT_FIX)/colr-emoji-subset.ttf

test-font:
	@mkdir -p $(BUILD)/fontref
	@$(CC) -O2 -w -o $(BUILD)/ttf_test tests/unit/ttf_test.c $(FONT_SRC) $(FONT_INC)
	@$(BUILD)/ttf_test fsroot/fonts/ui.ttf $(FONT_FIX)/SourceSans3-Regular.otf
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/font_cff_test tests/unit/font_cff_test.c \
	    $(FONT_SRC) c/kernel/gui/raster.c $(FONT_INC)
	@rc=0; for f in $(FONT_CASES); do \
	    b=`basename $$f`; \
	    python3 tests/unit/font_ref_gen.py $$f $(BUILD)/fontref/$$b.ref --px 32 --bitmaps 64 \
	        >/dev/null || { echo "ref gen failed for $$f"; exit 1; }; \
	    $(BUILD)/font_cff_test $$f $(BUILD)/fontref/$$b.ref || rc=1; \
	done; exit $$rc

test-font-otl:
	@mkdir -p $(BUILD)/fontref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/font_otl_test tests/unit/font_otl_test.c \
	    $(FONT_SRC) $(FONT_INC)
	@rc=0; for f in $(FONT_OTL_CASES); do \
	    b=`basename $$f`; \
	    python3 tests/unit/font_otl_ref.py $$f $(BUILD)/fontref/$$b.otl 2>/dev/null \
	        || { echo "ref gen failed for $$f"; exit 1; }; \
	    $(BUILD)/font_otl_test $$f $(BUILD)/fontref/$$b.otl || rc=1; \
	done; exit $$rc

# The sbix fixture is SYNTHESISED rather than committed: the only widely used
# sbix font is Apple Color Emoji, which is not redistributable. The generator
# grafts a real sbix table (two strikes of hand-built PNGs plus a 'dupe' record)
# onto kern-subset.ttf, which reaches every branch of sbix_lookup.
test-font-color:
	@mkdir -p $(BUILD)/fontref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/font_color_test tests/unit/font_color_test.c \
	    $(FONT_SRC) c/kernel/gui/raster.c $(FONT_INC)
	@python3 tests/unit/font_color_ref.py --make-sbix $(BUILD)/fontref/sbix-synth.ttf \
	    $(FONT_FIX)/kern-subset.ttf 2>/dev/null
	@rc=0; for f in $(FONT_FIX)/colr-emoji-subset.ttf $(FONT_FIX)/cbdt-emoji-subset.ttf \
	                $(BUILD)/fontref/sbix-synth.ttf; do \
	    b=`basename $$f`; \
	    python3 tests/unit/font_color_ref.py $$f $(BUILD)/fontref/$$b.clr 2>/dev/null \
	        || { echo "ref gen failed for $$f"; exit 1; }; \
	    $(BUILD)/font_color_test $$f $(BUILD)/fontref/$$b.clr --px 48 || rc=1; \
	done; exit $$rc

# Long-running; not part of `make test`. `make test-font-fuzz SCALE=10` goes deeper.
test-font-fuzz:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/font_fuzz tests/unit/font_fuzz.c $(FONT_SRC) c/kernel/gui/raster.c $(FONT_INC)
	@UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/font_fuzz \
	    $(FONT_FIX)/SourceSans3-Regular.otf $(FONT_FIX)/cid-cff-subset.otf \
	    $(FONT_FIX)/colr-emoji-subset.ttf $(FONT_FIX)/cbdt-emoji-subset.ttf \
	    $(FONT_FIX)/kern-subset.ttf fsroot/fonts/mono.ttf

# NEGATIVE CONTROL. Two builds, each with one deliberate bug of the kind this
# suite exists to catch -- a CFF hv/vhcurveto that drops its optional trailing
# coordinate, and a glyf contour that drops the on-curve point implied between
# two off-curve points. Both still draw letters. Both MUST make test-font fail;
# if either passes, the exact path comparison is comparing nothing.
test-font-control:
	@mkdir -p $(BUILD)/fontref
	@python3 tests/unit/font_ref_gen.py $(FONT_FIX)/SourceSans3-Regular.otf \
	    $(BUILD)/fontref/control-cff.ref --px 32 --bitmaps 32 >/dev/null
	@python3 tests/unit/font_ref_gen.py fsroot/fonts/ui.ttf \
	    $(BUILD)/fontref/control-glyf.ref --px 32 --bitmaps 32 >/dev/null
	@$(CC) -O2 -w -DFONT_CONTROL_HV_LAST -o $(BUILD)/font_ctl_cff \
	    tests/unit/font_cff_test.c $(FONT_SRC) c/kernel/gui/raster.c $(FONT_INC)
	@$(CC) -O2 -w -DFONT_CONTROL_NO_MIDPOINT -o $(BUILD)/font_ctl_glyf \
	    tests/unit/font_cff_test.c $(FONT_SRC) c/kernel/gui/raster.c $(FONT_INC)
	@if $(BUILD)/font_ctl_cff $(FONT_FIX)/SourceSans3-Regular.otf \
	       $(BUILD)/fontref/control-cff.ref > $(BUILD)/fontref/ctl-cff.log 2>&1; then \
	    echo "CONTROL FAILED: the crippled CFF interpreter PASSED -- the path comparison is vacuous"; \
	    exit 1; \
	else \
	    echo "control ok: hv/vhcurveto sabotage is caught:"; \
	    grep '^FAIL' $(BUILD)/fontref/ctl-cff.log | head -3; \
	fi
	@if $(BUILD)/font_ctl_glyf fsroot/fonts/ui.ttf \
	       $(BUILD)/fontref/control-glyf.ref > $(BUILD)/fontref/ctl-glyf.log 2>&1; then \
	    echo "CONTROL FAILED: the crippled glyf reader PASSED -- the path comparison is vacuous"; \
	    exit 1; \
	else \
	    echo "control ok: implied-midpoint sabotage is caught:"; \
	    grep '^FAIL' $(BUILD)/fontref/ctl-glyf.log | head -3; \
	fi

# The H.264 decoder gates live in tests/h264.mk (see the note at its top);
# the include sits with the other test-suite includes further down.

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

# Audio CODEC test targets (test-wav/test-flac/test-mp3, test-audio-codec-units/
# -fuzz/-negctl/-os) for the from-scratch decoders in c/lib/audio. A separate
# fragment from tests/audio.mk above: that one is the sound-driver line's PCM
# layer and mixer, this one is the decoders that feed it, and the two are
# developed independently. Same reason, same shape.
-include tests/audio_codec.mk

# USB test targets (test-usb, test-usb-host/ring/desc/hid, test-usb-os,
# test-usb-none, test-usb-negctl). Same reason, same shape. The DRIVER needs no
# Makefile change at all -- C_SRC globs c/drivers and it registers itself
# through the device model's linker section.
-include tests/usb.mk

# Text shaping + bidi test targets (test-bidi, test-shape, test-text and
# their negative controls). Same reason, same shape as the fragments above.
-include tests/text.mk

# H.265/HEVC decoder test targets (test-h265, test-h265-units, test-h265-diff,
# test-video265). Same reason, same shape as the fragments above.
-include tests/h264.mk

-include tests/h265.mk

# Full-system test, the commit gate and the test-liveness audit
# (test-fullsystem, verify-commit, check-test-liveness). Same reason, same
# shape as the fragments above.
-include tests/fullsystem.mk

-include tests/prof.mk

# TLS/crypto performance: the per-phase handshake breakdown, the host
# primitive costs, the gate and its two controls. Own fragment; see the file.
-include tests/tlsperf.mk

# Kernel primitive costs: syscall entry, context switch, frame allocation, page
# fault, and how much of four cores the big kernel lock eats. Own fragment.
-include tests/kbench.mk

# Network measurement: the instrumented Ethernet wire (netwire.py), the paired
# A/B and driver comparisons that run over it, and test-net-rx -- which asserts
# WHERE the receive path runs. Own fragment; start at tests/boot/netwire.py's
# header for why `-netdev user` cannot measure a TCP window.
-include tests/netperf.mk

# The link layer: Ethernet framing/VLAN/loopback and the IPv4 neighbour cache,
# each with a host suite and a negative control that restores the behaviour the
# rewrite replaced. Own fragment for the same reason as the fragments above.
-include tests/link.mk

# The CSS engine corpus audit + fidelity targets (kept out of this file so a
# whole-file Makefile overwrite from a concurrent line cannot delete them).
-include tests/cssweb.mk

# The JavaScript engine's measurement + language-coverage targets (bench-js,
# bench-js-os, test-js-syntax and its negative control). Own fragment for the
# same reason as the others: a whole-file Makefile overwrite from a concurrent
# line cannot delete it. It also defines $(JSBENCH_PACK), used in the $(DISK)
# recipe above.
-include tests/jsperf.mk

# Container demuxer test targets (test-demux and its parts: -units, -diff,
# -lacing, -fuzz, -negctl, test-avsync, test-demux-os) plus MED_OBJ and
# /bin/demuxcheck. Own fragment for the same reason as the others: this tree is
# worked on by several people at once and a whole-file Makefile edit from a
# concurrent line cannot delete it.
-include tests/demux.mk
-include tests/mse.mk

# Preview's on-device format gate (test-preview, test-preview-timing,
# test-preview-negctl) and the two media fixtures it puts on the disk. Own
# fragment for the same reason as the others -- and it must come AFTER
# tests/demux.mk, which is where $(MED_OBJ) is defined.
-include tests/preview.mk

# Ring-3 memory protection: W^X, NX, SMEP/SMAP and syscall pointer validation,
# plus /bin/secprobe -- the hostile ring-3 program the gate drives. Own fragment
# for the same reason as the others: a whole-file Makefile overwrite from a
# concurrent line cannot delete it.
-include tests/sec.mk
# The settings store: does this machine remember anything about its user?
-include tests/settings.mk

# The browser LOADER test (test-loader), its negative control and the on-device
# test-script-nav. Own fragment for the same reason as every other one above --
# and this one learned it the hard way: written straight into this file, the
# targets were deleted by a whole-file overwrite from a concurrent line three
# times in one afternoon, once by me.
-include tests/loader.mk

# The ring-3 heap's COST: test-arena (the .bss/commit-bound gate plus its two
# negative controls) and bench-arena (per-page heap over the cssweb corpus).
# Own fragment for the same reason as every other one above.
-include tests/mem.mk

# --- aui widget toolkit ------------------------------------------------------
#
# The coverage rasterizer is pure integer arithmetic over a byte buffer, so it
# is checked EXACTLY on the host against a 16x supersampled reference -- in
# milliseconds, without booting anything. That is the half of "are the shapes
# right" that is cheap to get wrong and cheap to check.
test-aui-mask:
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -Ic/apps/gui -Ic/apps -Ic/lib/gfx -Iinclude -Iinclude/abi \
	    -o $(BUILD)/aui_mask_test tests/unit/aui_mask_test.c $(GFX_SRC) -lm
	$(BUILD)/aui_mask_test

# --- the aui gallery is the toolkit's visual regression test -----------------
#
# A toolkit change is a VISUAL change, so it is asserted against pixels: the
# driver boots the desktop, opens Gallery, and measures the frame -- corner
# anti-aliasing as a count of distinct tones along an arc, hover as a colour
# that moves when the pointer does, focus as a ring that Tab relocates, a shadow
# as a luminance ramp under a card.
#
# test-aui-negctl is the NEGATIVE CONTROL and it is meant to fail: it rebuilds
# the gallery with -DAUI_NO_AA, which routes every rounded shape back through
# the kernel's hard-edged SYS_GUI_RRECT and drops the alpha and shadow paths.
# The same assertions then fail, which is how "these corners are really
# anti-aliased" is demonstrated rather than asserted. The target succeeds when
# the test fails.
test-aui: $(ISO) $(DISK)
	bash tests/boot/run-aui-test.sh $(ISO) $(DISK)

$(BUILD)/apps/aui_noaa.o: $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -DAUI_NO_AA -c $(GUIDIR)/aui.c -o $@

$(BUILD)/gallery_noaa.elf: $(GUIDIR)/gallery.c $(APPDIR)/crt0.asm $(APPDIR)/logit.h \
                           $(GUIDIR)/aui.h $(BUILD)/apps/aui_noaa.o $(GFX_OBJ)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/gallery_noaa.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/gallery.c -o $(BUILD)/apps/gallery_noaa.o
	$(LD) -nostdlib -e _start -Ttext=0x4A000000 -o $@ $(BUILD)/apps/gallery_noaa.crt0.o \
	    $(BUILD)/apps/gallery_noaa.o $(BUILD)/apps/aui_noaa.o $(GFX_OBJ)
$(BUILD)/gallery_noaa.aex: $(BUILD)/gallery_noaa.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/gallery_noaa.elf $@ 'Gallery' - 'G' 120 140 250

# --- the Activity Monitor: the task manager, and its kill ---------------------
#
# A monitor you cannot act from is a poster, so the assertion that matters is
# that a process SELECTED IN THE TABLE actually dies. qmp_monitor.py requires
# that from three independent places -- the kernel's serial log, the Monitor's
# own table (which is SYS_PROCS, i.e. the PCB table), and the pixels where the
# victim's window used to be -- because any one of them can go quiet while the
# feature is broken.
#
# It also asserts the REFUSAL: the console shell (init) is the one process
# SYS_KILL declines, and the Force Quit button must be greyed out for it BEFORE
# the click, driven by the LOGIT_PROC_PROTECTED flag the kernel publishes rather
# than by a rule the app re-derives.
test-monitor: $(ISO) $(DISK)
	python3 tests/qmp/qmp_monitor.py $(ISO) $(DISK) --out $(BUILD)/monitor.png

# The NEGATIVE CONTROL, and it is meant to fail. -DMONITOR_NEGCTL builds the
# app so that it (a) ignores LOGIT_PROC_PROTECTED, lighting the button up for
# the console shell, and (b) aims its kill at pid 0. Nothing dies and nothing is
# refused, so the refusal assertion and all three "it is gone" assertions fail.
# The target succeeds when the test fails.
$(BUILD)/monitor_negctl.elf: $(GUIDIR)/monitor.c $(APPDIR)/crt0.asm $(APPDIR)/logit.h \
                             $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(GFX_OBJ)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/monitor_negctl.crt0.o
	$(CC) $(UCFLAGS) -DMONITOR_NEGCTL -c $(GUIDIR)/monitor.c -o $(BUILD)/apps/monitor_negctl.o
	$(LD) -nostdlib -e _start -Ttext=0x42000000 -o $@ $(BUILD)/apps/monitor_negctl.crt0.o \
	    $(BUILD)/apps/monitor_negctl.o $(BUILD)/apps/aui.o $(GFX_OBJ)
$(BUILD)/monitor_negctl.aex: $(BUILD)/monitor_negctl.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/monitor_negctl.elf $@ 'Monitor' - 'M' 255 100 100

# $(DISK)'s prerequisite list names $(BUILD)/monitor.aex (via APPS), not
# $(MONITOR_AEX), so the substitute has to be built as its own goal first --
# otherwise mkfs.py is handed a path that was never made.
test-monitor-negctl: $(ISO) $(BUILD)/monitor_negctl.aex
	$(MAKE) DISK=$(BUILD)/disk_monneg.img MONITOR_AEX=$(BUILD)/monitor_negctl.aex \
	    $(BUILD)/disk_monneg.img
	@echo "--- negative control: the SAME assertions against a crippled Monitor ---"
	@if python3 tests/qmp/qmp_monitor.py $(ISO) $(BUILD)/disk_monneg.img \
	    --out $(BUILD)/monitor_negctl.png; then \
	    echo "NEGATIVE CONTROL FAILED: the crippled build passed the assertions"; exit 1; \
	else \
	    echo "negative control OK: the crippled build fails the assertions"; \
	fi

test-aui-negctl: $(ISO)
	$(MAKE) DISK=$(BUILD)/disk_noaa.img GALLERY_AEX=$(BUILD)/gallery_noaa.aex $(BUILD)/disk_noaa.img
	@echo "--- negative control: the SAME assertions against a hard-edged build ---"
	@if bash tests/boot/run-aui-test.sh $(ISO) $(BUILD)/disk_noaa.img; then \
	    echo "NEGATIVE CONTROL FAILED: the AA assertions passed without the rasterizer"; exit 1; \
	else \
	    echo "negative control ok: without aui's rasterizer the gallery fails its own test"; \
	fi

# What one aui frame costs, measured on the machine at 1920x1200 under TCG
# rather than estimated: Gallery times its own repaint with CLOCK_MONOTONIC and
# prints it on the serial console.
bench-aui: $(ISO) $(DISK)
	bash tests/boot/run-aui-bench.sh $(ISO) $(DISK)

# Open Logit, the 2D rendering engine: accuracy against a 16x supersampled
# reference, compositing against the Porter-Duff formula in double, the
# negative control, and the cost. Own fragment for the same reason as every
# other one above.
-include tests/gfx.mk

# The CI this repository did not have: `make test-audit` (tests that cannot
# fail) and `make ci` (build from a clean clone of HEAD, then the suites). Own
# fragment for the same reason as every other one above.
-include tests/ci.mk

# The clipboard (test-clip + its two negative controls) and notifications
# (test-notify, test-notify-negctl, test-notify-cost). Own fragment for the same
# reason as every other one above.
-include tests/clip.mk

# Is a half-drawn window ever on screen? (test-flash + its negative control).
# A window has ONE canvas, so an app that runs ahead of the compositor gets its
# erased canvas composited -- the "one click and it does a reload-refresh" the
# machine's user reported. Own fragment for the same reason as every other one
# above.
-include tests/flash.mk

# The framework corpus (React/Next/Vue/Angular/Svelte/Vite/webpack): which
# framework runtimes this browser cannot run, ranked by cause. Own fragment,
# see the header of tests/frameworks.mk.
-include tests/frameworks.mk

-include tests/http2.mk

# mini-libc -> real libc gates (fnmatch/glob/regex/inet/pwd/grp/uname/mman/
# sched/poll/select/resource/syslog/termios/netdb/socket). See its header.
-include tests/libc.mk
