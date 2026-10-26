# tests/libc.mk -- gates for the "mini-libc -> real C library" work.
# Kept out of the main Makefile on purpose -- several agents edit that file
# concurrently and a shared-file edit cannot be committed without swallowing
# theirs (the same reason tests/audio.mk, tests/h265.mk, tests/nic.mk are their
# own fragments). The root Makefile pulls this in with one line:
# `-include tests/libc.mk`.
#
# THE DIFF STRATEGY. fnmatch(), inet_ntop(), regcomp(), basename(), tsearch(),
# strptime(), fmemopen(), readv(), realpath(), getopt(), iconv() and friends are
# either pure computation or thin wrappers over calls the host also has -- so
# "correct" here means "agrees with glibc", and that gives every one of them a
# REFERENCE rather than a hand-written expectation. Each test source below is
# compiled TWICE:
#   (a) against OUR OWN headers/implementation, using clang's minimal
#       freestanding resource-dir headers (no glibc headers at all: same recipe
#       as LIBCDIFF_INC in the main Makefile);
#   (b) as a completely ordinary host program against glibc.
# The two runs' stdout is diffed byte for byte. This needs NO symbol renaming
# (unlike test-libc-diff, which links both into ONE process): each build is its
# own process, so "ours" and "glibc" never coexist in one symbol table. A named
# FNM_*/REG_*/AF_INET6/SEEK_* macro used in the test source resolves against
# whichever header THAT compile saw, so the comparison is of BEHAVIOUR, not of
# two implementations happening to assign a constant the same bit -- which is
# the correct thing to compare.
#
# Note (a) still LINKS against glibc: any libc function the test calls that we
# did not write resolves to the host's. That is why most gates also link
# tests/unit/libc_host_errno_shim.c -- our TUs reference `errno` as an ordinary
# int, which glibc does not provide under that name.
# Correction (2026-09-22): the historical description above assumed Linux.
# The reference actually links the HOST libc, including Darwin's libSystem.
# Its pton accepts leading decimal zeroes; treating that as glibc's strict
# contract produced a false inet regression. Name the reference honestly and
# keep deliberate parser-policy differences in a separate assertion gate.
#
# ---------------------------------------------------------------------------
# RUN THE TWO BINARIES SEQUENTIALLY, INTO FILES. NEVER `diff <(a) <(b)`.
# ---------------------------------------------------------------------------
# Process substitution starts BOTH binaries concurrently. Two of these gates
# (uio, pathx) create and delete real files under a fixed scratch path, so run
# concurrently they race each other and fail for a reason that is nobody's bug.
# It cost two independent agents a debugging session each to find that, so the
# rule is structural here rather than remembered: LIBC_DIFF_RULE always writes
# $(BUILD)/libchost/<name>.{ours,reference}.out and diffs the files. A future gate
# that "simplifies" this back to process substitution reintroduces a flaky test
# that passes on a fast machine.
#
# One more shared trap: the two binaries are NECESSARILY named differently
# (<name>_ours vs <name>_reference), so a test that prints its own argv[0] or
# program name cannot use this rule as-is. test-libc-libgen is the one gate that
# needs that (err()/warn() embed the program name), and it re-execs both
# binaries under a common argv[0] with `exec -a`; see its own recipe below.

.PHONY: test-libc-host test-libc2 test-libc-expand-host test-libc-expand-control \
        test-libc-argz-bounds test-libc-argz-bounds-control \
        $(addprefix test-libc-,$(LIBC_DIFFS))

# The root rule owns the recipe; this fragment adds the expansion half's
# header dependency so changing a guest assertion can never reuse a stale
# libctest object.
$(BUILD)/asobj/tests/unit/libctest_main.o: tests/unit/libctest_expand.h

LIBC_HOST_INC := -nostdinc -isystem $(shell $(CC) -print-resource-dir)/include \
                 -Ic/apps/libc/include -Ic/apps/libc/include/uonly -Ic/apps/libc/src
