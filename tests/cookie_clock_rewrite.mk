COOKIE_REWRITE_DIR = $(BUILD)/cookie-clock-rewrite
COOKIE_REWRITE_SRC = tests/unit/cookie_clock_rewrite_test.c c/apps/browser/cookie_persistence.c c/net/http/cookies.c
COOKIE_REWRITE_DEP = $(COOKIE_REWRITE_SRC) $(COOKIE_PERSIST_DEPS) tests/cookie_clock_rewrite.mk
$(COOKIE_REWRITE_DIR)/current: $(COOKIE_REWRITE_DEP)
	@mkdir -p $(COOKIE_REWRITE_DIR)
	$(CC) -O2 -Wall -Wextra $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_REWRITE_SRC)
$(COOKIE_REWRITE_DIR)/old: $(COOKIE_REWRITE_DEP)
	@mkdir -p $(COOKIE_REWRITE_DIR)
	$(CC) -O2 -Wall -Wextra -DCOOKIE_PERSIST_GLOBAL_CEILING $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_REWRITE_SRC)
.PHONY: test-cookie-clock-rewrite test-cookie-clock-rewrite-san
test-cookie-clock-rewrite: $(COOKIE_REWRITE_DIR)/old $(COOKIE_REWRITE_DIR)/current
	@rc=0; $(COOKIE_REWRITE_DIR)/old > $(COOKIE_REWRITE_DIR)/old.log 2>&1 || rc=$$?; cat $(COOKIE_REWRITE_DIR)/old.log; test $$rc -eq 1 && rg -q '^FAIL: saved mixed-age records reopen without header access$$' $(COOKIE_REWRITE_DIR)/old.log && rg -q '^cookie-clock-rewrite: 8 checks, 2 failures$$' $(COOKIE_REWRITE_DIR)/old.log
	@$(COOKIE_REWRITE_DIR)/current
$(COOKIE_REWRITE_DIR)/san: $(COOKIE_REWRITE_DEP)
	@mkdir -p $(COOKIE_REWRITE_DIR)
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_REWRITE_SRC)
test-cookie-clock-rewrite-san: test-cookie-clock-rewrite $(COOKIE_REWRITE_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(COOKIE_REWRITE_DIR)/san
ci-host: test-cookie-clock-rewrite
