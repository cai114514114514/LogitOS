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
#
# ---------------------------------------------------------------------------
# RUN THE TWO BINARIES SEQUENTIALLY, INTO FILES. NEVER `diff <(a) <(b)`.
# ---------------------------------------------------------------------------
# Process substitution starts BOTH binaries concurrently. Two of these gates
# (uio, pathx) create and delete real files under a fixed scratch path, so run
# concurrently they race each other and fail for a reason that is nobody's bug.
# It cost two independent agents a debugging session each to find that, so the
# rule is structural here rather than remembered: LIBC_DIFF_RULE always writes
# $(BUILD)/libchost/<name>.{ours,glibc}.out and diffs the files. A future gate
# that "simplifies" this back to process substitution reintroduces a flaky test
# that passes on a fast machine.
#
# One more shared trap: the two binaries are NECESSARILY named differently
# (<name>_ours vs <name>_glibc), so a test that prints its own argv[0] or
# program name cannot use this rule as-is. test-libc-libgen is the one gate that
# needs that (err()/warn() embed the program name), and it re-execs both
# binaries under a common argv[0] with `exec -a`; see its own recipe below.

.PHONY: test-libc-host test-libc2 $(addprefix test-libc-,$(LIBC_DIFFS))

LIBC_HOST_INC := -nostdinc -isystem $(shell $(CC) -print-resource-dir)/include \
                 -Ic/apps/libc/include -Ic/apps/libc/include/uonly -Ic/apps/libc/src
LIBC_SHIM     := tests/unit/libc_host_errno_shim.c

# name -> the implementation sources its "ours" build links, on top of the test
# source itself. Kept next to the rule rather than inside it so that adding a
# gate is one line here plus one name in LIBC_DIFFS.
LIBC_IMPL_fnmatch   := c/apps/libc/src/fnmatch.c
LIBC_IMPL_inet      := c/apps/libc/src/inet.c $(LIBC_SHIM)
LIBC_IMPL_regex     := c/apps/libc/src/regex.c
LIBC_IMPL_libgen    := c/apps/libc/src/libgen.c c/apps/libc/src/err.c $(LIBC_SHIM)
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
LIBC_XCF_pathx := -DLIBC_OURS_HOST_TEST

define LIBC_DIFF_RULE
test-libc-$(1):
	@mkdir -p $$(BUILD)/libchost
	@$$(CC) -std=c11 -O1 -w $$(LIBC_HOST_INC) $$(LIBC_XCF_$(1)) \
	    -o $$(BUILD)/libchost/$(1)_ours \
	    tests/unit/libc_$(1)_test.c $$(LIBC_IMPL_$(1))
	@$$(CC) -std=c11 -O1 -w -o $$(BUILD)/libchost/$(1)_glibc tests/unit/libc_$(1)_test.c
	@$$(BUILD)/libchost/$(1)_glibc > $$(BUILD)/libchost/$(1).glibc.out
	@$$(BUILD)/libchost/$(1)_ours  > $$(BUILD)/libchost/$(1).ours.out
	@if diff -u $$(BUILD)/libchost/$(1).glibc.out $$(BUILD)/libchost/$(1).ours.out; then \
	    echo "PASS: $(1) matches glibc ($$$$(wc -l < $$(BUILD)/libchost/$(1).ours.out) cases)"; \
	else echo "FAIL: $(1) diverges from glibc (above)"; exit 1; fi
endef

LIBC_DIFFS := fnmatch inet regex libgen search strptime memstream uio pathx getopt iconv
$(foreach g,$(LIBC_DIFFS),$(eval $(call LIBC_DIFF_RULE,$(g))))

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
	  bash -c "exec -a /usr/local/bin/progtest $(BUILD)/libchost/libgen_glibc err $$c" \
	      > $(BUILD)/libchost/err.g.out 2> $(BUILD)/libchost/err.g.err; ge=$$?; \
	  if [ "$$oe" != "$$ge" ] \
	     || ! diff -q $(BUILD)/libchost/err.g.out $(BUILD)/libchost/err.o.out >/dev/null \
	     || ! diff -q $(BUILD)/libchost/err.g.err $(BUILD)/libchost/err.o.err >/dev/null; then \
	    echo "FAIL: err/warn case $$c diverges (exit $$oe vs $$ge)"; \
	    diff -u $(BUILD)/libchost/err.g.err $(BUILD)/libchost/err.o.err || true; ok=0; \
	  fi; \
	done; \
	if [ $$ok = 1 ]; then \
	  echo "PASS: err/warn family matches glibc (8 cases, stdout+stderr+exit)"; \
	else echo "FAIL: err/warn family diverges from glibc (above)"; exit 1; fi
.PHONY: test-libc-err

# Everything above proves the pure-computation additions against a reference.
# This runs them together, host-side, in one pass.
test-libc-host: $(addprefix test-libc-,$(LIBC_DIFFS)) test-libc-err
	@echo "PASS: all host-diffed libc additions agree with glibc"

# --- on-target: the syscall-backed additions (glob, pwd/grp, uname, mman,
# sched, getrlimit, environ) that CANNOT be host-diffed because they talk to
# the real kernel. Same battery pattern as /bin/libctest (main Makefile), a
# second small ELF sharing the exact same LIBC_OBJS so it links the real
# objects the rest of the system runs, not a copy. ---
LIBC2_SRC := tests/unit/libc2_test.c
ifneq ($(wildcard $(LIBC2_SRC)),)
$(BUILD)/asobj/tests/unit/libc2_test.o: $(LIBC2_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
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