LIBC_SHIM     := tests/unit/libc_host_errno_shim.c
LIBC_HOST_NAME := $(shell uname -s) host libc

# Query the selected compiler's actual C runtime headers. "Linux" alone may
# mean musl, and uname's architecture may differ from the compiler target.
# Several historical fixtures intentionally assert GNU-only behavior; a
# Darwin byte diff is not an alternate oracle for those contracts.
LIBC_HOST_MACROS := $(shell $(CC) -dM -E -include stdlib.h -x c /dev/null 2>/dev/null)
LIBC_HOST_GLIBC := $(if $(findstring __GLIBC__,$(LIBC_HOST_MACROS)),yes,no)
LIBC_HOST_LINUX_X64 := $(if $(and $(findstring __linux__,$(LIBC_HOST_MACROS)),$(findstring __x86_64__,$(LIBC_HOST_MACROS))),yes,no)
LIBC_PLATFORM_SKIPS :=
ifeq ($(LIBC_HOST_GLIBC),no)
LIBC_PLATFORM_SKIPS += strptime getopt pathx iconv
endif
ifeq ($(LIBC_HOST_LINUX_X64),no)
LIBC_PLATFORM_SKIPS += pathx iconv
endif
LIBC_PLATFORM_SKIPS := $(sort $(LIBC_PLATFORM_SKIPS))
LIBC_REQUIRE_strptime := glibc reference (GNU partial-field and timezone semantics)
LIBC_REQUIRE_getopt := glibc reference (GNU reset and argv permutation semantics)
LIBC_REQUIRE_pathx := Linux x86_64 glibc (fixture uses Linux syscall numbers and struct stat)
LIBC_REQUIRE_iconv := Linux x86_64 glibc (linked runtime uses x86 int-0x80 assembly)

# name -> the implementation sources its "ours" build links, on top of the test
# source itself. Kept next to the rule rather than inside it so that adding a
# gate is one line here plus one name in LIBC_DIFFS.
LIBC_IMPL_fnmatch   := c/apps/libc/src/fnmatch.c
LIBC_IMPL_inet      := c/apps/libc/src/inet.c $(LIBC_SHIM)
LIBC_IMPL_regex     := c/apps/libc/src/regex.c
LIBC_IMPL_search    := c/apps/libc/src/search.c $(LIBC_SHIM)
LIBC_IMPL_strptime  := c/apps/libc/src/strptime.c $(LIBC_SHIM)
LIBC_IMPL_memstream := c/apps/libc/src/stdio.c c/apps/libc/src/memstream.c \
                       c/apps/libc/src/dtoa.c c/apps/libc/src/scanf.c $(LIBC_SHIM)
LIBC_IMPL_uio       := c/apps/libc/src/uio.c $(LIBC_SHIM)
LIBC_IMPL_pathx     := c/apps/libc/src/pathx.c c/apps/libc/src/ftw.c $(LIBC_SHIM)
LIBC_IMPL_getopt    := c/apps/libc/src/getopt.c
# langinfo.c and locale.c are NOT optional here, and leaving them out does not
# fail to link -- it SEGFAULTS AT RUNTIME. Host build (a) is an ordinary
# dynamically-linked program, so an unresolved nl_langinfo() quietly binds to
# GLIBC's, which then returns a pointer into glibc's own locale structures that
# our code walks with our layout. locale.c comes along because langinfo.c reads
# RADIXCHAR/THOUSEP out of localeconv(). tests/unit/libc_iconv_test.c's header
# records this at length; it is the general hazard of strategy (a) -- a missing
# implementation TU is a runtime fallback, not a link error.
LIBC_IMPL_iconv     := c/apps/libc/src/iconv.c c/apps/libc/src/wchar.c \
                       c/apps/libc/src/malloc.c c/apps/libc/src/stdlib.c \
                       c/apps/libc/src/dtoa.c c/apps/libc/src/langinfo.c \
                       c/apps/libc/src/locale.c $(LIBC_SHIM)

