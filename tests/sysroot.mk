# tests/sysroot.mk -- the sysroot: what a C compiler on the device needs from
# the disk, built, packed onto a test image, and gated.
#
# In its own fragment for the reason tests/libc.mk, tests/libm.mk and
# tests/fsgeom.mk each give: the Makefile is contended by several lines at once.
# It is NOT yet `-include`d from the Makefile (that file is owned elsewhere
# this week); until that one line lands, run it as
#
#     make -f Makefile -f tests/sysroot.mk test-sysroot
#
# which reads the Makefile and then this file, exactly as `-include` would.
#
# WHAT A COMPILER NEEDS, and where it is in this tree (measured 2026-08-21):
#   c/apps/libc/include/**   64 headers; uonly/sched.h sits apart only because
#                            the KERNEL has a sched.h too (CLAUDE.md, Source
#                            layout) -- on the disk it is plain /usr/include/sched.h
#   $(LIBC_OBJS) $(LIBM_OBJ) the objects; no archive of them existed
#   c/apps/crt0_cli.asm      the CLI startup every /bin program links
#   third_party/tcc/include  tcc's own stddef/stdarg/stdbool/float/varargs:
#                            tcc's va_arg is a call into libtcc1, so it must
#                            see ITS stdarg.h, not a libc's
#   third_party/tcc/lib      libtcc1.c va_list.c alloca86_64.S are x86-64;
#                            alloca86.S alloca86-bt.S are i386; alloca-arm.S
#                            armeabi.c armflush.c lib-arm64.c are ARM;
#                            alloca86_64-bt.S + bcheck.c are the -b bounds
#                            checker and bcheck.c needs a hosted libc
#                            (<malloc.h>, signal, fprintf) -- all excluded
#
# tcc's paths (third_party/tcc/tcc.h:178-222, libtcc.c:974, tccelf.c:1192):
#   headers    {B}/include : /usr/local/include : /usr/include
#   libraries  /usr/lib : /lib : /usr/local/lib          (-lc -> libc.a)
#   crt        /usr/lib                                  (crt1.o crti.o crtn.o)
#   libtcc1    {B}/libtcc1.a     where {B} = CONFIG_TCCDIR = /usr/lib/tcc
# The layout below is exactly that. tools/mksysroot.py says why each name.
#
# THE HOST TCC. Every gate here runs the SAME tcc source that becomes tcc.aex,
# built natively with clang in one command, with CONFIG_SYSROOT compiled in as
# $(abspath build/sysroot): its default search paths are then the packed
# layout and nothing else, so "tcc hello.c -o hello" with NO flags on the host
# exercises what the device user will type. third_party/tcc is not configured
# or built in place (it belongs to another line); the one header configure
# would have generated, config.h, is a single #define written beside the
# binary. A sysroot that needed the compiler it serves to be built first would
# be circular, which is why libtcc1 is compiled by clang with UCFLAGS.
#
#   make -f Makefile -f tests/sysroot.mk sysroot               build/sysroot/
#   ...  test-sysroot-tree       mkfs.py add_tree: round trip byte-for-byte,
#                                determinism (+ its negative control), refusals
#   ...  test-sysroot-headers    every header parses under tcc, both search
#                                orders; use-site probes vs an expectation
#                                table; generated stdint.h/limits.h vs clang's
#   ...  test-sysroot-link       tcc links hello against the sysroot ==
#                                byte-identical to the build tree's objects;
#                                NEGATIVE CONTROL: one member dropped from
#                                libc.a fails on that symbol by name
#   ...  test-sysroot-image      the Makefile's disk + the sysroot at / +
#                                /bin/hello-tcc; fsck clean; every sysroot
#                                file on the image byte-for-byte
#   ...  test-sysroot            all four (host, no QEMU)
#   ...  test-sysroot-os         boots the image, runs /bin/hello-tcc, the
#                                tcc-LINKED program, and greps its output
.PHONY: sysroot test-sysroot test-sysroot-tree test-sysroot-headers \
        test-sysroot-link test-sysroot-image test-sysroot-os

