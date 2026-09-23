# /bin/sh command-string contract. The host suite runs the real sh.c through
# main("-c") and the existing honest pipe model; the private guest packs the
# resulting AEX and checks files that the guest itself wrote. A serial marker
# alone would be weak evidence because the tty echoes commands as they arrive.
SH_COMMANDS_INC := -Itests/unit -Ic/apps/coreutils -Ic/apps -Iinclude/abi
SH_COMMANDS_BINS := $(BUILD)/login.aex $(BUILD)/sh.aex $(BUILD)/echo.aex \
                    $(BUILD)/false.aex

.PHONY: test-sh-commands test-sh-commands-negctl test-sh-commands-os

test-sh-commands-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DSH_COMMAND_LIST_NEGCTL \
	    -o $(BUILD)/sh_commands_negctl tests/unit/sh_commands_test.c $(SH_COMMANDS_INC)
	@if $(BUILD)/sh_commands_negctl >$(BUILD)/sh_commands_negctl.log 2>&1; then \
	    echo 'FAIL: the old single-line -c path passed the command-string suite'; exit 1; fi
	@n=$$(grep -c '^FAIL:' $(BUILD)/sh_commands_negctl.log); \
	 if [ "$$n" -ne 19 ]; then \
	    echo "FAIL: single-line control reddened $$n checks, expected 19"; \
	    grep '^FAIL:' $(BUILD)/sh_commands_negctl.log; exit 1; fi
	@echo 'negative control ok: old -c path fails 19 specific command-string checks'

test-sh-commands: test-sh-commands-negctl
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/sh_commands_test \
	    tests/unit/sh_commands_test.c $(SH_COMMANDS_INC)
	@$(BUILD)/sh_commands_test

test-sh-commands-os: test-sh-commands $(ISO) $(SH_COMMANDS_BINS) $(FONTS)
	@python3 tests/boot/run-sh-commands.py --build $(BUILD) \
	    --out $(BUILD)/sh-commands-guest

# The established shell host suite and ordinary CI both consume the new gate.
# This fragment is included by exec.mk so test-mk-wired audits the connection.
test-sh: test-sh-commands
ci-host: test-sh-commands