# pathx's fixture-building code has to know which build it is in: it creates a
# real directory tree under /tmp and both builds would otherwise use the same
# path. Nothing else needs a per-build define, and nothing else should.
# Correction (2026-09-22): search/uio now also distinguish their portable
# reference subset from project-only assertions; the GNU corpus stays intact.
LIBC_XCF_pathx := -DLIBC_OURS_HOST_TEST

ifeq ($(LIBC_HOST_GLIBC),no)
# The default source still holds the GNU corpus. On other hosts, compare the
# common API contract and assert project-only extensions separately; stderr
# names the GNU comparison that remains unavailable.
LIBC_XCF_search := -DLIBC_COMMON_TEST -DLIBC_OURS_HOST_TEST
LIBC_REF_XCF_search := -DLIBC_COMMON_TEST
LIBC_DEPS_search := tests/unit/libc_search_common.h
LIBC_XCF_uio := -DLIBC_COMMON_TEST -DLIBC_OURS_HOST_TEST
LIBC_REF_XCF_uio := -DLIBC_COMMON_TEST
LIBC_IMPL_uio += $(BUILD)/libchost/uio_host.o
LIBC_REF_IMPL_uio := $(BUILD)/libchost/uio_host.o
LIBC_DEPS_uio := tests/unit/libc_uio_common.h tests/unit/libc_uio_host.h $(BUILD)/libchost/uio_host.o

# Darwin's fmemopen rejects a zero-sized buffer.  The glibc corpus dereferences
# the resulting NULL stream, so both sides must omit those cases here.  Keep
# the complete corpus on glibc, where zero-sized streams are a real contract.
LIBC_XCF_memstream := -DLIBC_COMMON_TEST
LIBC_REF_XCF_memstream := -DLIBC_COMMON_TEST

# Setup uses the host's actual open flags and errno ABI. Compile it separately
# with host headers; compiling it with LIBC_HOST_INC would recreate the ABI
# mismatch that made the former Linux-number fixture invalid on Darwin.
$(BUILD)/libchost/uio_host.o: tests/unit/libc_uio_host.c tests/unit/libc_uio_host.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O1 -Wall -Wextra -c $< -o $@

test-libc-search-control:
	python3 tests/unit/libc_portable_control.py --build $(BUILD)/libchost --family search --cc "$(CC)"

test-libc-uio-control:
	python3 tests/unit/libc_portable_control.py --build $(BUILD)/libchost --family uio --cc "$(CC)"

test-libc-search: test-libc-search-control
test-libc-uio: test-libc-uio-control
.PHONY: test-libc-search-control test-libc-uio-control
endif

define LIBC_DIFF_RULE
test-libc-$(1): $$(LIBC_DEPS_$(1))
	@mkdir -p $$(BUILD)/libchost
	@$$(CC) -std=c11 -O1 -w $$(LIBC_HOST_INC) $$(LIBC_XCF_$(1)) \
	    -o $$(BUILD)/libchost/$(1)_ours \
	    tests/unit/libc_$(1)_test.c $$(LIBC_IMPL_$(1))
	@$$(CC) -std=c11 -O1 -w $$(LIBC_REF_XCF_$(1)) -o $$(BUILD)/libchost/$(1)_reference tests/unit/libc_$(1)_test.c $$(LIBC_REF_IMPL_$(1))
	@$$(BUILD)/libchost/$(1)_reference > $$(BUILD)/libchost/$(1).reference.out
	@$$(BUILD)/libchost/$(1)_ours  > $$(BUILD)/libchost/$(1).ours.out
	@if diff -u $$(BUILD)/libchost/$(1).reference.out $$(BUILD)/libchost/$(1).ours.out; then \
	  echo "PASS: $(1) matches $$(LIBC_HOST_NAME) ($$$$(wc -l < $$(BUILD)/libchost/$(1).ours.out) output rows)"; \
	else echo "FAIL: $(1) diverges from $$(LIBC_HOST_NAME) (above)"; exit 1; fi
