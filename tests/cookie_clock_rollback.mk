# A real format decoder/core, with only the former clock-corruption rule
# restored in the negative. Every malformed-record assertion stays enabled.
COOKIE_CLOCK_DIR = $(BUILD)/cookie-clock
COOKIE_CLOCK_SRC = tests/unit/cookie_clock_rollback_test.c c/apps/browser/cookie_persistence.c c/net/http/cookies.c
COOKIE_CLOCK_DEPS = $(COOKIE_CLOCK_SRC) $(COOKIE_PERSIST_DEPS) tests/cookie_clock_rollback.mk
$(COOKIE_CLOCK_DIR)/current: $(COOKIE_CLOCK_DEPS)
	@mkdir -p $(COOKIE_CLOCK_DIR)
	$(CC) -O2 -Wall -Wextra $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_CLOCK_SRC)
$(COOKIE_CLOCK_DIR)/old: $(COOKIE_CLOCK_DEPS)
	@mkdir -p $(COOKIE_CLOCK_DIR)
	$(CC) -O2 -Wall -Wextra -DCOOKIE_PERSIST_CLOCK_STRICT $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_CLOCK_SRC)
.PHONY: test-cookie-clock-rollback test-cookie-clock-rollback-negctl test-cookie-clock-rollback-san
test-cookie-clock-rollback-negctl: $(COOKIE_CLOCK_DIR)/old
	@rc=0; $< > $(COOKIE_CLOCK_DIR)/old.log 2>&1 || rc=$$?; cat $(COOKIE_CLOCK_DIR)/old.log; test $$rc -eq 1 && grep -q '^cookie-clock: 47 checks, 13 failures$$' $(COOKIE_CLOCK_DIR)/old.log && grep -q '^FAIL: clock rollback restores both validated records$$' $(COOKIE_CLOCK_DIR)/old.log && ! grep -Eq '^FAIL: rollback (never|cannot|validates)' $(COOKIE_CLOCK_DIR)/old.log
test-cookie-clock-rollback: test-cookie-clock-rollback-negctl $(COOKIE_CLOCK_DIR)/current
	@$(COOKIE_CLOCK_DIR)/current
$(COOKIE_CLOCK_DIR)/san: $(COOKIE_CLOCK_DEPS)
	@mkdir -p $(COOKIE_CLOCK_DIR)
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_CLOCK_SRC)
test-cookie-clock-rollback-san: test-cookie-clock-rollback $(COOKIE_CLOCK_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(COOKIE_CLOCK_DIR)/san
ci-host: test-cookie-clock-rollback
