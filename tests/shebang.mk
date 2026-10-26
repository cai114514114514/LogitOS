# tests/shebang.mk -- execve runs a `#!` script with its interpreter
# (c/kernel/exec/load/shebang.h, proc_execve in exec.c).
#
# test-shebang-host: the parser against Linux's rules, case by case. The
# control keeps a CRLF file's trailing CR in the interpreter name and must fail.
#
# test-shebang: on the machine -- a script written by the shell, made
# executable with /bin/chmod, run by path with arguments. The control kernel is
# built with -DEXEC_NEGCTL_NO_SHEBANG and must refuse the same script.

.PHONY: test-shebang-host test-shebang-host-negctl test-shebang test-shebang-negctl

SHEBANG_INC := -Ic/kernel/exec/load

test-shebang-host-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DSHEBANG_NEGCTL_NO_TRIM $(SHEBANG_INC) \
		-o $(BUILD)/shebang_negctl tests/unit/shebang_test.c
	@if $(BUILD)/shebang_negctl > $(BUILD)/shebang_negctl.log 2>&1; then \
		cat $(BUILD)/shebang_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: the suite passes with a CR in the interpreter"; exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/shebang_negctl.log) CRLF checks fail"; \
	fi

test-shebang-host: test-shebang-host-negctl
	@$(CC) -O2 -Wall -Wextra $(SHEBANG_INC) -o $(BUILD)/shebang_test tests/unit/shebang_test.c
	@$(BUILD)/shebang_test

ci-host: test-shebang-host

SHEBANG_CTL_BUILD := $(BUILD)-noshebang
ifeq ($(NOSHEBANG),1)
CFLAGS += -DEXEC_NEGCTL_NO_SHEBANG
endif

test-shebang-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(SHEBANG_CTL_BUILD) NOSHEBANG=1 \
		$(SHEBANG_CTL_BUILD)/logit.iso > $(BUILD)/shebang_ctl_build.log 2>&1
	@bash tests/boot/run-shebang-test.sh $(SHEBANG_CTL_BUILD)/logit.iso $(DISK) --expect-refused

test-shebang: test-shebang-host test-shebang-negctl $(ISO) $(DISK)
	@bash tests/boot/run-shebang-test.sh $(ISO) $(DISK)

ci-boot: test-shebang

# /bin/as reads its version header from line 2 when line 1 is `#!`.
AS_VER_SRC := tests/unit/as_version_shebang_test.c c/apps/as/common/version.c
test-as-version-shebang-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_VERSION_NEGCTL_NO_SHEBANG -Ic/apps/as/common \
		-o $(BUILD)/as_ver_sb_negctl $(AS_VER_SRC)
	@if $(BUILD)/as_ver_sb_negctl > $(BUILD)/as_ver_sb_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without the #! skip"; exit 1; \
	else echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/as_ver_sb_negctl.log) checks fail without the skip"; fi
test-as-version-shebang: test-as-version-shebang-negctl
	@$(CC) -O2 -Wall -Wextra -Ic/apps/as/common -o $(BUILD)/as_ver_sb $(AS_VER_SRC)
	@$(BUILD)/as_ver_sb
test-shebang-host: test-as-version-shebang
.PHONY: test-as-version-shebang test-as-version-shebang-negctl