endef

LIBC_DIFFS := fnmatch inet regex libgen search strptime memstream uio pathx getopt iconv
.PHONY: $(addprefix test-libc-,$(LIBC_DIFFS))
$(foreach g,$(filter-out libgen $(LIBC_PLATFORM_SKIPS),$(LIBC_DIFFS)),$(eval $(call LIBC_DIFF_RULE,$(g))))

# Do not manufacture a pass, normalize away differences, or compile Linux raw
# syscall fixtures on Darwin. Preserve the full gates for their real oracle
# and print both the missing capability and the command that settles it.
define LIBC_PLATFORM_SKIP
test-libc-$(1):
	@echo "SKIP: test-libc-$(1) requires $$(LIBC_REQUIRE_$(1)); run make test-libc-$(1) on Linux x86_64 with glibc."
endef
$(foreach g,$(LIBC_PLATFORM_SKIPS),$(eval $(call LIBC_PLATFORM_SKIP,$(g))))

# Correction to the historical byte-diff claim: Darwin's permitted "//"
# policy differs. Keep all cases, assert our GNU-style root explicitly, and
# compare every other field. The runner also links the real memrchr on Darwin
# (where host libc has no such symbol), without interposing host string APIs.
test-libc-libgen-control:
	python3 tests/unit/libc_libgen_test.py --build $(BUILD)/libchost --cc "$(CC)" --controls

test-libc-libgen: test-libc-libgen-control
	python3 tests/unit/libc_libgen_test.py --build $(BUILD)/libchost --cc "$(CC)"

.PHONY: test-libc-libgen test-libc-libgen-control

# Keep controls on the ordinary entrypoint, not only an aggregate CI target.
test-libc-inet-control:
	python3 tests/unit/libc_inet_contract_test.py --build $(BUILD)/inet-contract --cc "$(CC)" --controls

test-libc-inet-contract: test-libc-inet-control
	python3 tests/unit/libc_inet_contract_test.py --build $(BUILD)/inet-contract --cc "$(CC)"

test-libc-inet: test-libc-inet-contract
.PHONY: test-libc-inet test-libc-inet-control test-libc-inet-contract

# The same assertions can run against the complete shipped libc in ring 3.
# Build explicitly and pack into an isolated test disk; this diagnostic does
# not become a normal installed CLI program merely because the gate exists.
$(BUILD)/inet-contract.elf: $(BUILD)/asobj/tests/unit/libc_inet_contract_test.o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/inet-contract.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/inet-contract.crt0c.o $(BUILD)/asobj/tests/unit/libc_inet_contract_test.o $(LIBC_OBJS)

$(BUILD)/inet-contract.aex: $(BUILD)/inet-contract.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ inet-contract - '?' 150 150 150

# err()/warn() print "<progname>: ...", and the two binaries cannot share a
# name, so the generic rule's diff would fail on the program name alone. Both
# are re-exec'd under one argv[0] instead. stderr is compared as well as stdout
# and so is the exit status, because err() is supposed to exit(status) -- a
# version that printed correctly and returned would pass a stdout-only diff.
#
# NO `set -e` IN THIS RECIPE. err() and errx() are SUPPOSED to exit non-zero --
# that is half of what is being compared -- so an errexit shell aborts on the
# first correct result. The exit status is captured and diffed instead.
test-libc-err: test-libc-libgen
	@ok=1; \
	for c in warnfam err errnull errx errxnull verr verrx errzero; do \
	  bash -c "exec -a /usr/local/bin/progtest $(BUILD)/libchost/libgen_ours err $$c" \
	      > $(BUILD)/libchost/err.o.out 2> $(BUILD)/libchost/err.o.err; oe=$$?; \
	  bash -c "exec -a /usr/local/bin/progtest $(BUILD)/libchost/libgen_reference err $$c" \
	      > $(BUILD)/libchost/err.g.out 2> $(BUILD)/libchost/err.g.err; ge=$$?; \
	  if [ "$$oe" != "$$ge" ] \
	     || ! diff -q $(BUILD)/libchost/err.g.out $(BUILD)/libchost/err.o.out >/dev/null \
	     || ! diff -q $(BUILD)/libchost/err.g.err $(BUILD)/libchost/err.o.err >/dev/null; then \
	    echo "FAIL: err/warn case $$c diverges (exit $$oe vs $$ge)"; \
	    diff -u $(BUILD)/libchost/err.g.err $(BUILD)/libchost/err.o.err || true; ok=0; \
	  fi; \
	done; \
	if [ $$ok = 1 ]; then \
	  echo "PASS: err/warn family matches $(LIBC_HOST_NAME) (8 cases, stdout+stderr+exit)"; \
	else echo "FAIL: err/warn family diverges from $(LIBC_HOST_NAME) (above)"; exit 1; fi
