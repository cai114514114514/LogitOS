# tests/libc.mk -- gates for the "mini-libc -> real C library" work (fnmatch,
# glob, regex, arpa/inet, pwd/grp, sys/utsname, sys/resource, sched.h,
# sys/mman.h, syslog.h, termios.h, poll.h/sys/select.h, netdb.h,
# sys/socket.h). Kept out of the main Makefile on purpose -- several agents
# edit that file concurrently and a shared-file edit cannot be committed
# without swallowing theirs (the same reason tests/audio.mk, tests/h265.mk,
# tests/nic.mk are their own fragments). The root Makefile pulls this in with
# one line: `-include tests/libc.mk`.
#
# THE DIFF STRATEGY. fnmatch()/inet_ntop()/inet_pton()/regcomp()/regexec()
# are pure computation -- no syscall anywhere in them -- so "correct" here
# means "agrees with glibc". Each test source below is compiled TWICE:
#   (a) against OUR OWN headers/implementation, using clang's minimal
#       freestanding resource-dir headers (no glibc at all: same recipe as
#       LIBCDIFF_INC in the main Makefile, which is the established pattern
#       for this exact kind of test -- see its comment for why -nostdinc is
#       needed there too);
#   (b) as a completely ordinary host program against glibc.
# The two runs' stdout is diffed byte for byte. This needs NO symbol
# renaming (unlike test-libc-diff, which links both into ONE process): each
# build is its own process, so "ours" and "glibc" never have to coexist in
# the same symbol table. A named FNM_*/REG_*/AF_INET6 macro used in the test
# source resolves against whichever header that particular compile saw, so
# the comparison is of BEHAVIOUR, not of the two implementations happening to
# assign a constant the same bit -- which is the correct thing to compare.

.PHONY: test-libc-fnmatch test-libc-inet test-libc-regex test-libc2 test-libc-host

LIBC_HOST_INC := -nostdinc -isystem $(shell $(CC) -print-resource-dir)/include \
                 -Ic/apps/libc/include -Ic/apps/libc/include/uonly -Ic/apps/libc/src

test-libc-fnmatch:
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -w $(LIBC_HOST_INC) -o $(BUILD)/libchost/fnmatch_ours \
	    tests/unit/libc_fnmatch_test.c c/apps/libc/src/fnmatch.c
	@$(CC) -std=c11 -O1 -w -o $(BUILD)/libchost/fnmatch_glibc tests/unit/libc_fnmatch_test.c
	@$(BUILD)/libchost/fnmatch_ours  > $(BUILD)/libchost/fnmatch_ours.out
	@$(BUILD)/libchost/fnmatch_glibc > $(BUILD)/libchost/fnmatch_glibc.out
	@if diff -u $(BUILD)/libchost/fnmatch_glibc.out $(BUILD)/libchost/fnmatch_ours.out; then \
	    echo "PASS: fnmatch matches glibc ($$(wc -l < $(BUILD)/libchost/fnmatch_ours.out) cases)"; \
	else echo "FAIL: fnmatch diverges from glibc (above)"; exit 1; fi

test-libc-inet:
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -w $(LIBC_HOST_INC) -o $(BUILD)/libchost/inet_ours \
	    tests/unit/libc_inet_test.c c/apps/libc/src/inet.c tests/unit/libc_host_errno_shim.c
	@$(CC) -std=c11 -O1 -w -o $(BUILD)/libchost/inet_glibc tests/unit/libc_inet_test.c
	@$(BUILD)/libchost/inet_ours  > $(BUILD)/libchost/inet_ours.out
	@$(BUILD)/libchost/inet_glibc > $(BUILD)/libchost/inet_glibc.out
	@if diff -u $(BUILD)/libchost/inet_glibc.out $(BUILD)/libchost/inet_ours.out; then \
	    echo "PASS: arpa/inet matches glibc ($$(wc -l < $(BUILD)/libchost/inet_ours.out) cases)"; \
	else echo "FAIL: arpa/inet diverges from glibc (above)"; exit 1; fi

test-libc-regex:
	@mkdir -p $(BUILD)/libchost
	@$(CC) -std=c11 -O1 -w $(LIBC_HOST_INC) -o $(BUILD)/libchost/regex_ours \
	    tests/unit/libc_regex_test.c c/apps/libc/src/regex.c
	@$(CC) -std=c11 -O1 -w -o $(BUILD)/libchost/regex_glibc tests/unit/libc_regex_test.c
	@$(BUILD)/libchost/regex_ours  > $(BUILD)/libchost/regex_ours.out
	@$(BUILD)/libchost/regex_glibc > $(BUILD)/libchost/regex_glibc.out
	@if diff -u $(BUILD)/libchost/regex_glibc.out $(BUILD)/libchost/regex_ours.out; then \
	    echo "PASS: regex matches glibc on unambiguous cases ($$(wc -l < $(BUILD)/libchost/regex_ours.out) cases)"; \
	else echo "FAIL: regex diverges from glibc (above)"; exit 1; fi

# Everything above proves the pure-computation additions against a reference.
# This runs them together, host-side, in one pass.
test-libc-host: test-libc-fnmatch test-libc-inet test-libc-regex
	@echo "PASS: all host-diffed libc additions agree with glibc"

# --- on-target: the syscall-backed additions (glob, pwd/grp, uname, mman,
# sched, getrlimit) that CANNOT be host-diffed because they talk to the real
# kernel. Same battery pattern as /bin/libctest (main Makefile), a second
# small ELF sharing the exact same LIBC_OBJS so it links the real objects the
# rest of the system runs, not a copy. ---
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
