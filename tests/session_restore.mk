# Persistence-only slice of the loader's real source list; no copied browser shim.
SESSION_RESTORE_SRC = $(filter c/apps/gui/browser/app/tabs.c,$(LOADER_SRC)) tests/unit/session_restore_test.c
SESSION_RESTORE_DIR := $(BUILD)/session-restore
$(SESSION_RESTORE_DIR)/test: $(SESSION_RESTORE_SRC) c/apps/gui/browser/app/tabs.h
	@mkdir -p $(SESSION_RESTORE_DIR)
	$(CC) -O2 -Wall -Wextra $(BROWSER_INC) -o $@ $(SESSION_RESTORE_SRC)
$(SESSION_RESTORE_DIR)/negctl: $(SESSION_RESTORE_SRC) c/apps/gui/browser/app/tabs.h
	@mkdir -p $(SESSION_RESTORE_DIR)
	$(CC) -O2 -Wall -Wextra -DTABS_RESTORE_LEGACY $(BROWSER_INC) -o $@ $(SESSION_RESTORE_SRC)