.PHONY: test-libc-err

# Everything above proves the pure-computation additions against a reference.
# This runs them together, host-side, in one pass.
test-libc-host: $(addprefix test-libc-,$(LIBC_DIFFS)) test-libc-err
	@echo "DONE: available libc host comparisons and contracts completed; see any SKIP lines for missing platform coverage ($(LIBC_HOST_NAME))"

# Broad, fast host coverage for the GNU/BSD/C23 compatibility wave.  The
# syscall-backed entropy and UTF-16 state-machine cases remain in /bin/libctest
# because a Darwin process cannot exercise Logit's int-0x80 ABI or its mbstate_t.
LIBC_EXPAND_HOST_SRC := tests/unit/libc_expand_host_test.c \
    c/apps/libc/src/argz.c c/apps/libc/src/envz.c c/apps/libc/src/intl.c \
    c/apps/libc/src/string_ext.c c/apps/libc/src/compat.c \
    c/apps/libc/src/drand48.c c/apps/libc/src/locale_ext.c \
    c/apps/libc/src/ctype_l.c $(LIBC_SHIM)

test-libc-expand-control:
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -w $(LIBC_HOST_INC) -DLIBC_EXPAND_NEGATIVE_CONTROL \
	    -o $(BUILD)/libchost/libc_expand_control $(LIBC_EXPAND_HOST_SRC)
	@$(BUILD)/libchost/libc_expand_control > $(BUILD)/libchost/libc_expand_control.out; rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q '^EXPECTED_CONTROL_FAILURE argz_replace multi-hit$$' \
	       $(BUILD)/libchost/libc_expand_control.out; then \
	    echo "CONTROL FAILED: the sabotaged multi-hit argz_replace was not detected"; \
	    cat $(BUILD)/libchost/libc_expand_control.out; exit 1; \
	  fi; \
	  echo "control ok: host gate rejected the sabotaged second argz replacement"

test-libc-expand-host: test-libc-expand-control
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -w $(LIBC_HOST_INC) \
	    -o $(BUILD)/libchost/libc_expand_host $(LIBC_EXPAND_HOST_SRC)
	@$(BUILD)/libchost/libc_expand_host

# A malformed final argz element is still confined by argz_len.  Run this
# under ASan because an unbounded string search can produce the right return
# value while silently reading beyond the caller's allocation.
test-libc-argz-bounds-control:
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -g -w -fsanitize=address,undefined \
	    $(LIBC_HOST_INC) -DLIBC_ARGZ_UNBOUNDED_CONTROL \
	    -o $(BUILD)/libchost/argz_bounds_control \
	    tests/unit/libc_argz_bounds_test.c c/apps/libc/src/argz.c
	@$(BUILD)/libchost/argz_bounds_control > $(BUILD)/libchost/argz_bounds_control.out 2>&1; rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q 'AddressSanitizer: heap-buffer-overflow' \
	       $(BUILD)/libchost/argz_bounds_control.out; then \
	    echo 'CONTROL FAILED: unbounded argz scan escaped ASan'; \
	    cat $(BUILD)/libchost/argz_bounds_control.out; exit 1; \
	  fi; \
	  echo 'control ok: ASan rejected unbounded argz scan'

