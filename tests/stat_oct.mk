# The real stat command is included in the host test with fake metadata
# syscalls.  The assertions inspect side effects, because the old parser could
# print success while quietly applying mode/mask zero.
STAT_OCT_SRC := tests/unit/stat_oct_test.c c/apps/coreutils/stat.c
STAT_OCT_FLAGS := -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
                  -fno-omit-frame-pointer -Iinclude/abi -Ic/apps

.PHONY: test-stat-oct test-stat-oct-negctl

$(BUILD)/stat_oct_test: $(STAT_OCT_SRC)
	@mkdir -p $(BUILD)
	@$(CC) $(STAT_OCT_FLAGS) -o $@ tests/unit/stat_oct_test.c

$(BUILD)/stat_oct_negctl: $(STAT_OCT_SRC)
	@mkdir -p $(BUILD)
	@$(CC) $(STAT_OCT_FLAGS) -DSTAT_OCT_NEGCTL -o $@ tests/unit/stat_oct_test.c

test-stat-oct-negctl: $(BUILD)/stat_oct_negctl
	@if $(BUILD)/stat_oct_negctl >$(BUILD)/stat_oct_negctl.log 2>&1; then \
	    echo 'FAIL: permissive octal parser passed the side-effect suite'; exit 1; fi
	@grep -q "FAIL -u '9' rejects without changing metadata" $(BUILD)/stat_oct_negctl.log
	@grep -q "FAIL -c '9' rejects without changing metadata" $(BUILD)/stat_oct_negctl.log
	@echo 'negative control ok: invalid umask and chmod both change metadata'

test-stat-oct: test-stat-oct-negctl $(BUILD)/stat_oct_test
	@$(BUILD)/stat_oct_test

# The established CLI host suite and CI consume the new gate.
test-sh: test-stat-oct
ci-host: test-stat-oct
