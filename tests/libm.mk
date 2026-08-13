# --- musl libm, built for an ordinary CLI program ----------------------------
#
# WHY THIS EXISTS. third_party/libm has been in the tree since QuickJS, but it
# is compiled only into ENGINE_SRCS (Makefile:588) -- the browser. mini-libc's
# own c/apps/libc/src/math_basic.c implements the float-suffixed bit-twiddling
# subset and nothing else, and says so at the top. So any CLI program that
# calls pow(), exp() or sin() fails at LINK time, which is the first wall a
# ported program hits. This fragment gives the same sources their own objects
# so /bin/anything can link them.
#
# ITS OWN OBJECT DIRECTORY, AND THIS IS THE LOAD-BEARING PART.
# musl's third_party/libm/libm.h uses bare `hidden` on twenty prototypes and
# does not include <features.h> itself, so -include features.h is genuinely
# required to compile it. But features.h defines LOWERCASE `weak` and `hidden`
# as attribute macros, and letting either escape into a port's own translation
# units is the disaster documented at Makefile:663-675: a struct field named
# `weak` expands to `__attribute__((__weak__))`, which declares no member, so
# the field DISAPPEARS from that TU's view of the struct while remaining in
# others'. sizeof skews, and code walks the struct at the wrong stride. It is
# not a compile error. Lua's global_State has a field named exactly `weak`.
#
# So the force-include is scoped to $(BUILD)/libmobj and nothing else, the same
# way $(BUILD)/cssobj scopes LibCSS's -D_ALIGNED= and -DWITHOUT_ICONV_FILTER.
# Do NOT reuse $(BUILD)/jsobj/third_party/libm/*.o: those carry -DLOGIT_OS,
# -DCONFIG_STACK_CHECK and -Ithird_party/quickjs, and reusing them would make
# /bin/lua depend on the browser's flag set.
LIBM_SRC := $(sort $(wildcard third_party/libm/*.c))
LIBM_OBJ := $(patsubst %.c,$(BUILD)/libmobj/%.o,$(LIBM_SRC))

$(BUILD)/libmobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -include features.h -Ithird_party/libm -c $< -o $@

# --- /bin/libmcheck: the port is bit-identical to a native build --------------
#
# Not "musl is correct" -- musl and glibc differ in the last ulp of a
# transcendental and neither is wrong. This asserts that the freestanding
# cross-build of the SAME sources produces the same bits as a native build of
# them, which is the thing that can actually break: a different long double
# path, a missing __rem_pio2_large, an SSE mode set by boot code instead of by
# the ABI. Same separation c/apps/audio/audiocheck.c:8-20 draws for the codecs.
#
# The host reference links the same third_party/libm objects. They are OBJECTS,
# not an archive, so ld takes ours and never reaches glibc's libm for anything
# they define.
LIBMCHK_SRC := tests/unit/libmcheck.c
ifneq ($(wildcard $(LIBMCHK_SRC)),)

$(BUILD)/libmobj/libmcheck.o: $(LIBMCHK_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/libmcheck.elf: $(BUILD)/libmobj/libmcheck.o $(LIBM_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/libmcheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/libmcheck.crt0c.o $(BUILD)/libmobj/libmcheck.o $(LIBM_OBJ) $(LIBC_OBJS)

$(BUILD)/libmcheck.aex: $(BUILD)/libmcheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/libmcheck.elf $@ libmcheck - '?' 150 150 150

CLI += libmcheck
$(DISK): $(BUILD)/libmcheck.aex

# The host half, and it needs OUR headers, not the platform's. Two reasons, and
# the first one is not optional:
#
#   -include features.h against glibc's <math.h> does not compile at all --
#   `weak` and `hidden` are lowercase macros and glibc's declarations use those
#   words. So the libm sources are built the way tests/libc.mk builds our libc
#   host-side: -nostdinc plus c/apps/libc/include, which is exactly why
#   features.h and endian.h live there in the first place.
#
#   -fno-builtin so the compiler cannot constant-fold sin(0.5) at compile time
#   and quietly compare ITS answer instead of musl's. The target build gets the
#   same protection from UCFLAGS' -ffreestanding.
#
# Note the force-include is on the libm objects only, never on libmcheck.c --
# the same scoping the target side uses, for the same reason.
LIBM_HOST_INC := -nostdinc -isystem $(shell $(CC) -print-resource-dir)/include \
                 -Ic/apps/libc/include -Ic/apps/libc/include/uonly -Ithird_party/libm
LIBM_HOST_OBJ := $(patsubst %.c,$(BUILD)/libmhost/%.o,$(LIBM_SRC))

$(BUILD)/libmhost/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w -fno-builtin -include features.h $(LIBM_HOST_INC) -c $< -o $@

$(BUILD)/libmhost/libmcheck.o: $(LIBMCHK_SRC)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w -fno-builtin $(LIBM_HOST_INC) -c $< -o $@

# musl's nearbyint calls fetestexcept/feclearexcept, which are mini-libc's
# (c/apps/libc/src/fenv.c, MXCSR). The target already links it through
# LIBC_OBJS; the host reference has to link the same file, not glibc's fenv, or
# the two builds would differ in a function that is part of what is being
# compared.
$(BUILD)/libmhost/fenv.o: c/apps/libc/src/fenv.c
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w -fno-builtin $(LIBM_HOST_INC) -c $< -o $@

$(BUILD)/libmhost/ref: $(BUILD)/libmhost/libmcheck.o $(LIBM_HOST_OBJ) $(BUILD)/libmhost/fenv.o
	@$(CC) -o $@ $(BUILD)/libmhost/libmcheck.o $(LIBM_HOST_OBJ) $(BUILD)/libmhost/fenv.o

test-libm-host: $(BUILD)/libmhost/ref
	@$(BUILD)/libmhost/ref > $(BUILD)/libmhost/ref.out
	@echo "PASS: host reference built, $$(wc -l < $(BUILD)/libmhost/ref.out) lines"

test-libm-cli: $(ISO) $(DISK) $(BUILD)/libmhost/ref
	@$(BUILD)/libmhost/ref > $(BUILD)/libmhost/ref.out
	@bash tests/boot/run-libm-test.sh $(ISO) $(DISK) $(BUILD)/libmhost/ref.out

.PHONY: test-libm-host test-libm-cli
else
$(warning tests/libm.mk: $(LIBMCHK_SRC) is missing -- /bin/libmcheck will not be built)
endif