SYSROOT        := $(BUILD)/sysroot
SYSROOT_WORK   := $(BUILD)/sysroot-work
SYSROOT_ABS    := $(abspath $(SYSROOT))
# llvm-ar is asked for by name because its deterministic mode and its GNU
# index are what tcc's loader reads; the versioned names are what Ubuntu
# installs. GNU ar is the fallback (it also writes a GNU "/" index).
SYSROOT_AR     := $(firstword $(shell which llvm-ar llvm-ar-21 llvm-ar-20 llvm-ar-19 llvm-ar-18 2>/dev/null) ar)
SYSROOT_NM     := $(firstword $(shell which llvm-nm llvm-nm-21 llvm-nm-20 llvm-nm-19 llvm-nm-18 2>/dev/null) nm)
SYSROOT_TCCLIVE := third_party/tcc
# The SNAPSHOT of third_party/tcc everything below reads -- see
# tests/unit/sysroot_tccsnap.sh for why the live tree is not read directly
# (it is untracked and being patched in place by tests/tcc.mk's line; it
# stopped compiling under this fragment forty minutes after it first did).
SYSROOT_TCCSRC  := $(SYSROOT_WORK)/tccsrc
SYSROOT_HOSTTCC := $(SYSROOT_WORK)/hosttcc/tcc
# See tools/mksysroot.py ("THE ARCHIVES ARE SCANNED ONCE EACH"): libtcc1's
# objects are also members of libc.a, because tcc scans libc.a to a fixpoint
# and libtcc1.a afterwards, never libc.a again, and va_list.o needs memset()
# and abort(). MEASURED without it first (2026-08-21, SYSROOT_MKFLAGS= on the
# command line): sysroot_hello.c linked, because printf's dependencies had
# already pulled string.o and stdlib.o; the minimal variadic program in
# sysroot_link_test.py -- va_start/va_arg and nothing else -- died on
# `tcc: error: undefined symbol 'memset'`, and so did the stdarg probe in
# sysroot_hdr_test.py. Both gates keep that measurement as a check.
SYSROOT_MKFLAGS ?= --libtcc1-in-libc

# The flags are part of what the tree is a function of; a command-line
# override must rebuild it. Written through cmp so the file's mtime moves
# only when the content does.
.PHONY: sysroot-mkflags
sysroot-mkflags:
	@mkdir -p $(SYSROOT_WORK)
	@echo '$(SYSROOT_MKFLAGS)' | cmp -s - $(SYSROOT_WORK)/mkflags.txt 2>/dev/null \
	    || echo '$(SYSROOT_MKFLAGS)' > $(SYSROOT_WORK)/mkflags.txt
$(SYSROOT_WORK)/mkflags.txt: sysroot-mkflags

