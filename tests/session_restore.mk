# Persistence-only slice of the loader's real source list; no copied browser shim.
SESSION_RESTORE_SRC = $(filter c/apps/browser/tabs.c,$(LOADER_SRC)) tests/unit/session_restore_test.c
SESSION_RESTORE_DIR := $(BUILD)/session-restore
.PHONY: test-session-restore test-session-restore-negctl
$(SESSION_RESTORE_DIR)/test: $(SESSION_RESTORE_SRC) c/apps/browser/tabs.h
	@mkdir -p $(SESSION_RESTORE_DIR)
	$(CC) -O2 -Wall -Wextra -Ic/apps/browser -o $@ $(SESSION_RESTORE_SRC)
$(SESSION_RESTORE_DIR)/negctl: $(SESSION_RESTORE_SRC) c/apps/browser/tabs.h
	@mkdir -p $(SESSION_RESTORE_DIR)
	$(CC) -O2 -Wall -Wextra -DTABS_RESTORE_LEGACY -Ic/apps/browser -o $@ $(SESSION_RESTORE_SRC)
test-session-restore-negctl: $(SESSION_RESTORE_DIR)/negctl
	@$(SESSION_RESTORE_DIR)/negctl > $(SESSION_RESTORE_DIR)/negctl.log 2>&1; rc=$$?; cat $(SESSION_RESTORE_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: disk position awaits first layout' $(SESSION_RESTORE_DIR)/negctl.log && grep -q 'FAIL: first successful layout consumes restore' $(SESSION_RESTORE_DIR)/negctl.log
test-session-restore: test-session-restore-negctl $(SESSION_RESTORE_DIR)/test
	@$(SESSION_RESTORE_DIR)/test