test-libc-argz-bounds: test-libc-argz-bounds-control
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -g -w -fsanitize=address,undefined \
	    $(LIBC_HOST_INC) -o $(BUILD)/libchost/argz_bounds \
	    tests/unit/libc_argz_bounds_test.c c/apps/libc/src/argz.c
	@$(BUILD)/libchost/argz_bounds

# Make the aggregate name reach the expansion even though an older unrelated
# inet differential currently reddens earlier on Darwin (reported separately).
# Correction (2026-09-22): inet now separates Darwin differences from the
# strict contract and tests IPv6 rejection with controls; keep expansion wired.
test-libc-host: test-libc-expand-host test-libc-argz-bounds

# --- on-target: the syscall-backed additions (glob, pwd/grp, uname, mman,
# sched, getrlimit, environ) that CANNOT be host-diffed because they talk to
# the real kernel. Same battery pattern as /bin/libctest (main Makefile), a
# second small ELF sharing the exact same LIBC_OBJS so it links the real
# objects the rest of the system runs, not a copy. ---
LIBC2_SRC := tests/unit/libc2_test.c
ifneq ($(wildcard $(LIBC2_SRC)),)
$(BUILD)/asobj/tests/unit/libc2_test.o: $(LIBC2_SRC)
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@
$(BUILD)/libctest2.elf: $(BUILD)/asobj/tests/unit/libc2_test.o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/libctest2.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/libctest2.crt0c.o $(BUILD)/asobj/tests/unit/libc2_test.o $(LIBC_OBJS)
$(BUILD)/libctest2.aex: $(BUILD)/libctest2.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/libctest2.elf $@ libctest2 - '?' 150 150 150
CLI += libctest2
$(DISK): $(BUILD)/libctest2.aex
test-libc2: $(ISO) $(DISK)
	@bash tests/boot/run-libc2-test.sh $(ISO) $(DISK)
else
$(warning tests/libc.mk: $(LIBC2_SRC) is missing -- /bin/libctest2 will not be built, and test-libc2 cannot run)
endif

# WCHAR_MIN / WCHAR_MAX describe wchar_t. The header said `WCHAR_MIN 0` for a
# SIGNED 32-bit wchar_t and fought the compiler's <stdint.h>. A property of the
# header is a compile-time fact, so the gate is -fsyntax-only over _Static_asserts.
# The control re-injects the old definition into a scratch copy of the header and
# the same file must then FAIL to compile -- on the assertion, not on anything else.
LIBC_WCHAR_CC := $(CC) --target=x86_64-elf -ffreestanding -fsyntax-only
.PHONY: test-libc-wchar-limits test-libc-wchar-limits-negctl
test-libc-wchar-limits-negctl:
	@d=$(BUILD)/wchar-limits-neg; mkdir -p $$d; \
	 sed 's/^typedef int wint_t;/typedef int wint_t;\n#define WCHAR_MIN 0/' c/apps/libc/include/wchar.h > $$d/wchar.h; \
	 if $(LIBC_WCHAR_CC) -I$$d -Ic/apps/libc/include/uonly -Ic/apps/libc/include \
	      tests/unit/libc_wchar_limits_test.c > $$d/log 2>&1; then \
	     echo "NEGATIVE CONTROL FAILED: WCHAR_MIN 0 on a signed wchar_t compiled"; exit 1; fi; \
	 grep -q 'WCHAR_MIN disagrees with the signedness' $$d/log || \
	     { echo "NEGATIVE CONTROL FAILED: it failed, but not on the signedness assertion"; cat $$d/log; exit 1; }; \
	 echo "negative control ok: the old WCHAR_MIN 0 is rejected"
