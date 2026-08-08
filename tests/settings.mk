# tests/settings.mk -- the settings store: does this machine remember its user?
#
# Own fragment rather than lines in the Makefile, for the reason the other
# fragments exist: a whole-file Makefile overwrite from a concurrent line
# deletes targets written straight into it.
#
# Every target here has a RECIPE, which is what makes tools/audit_tests.py
# classify it and `tools/ci.sh` run it -- the suite list is derived from the
# Makefile, not written down, so nothing has to be remembered to include these.
# test-settings-negctl matches NOT_CI (it is a negative control) and is run BY
# its positive counterpart rather than by the suite, which is the convention the
# audit expects.

.PHONY: test-settings test-settings-host test-settings-negctl test-settings-os \
        test-settings-ui

# The host flags mirror the LogitFS host tests: ASan + UBSan, because a parser
# whose whole job is to survive arbitrary bytes is exactly where an off-by-one
# hides, and a sweep over every byte offset of four files is a good way to find
# one.
SET_CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-function \
              -fsanitize=address,undefined -fno-omit-frame-pointer
SET_INC    := -Ic/kernel/core -Ic/fs -Ic/drivers/block -Iinclude/abi -Ic/kernel/mm
SET_SRC    := tests/unit/settings_test.c c/kernel/core/settings.c c/drivers/block/crc32.c

# The truncate-at-every-byte-offset sweep, the garbage battery, the frame codec
# and the commit round trip, against the REAL c/kernel/core/settings.c.
#
# It runs its own negative controls as a prerequisite of passing: a positive
# result here means nothing unless the same assertions have been watched fail.
test-settings-host: test-settings-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(SET_CFLAGS) $(SET_INC) -o $(BUILD)/settings_test $(SET_SRC)
	@$(BUILD)/settings_test

# TWO negative controls, each disabling ONE claim, each REQUIRED to fail:
#
#   SETTINGS_NO_TRUNC_GUARD  parses an unterminated final line. That single rule
#                            is what makes truncation safe by construction; with
#                            it gone, a file cut mid-token yields a value nobody
#                            wrote. Without this control the sweep would pass
#                            for a parser that had never had the rule.
#   SETTINGS_NO_RANGE_CHECK  returns stored values unchecked, so a hand-edited
#                            `ui.dark = -1` reaches the compositor. This is the
#                            control for "range-check on READ, not on write".
#
# The test binary itself inverts its exit status under either flag, so a control
# that PASSES is reported as the failure it is.
test-settings-negctl:
	@mkdir -p $(BUILD)
	@echo "--- negative control 1: no unterminated-line guard (must fail) ---"
	@$(CC) $(SET_CFLAGS) $(SET_INC) -DSETTINGS_NO_TRUNC_GUARD \
	    -o $(BUILD)/settings_negctl1 $(SET_SRC)
	@$(BUILD)/settings_negctl1 > $(BUILD)/settings_negctl1.log 2>&1; \
	  rc=$$?; tail -3 $(BUILD)/settings_negctl1.log; \
	  if [ $$rc -ne 0 ]; then \
	    echo "test-settings-negctl: FAIL -- control 1 did not fail as required"; exit 1; fi
	@echo "--- negative control 2: no range check on read (must fail) ---"
	@$(CC) $(SET_CFLAGS) $(SET_INC) -DSETTINGS_NO_RANGE_CHECK \
	    -o $(BUILD)/settings_negctl2 $(SET_SRC)
	@$(BUILD)/settings_negctl2 > $(BUILD)/settings_negctl2.log 2>&1; \
	  rc=$$?; tail -3 $(BUILD)/settings_negctl2.log; \
	  if [ $$rc -ne 0 ]; then \
	    echo "test-settings-negctl: FAIL -- control 2 did not fail as required"; exit 1; fi
	@echo "test-settings-negctl: both controls failed as required"

# The one that matters: SET IT, REBOOT WITHOUT -snapshot, CHECK IT. Six real
# boots against one disk image. A settings system tested without a real reboot
# has not been tested.
test-settings-os: $(ISO) $(DISK)
	@bash tests/boot/run-settings-test.sh $(ISO) $(DISK)

# The Settings window itself, driven over QMP, screenshotted in both themes.
test-settings-ui: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_settings.py

test-settings: test-settings-host test-settings-os test-settings-ui