# --- the host tcc, from a snapshot of the live tree that compiles ---------------
# Re-attempted whenever a live source changes; on failure the last good
# snapshot stays and the script says so. $(SYSROOT_HOSTTCC) and the snapshot
# under $(SYSROOT_TCCSRC) are both products of this one rule.
SYSROOT_TCC_STAMP := $(SYSROOT_WORK)/tccsrc.stamp
$(SYSROOT_TCC_STAMP): tests/unit/sysroot_tccsnap.sh tests/sysroot.mk \
                      $(wildcard $(SYSROOT_TCCLIVE)/*.c $(SYSROOT_TCCLIVE)/*.h \
                                 $(SYSROOT_TCCLIVE)/include/*.h $(SYSROOT_TCCLIVE)/lib/*)
	@mkdir -p $(SYSROOT_WORK)
	bash tests/unit/sysroot_tccsnap.sh $(CC) $(SYSROOT_TCCLIVE) $(SYSROOT_WORK) '$(SYSROOT_ABS)' \
	    $(wildcard $(BUILD)/tcc/pristine)
	@touch $@
$(SYSROOT_HOSTTCC): $(SYSROOT_TCC_STAMP)
	@test -x $@

# --- libtcc1 for the TARGET, with the Makefile's UCFLAGS -----------------------
# -MMD -MP dropped (no .d files in the work dir); -std=c11 dropped for the .S
# because clang rejects a C standard on assembler input.
SYSROOT_UCFLAGS  := $(filter-out -MMD -MP,$(UCFLAGS))
SYSROOT_TCC1_SRC := $(SYSROOT_TCCSRC)/lib/libtcc1.c $(SYSROOT_TCCSRC)/lib/va_list.c \
                    $(SYSROOT_TCCSRC)/lib/alloca86_64.S
# Member names match upstream's libtcc1.a (libtcc1.o va_list.o alloca86_64.o).
SYSROOT_TCC1_OBJ := $(patsubst %.S,%.o,$(patsubst %.c,%.o,\
                      $(patsubst $(SYSROOT_TCCSRC)/lib/%,$(SYSROOT_WORK)/tcc1/%,$(SYSROOT_TCC1_SRC))))
$(SYSROOT_TCC1_OBJ): $(SYSROOT_TCC_STAMP)
$(SYSROOT_WORK)/tcc1/%.o: $(SYSROOT_TCCSRC)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SYSROOT_UCFLAGS) -w -c $< -o $@
$(SYSROOT_WORK)/tcc1/%.o: $(SYSROOT_TCCSRC)/lib/%.S
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11,$(SYSROOT_UCFLAGS)) -c $< -o $@

# --- crt: crt0_cli.o under the name tcc asks for, and two empty objects --------
$(SYSROOT_WORK)/crt1.o: $(APPDIR)/crt0_cli.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(SYSROOT_WORK)/crtempty.o: tests/unit/sysroot_crtempty.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

# --- the tree ----------------------------------------------------------------
SYSROOT_HDRS := $(shell find c/apps/libc/include -name '*.h')
$(SYSROOT_WORK)/sysroot.stamp: tools/mksysroot.py $(LIBC_OBJS) $(LIBM_OBJ) $(SYSROOT_TCC1_OBJ) \
                               $(SYSROOT_WORK)/crt1.o $(SYSROOT_WORK)/crtempty.o $(SYSROOT_HDRS) \
                               $(SYSROOT_TCC_STAMP) $(SYSROOT_WORK)/mkflags.txt
	@mkdir -p $(SYSROOT_WORK)
	@printf '%s\n' $(LIBC_OBJS) $(LIBM_OBJ) > $(SYSROOT_WORK)/libc.objs
	@printf '%s\n' $(SYSROOT_TCC1_OBJ) > $(SYSROOT_WORK)/libtcc1.objs
	python3 tools/mksysroot.py --out $(SYSROOT) --libc-include c/apps/libc/include \
	    --tcc-include $(SYSROOT_TCCSRC)/include --ar $(SYSROOT_AR) \
	    --libc-objs $(SYSROOT_WORK)/libc.objs --libtcc1-objs $(SYSROOT_WORK)/libtcc1.objs \
	    --crt1 $(SYSROOT_WORK)/crt1.o --crtempty $(SYSROOT_WORK)/crtempty.o \
	    --manifest $(SYSROOT_WORK)/MANIFEST.txt $(SYSROOT_MKFLAGS)
	@touch $@
sysroot: $(SYSROOT_WORK)/sysroot.stamp

# --- gates (host) --------------------------------------------------------------
test-sysroot-tree: tools/mkfs.py tests/unit/sysroot_tree_test.py
	python3 tests/unit/sysroot_tree_test.py $(SYSROOT_WORK)/treetest

test-sysroot-headers: $(SYSROOT_WORK)/sysroot.stamp $(SYSROOT_HOSTTCC) tests/unit/sysroot_hdr_test.py
	python3 tests/unit/sysroot_hdr_test.py $(SYSROOT_HOSTTCC) $(SYSROOT) $(CC) $(SYSROOT_WORK)/hdr

# Produces $(SYSROOT_WORK)/link/hello_sys, the tcc-linked program the image
# carries as /bin/hello-tcc; the negative control is inside the same script
# and its count is in the script's output.
test-sysroot-link: $(SYSROOT_WORK)/sysroot.stamp $(SYSROOT_HOSTTCC) tests/unit/sysroot_link_test.py \
                   tests/unit/sysroot_hello.c
	python3 tests/unit/sysroot_link_test.py $(SYSROOT_HOSTTCC) $(SYSROOT) $(SYSROOT_WORK)/link \
	    tests/unit/sysroot_hello.c $(SYSROOT_WORK)/libc.objs $(SYSROOT_WORK)/libtcc1.objs \
	    $(SYSROOT_NM) $(SYSROOT_AR)

$(SYSROOT_WORK)/link/hello_sys: test-sysroot-link
$(SYSROOT_WORK)/hello-tcc.aex: $(SYSROOT_WORK)/link/hello_sys tools/mkaex.py
	@python3 tools/mkaex.py $< $@ hello-tcc - '?' --cli --category test \
	    --id os.logit.sysroot.hello > /dev/null

# --- the test image: the Makefile's disk + the sysroot at / + /bin/hello-tcc ---
# NOT $(DISK): the sysroot is 500-odd inodes of test fixture until tcc.aex
# exists to use it, and every other harness boots $(DISK). The Makefile's
# file list is taken from `make -n` (continuations joined; -W so the recipe
# is printed even when the disk is up to date), never retyped.
$(SYSROOT_WORK)/disk.img: $(SYSROOT_WORK)/sysroot.stamp $(SYSROOT_WORK)/hello-tcc.aex $(DISK) \
                          tests/unit/sysroot_img.py tools/mkfs.py
	@$(MAKE) -n -W tools/mkfs.py $(DISK) > $(SYSROOT_WORK)/make-n.txt
	python3 tests/unit/sysroot_img.py . $(SYSROOT_WORK)/make-n.txt $@ \
	    $(SYSROOT):/ $(SYSROOT_WORK)/hello-tcc.aex:/bin/hello-tcc

# fs_format_test is the Makefile's own geometry + fsck checker, built here from
# the same recipe test-fs-format uses, pointed at this image.
test-sysroot-image: $(SYSROOT_WORK)/disk.img tests/unit/sysroot_img_test.py
	@$(CC) $(FS_CFLAGS) -o $(SYSROOT_WORK)/fs_format_test tests/unit/fs_format_test.c \
	    c/fs/fsck.c c/drivers/block/crc32.c $(FS_STUB)
	$(SYSROOT_WORK)/fs_format_test $(SYSROOT_WORK)/disk.img
	python3 tests/unit/sysroot_img_test.py $(SYSROOT_WORK)/disk.img $(SYSROOT)

test-sysroot: test-sysroot-tree test-sysroot-headers test-sysroot-link test-sysroot-image

# --- device ------------------------------------------------------------------
test-sysroot-os: $(ISO) $(SYSROOT_WORK)/disk.img
	bash tests/boot/run-sysroot-test.sh $(ISO) $(SYSROOT_WORK)/disk.img