test-libc-wchar-limits: test-libc-wchar-limits-negctl
	$(LIBC_WCHAR_CC) -Ic/apps/libc/include/uonly -Ic/apps/libc/include tests/unit/libc_wchar_limits_test.c
	@echo "libc wchar limits: ok"
ci-host: test-libc-wchar-limits

# MB_LEN_MAX was 1 -- clang's <limits.h> defines it first and the mini-libc value
# sat behind #ifndef -- while MB_CUR_MAX is 4. Control: re-inject the #ifndef form
# into a scratch copy of limits.h; the same file must fail on the named assertion.
.PHONY: test-libc-mb-len-max test-libc-mb-len-max-negctl
test-libc-mb-len-max-negctl:
	@d=$(BUILD)/mb-len-max-neg; mkdir -p $$d; \
	 sed -e 's/^#undef MB_LEN_MAX$$/#ifndef MB_LEN_MAX/' -e 's/^#define MB_LEN_MAX 4 .*/#define MB_LEN_MAX 4\n#endif/' \
	     -e 's|#include_next <limits.h>|#include "'"$$($(CC) -print-resource-dir)"'/include/limits.h"|' \
	     c/apps/libc/include/limits.h > $$d/limits.h; \
	 if $(LIBC_WCHAR_CC) -I$$d -Ic/apps/libc/include/uonly -Ic/apps/libc/include \
	      tests/unit/libc_mb_len_max_test.c > $$d/log 2>&1; then \
	     echo "NEGATIVE CONTROL FAILED: the #ifndef form compiled"; exit 1; fi; \
	 grep -q 'MB_LEN_MAX is smaller than MB_CUR_MAX' $$d/log || \
	     { echo "NEGATIVE CONTROL FAILED: not on the MB_CUR_MAX assertion"; cat $$d/log; exit 1; }; \
	 echo "negative control ok: the #ifndef form leaves MB_LEN_MAX at clang's 1"
test-libc-mb-len-max: test-libc-mb-len-max-negctl
	$(LIBC_WCHAR_CC) -Ic/apps/libc/include/uonly -Ic/apps/libc/include tests/unit/libc_mb_len_max_test.c
	@echo "libc MB_LEN_MAX: ok"
ci-host: test-libc-mb-len-max

# NAME_MAX said 63; the filesystem refuses a 60-character name (59 is the limit).
# The control puts 63 back in a scratch copy of limits.h and the named assertion
# must be what fails.
.PHONY: test-libc-name-max test-libc-name-max-negctl
LIBC_NAME_INC := -Ic/apps/libc/include/uonly -Ic/apps/libc/include -Ic/fs/vfs -Ic/fs/logitfs
test-libc-name-max-negctl:
	@d=$(BUILD)/name-max-neg; mkdir -p $$d; \
	 sed -e 's/^#define NAME_MAX    59 /#define NAME_MAX    63 /' \
	     -e 's|#include_next <limits.h>|#include "'"$$($(CC) -print-resource-dir)"'/include/limits.h"|' \
	     c/apps/libc/include/limits.h > $$d/limits.h; \
	 if $(LIBC_WCHAR_CC) -I$$d $(LIBC_NAME_INC) tests/unit/libc_name_max_test.c > $$d/log 2>&1; then \
	     echo "NEGATIVE CONTROL FAILED: NAME_MAX 63 compiled"; exit 1; fi; \
	 grep -q 'libc NAME_MAX differs' $$d/log || \
	     { echo "NEGATIVE CONTROL FAILED: not on the NAME_MAX assertion"; cat $$d/log; exit 1; }; \
	 echo "negative control ok: NAME_MAX 63 is rejected"
test-libc-name-max: test-libc-name-max-negctl
	$(LIBC_WCHAR_CC) $(LIBC_NAME_INC) tests/unit/libc_name_max_test.c
	@echo "libc NAME_MAX/PATH_MAX: ok"
ci-host: test-libc-name-max
