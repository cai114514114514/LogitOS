# tests/textutils.mk -- the userland tools the "Fluid" release adds to /bin
# (docs/superpowers/specs/2026-09-24-experience-release-design.md, T1-T3).
#
# Every tool here is written against POSIX and MINI-LIBC, not against clib.h,
# for one reason: the SAME source then builds natively on the host, so the
# host gate can run it beside the host's own tool on the same input and diff
# the two. CLI_RULE (Makefile) cannot be reused -- it links crt0_cli and the one
# object and nothing else, so a tool there has no malloc and no stdio.
#
# Placement: `CLI += name` reaches /bin because the $(DISK) recipe expands
# $(CLI) at run time (Makefile, the mkfs line); `$(DISK): x.aex` is what makes
# the image depend on it. Both are the pattern tests/libc.mk uses for
# /bin/libctest2.

TEXTUTILS := chmod df du
# Tools carrying a -DTEXTUTIL_NEGCTL mutation that the host cases must catch.
TEXTUTILS_MUTATED := chmod du
TEXTUTIL_DIR := c/apps/coreutils/text
TEXTUTIL_HDRS := $(wildcard $(TEXTUTIL_DIR)/*.h)

define TEXTUTIL_RULE
$(BUILD)/textutils/$(1).o: $(TEXTUTIL_DIR)/$(1).c $(TEXTUTIL_HDRS)
	@mkdir -p $$(dir $$@)
	$$(COMPILE) $$(UCFLAGS) -c $$< -o $$@
$(BUILD)/$(1).elf: $(BUILD)/textutils/$(1).o $(TEXTUTIL_EXTRA_$(1)) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $$@ $(BUILD)/apps/$(1).crt0c.o \
	    $(BUILD)/textutils/$(1).o $(TEXTUTIL_EXTRA_$(1)) $(LIBC_OBJS)
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
CLI += $(1)
$(DISK): $(BUILD)/$(1).aex
endef
$(foreach t,$(TEXTUTILS),$(eval $(call TEXTUTIL_RULE,$(t))))

# ---- the host gate: the same sources, built natively, against host tools ----
TEXTUTILS_HOST := $(BUILD)/textutils-host
$(TEXTUTILS_HOST)/%: $(TEXTUTIL_DIR)/%.c $(TEXTUTIL_HDRS)
	@mkdir -p $(dir $@)
	@$(HOSTCC) -O2 -Wall -Wextra $(TEXTUTIL_HOST_CFLAGS_$*) -I$(TEXTUTIL_DIR) -o $@ $< $(TEXTUTIL_HOST_EXTRA_$*)
HOSTCC ?= cc

# The control: every tool built with -DTEXTUTIL_NEGCTL, which plants one
# realistic mistake per tool (chmod ignoring the umask for a who-less clause,
# du reporting bytes instead of allocation, ...). The runner must FAIL on it,
# and must fail for every tool that has a mutation -- a tool whose mutation no
# case catches has cases that measure nothing.
TEXTUTILS_HOST_NEG := $(BUILD)/textutils-host-negctl
$(TEXTUTILS_HOST_NEG)/%: $(TEXTUTIL_DIR)/%.c $(TEXTUTIL_HDRS)
	@mkdir -p $(dir $@)
	@$(HOSTCC) -O2 -w -DTEXTUTIL_NEGCTL $(TEXTUTIL_HOST_CFLAGS_$*) -I$(TEXTUTIL_DIR) -o $@ $< $(TEXTUTIL_HOST_EXTRA_$*)

.PHONY: test-textutils-host test-textutils-host-negctl
test-textutils-host-negctl: $(addprefix $(TEXTUTILS_HOST_NEG)/,$(TEXTUTILS))
	@python3 tests/textutils/run_host.py $(TEXTUTILS_HOST_NEG) > $(BUILD)/textutils_negctl.log 2>&1; \
	missed=""; for t in $(TEXTUTILS_MUTATED); do \
		grep -q "^FAIL \[[a-z]*\] $$t " $(BUILD)/textutils_negctl.log || missed="$$missed $$t"; done; \
	if [ -n "$$missed" ]; then cat $(BUILD)/textutils_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no case catches the planted mistake in:$$missed"; exit 1; fi; \
	echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/textutils_negctl.log) cases catch the planted mistakes in: $(TEXTUTILS_MUTATED)"

test-textutils-host: test-textutils-host-negctl $(addprefix $(TEXTUTILS_HOST)/,$(TEXTUTILS))
	@python3 tests/textutils/run_host.py $(TEXTUTILS_HOST)

ci-host: test-textutils-host

# ---- statvfs/df on the machine (T1) ----
STATVFS_CTL_BUILD := $(BUILD)-nostatfs
ifeq ($(NOSTATFS),1)
CFLAGS += -DLOGITFS_NO_STATFS
endif
.PHONY: test-statvfs test-statvfs-negctl
test-statvfs-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(STATVFS_CTL_BUILD) NOSTATFS=1 \
		$(STATVFS_CTL_BUILD)/logit.iso > $(BUILD)/statvfs_ctl_build.log 2>&1
	@bash tests/boot/run-statvfs-test.sh $(STATVFS_CTL_BUILD)/logit.iso $(DISK) --expect-enosys
test-statvfs: test-statvfs-negctl $(ISO) $(DISK)
	@bash tests/boot/run-statvfs-test.sh $(ISO) $(DISK)
ci-boot: test-statvfs
