# SPDX-License-Identifier: MIT
COOKIE_PERSIST_SRC = tests/unit/cookie_persistence_test.c c/apps/gui/browser/storage/cookies.c c/net/http/cookies.c
COOKIE_PERSIST_DEPS = $(COOKIE_PERSIST_SRC) c/apps/gui/browser/storage/cookies.h c/apps/gui/browser/app/tabs.h c/net/http/cookies.h
COOKIE_PERSIST_INC = $(BROWSER_INC) -Ic/net/http
.PHONY: test-cookie-persistence test-cookie-persistence-negctl test-cookie-persistence-prefix-negctl
$(BUILD)/cookie_persistence_test: $(COOKIE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_PERSIST_SRC)
$(BUILD)/cookie_persistence_negctl: $(COOKIE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DCOOKIE_PERSIST_NO_WRITE $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_PERSIST_SRC)
$(BUILD)/cookie_persistence_prefix_negctl: $(COOKIE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DCOOKIE_PERSIST_PREFIX_READ $(COOKIE_PERSIST_INC) -o $@ $(COOKIE_PERSIST_SRC)
test-cookie-persistence-negctl: $(BUILD)/cookie_persistence_negctl
	@d=$$(mktemp -d); rc=0; $< write "$$d" > $(BUILD)/cookie_persistence_negctl.log 2>&1 && $< read "$$d" >> $(BUILD)/cookie_persistence_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/cookie_persistence_negctl.log; rm -rf "$$d"; test $$rc -eq 1 && grep -q '^FAIL: new process restores exact persistent cookie' $(BUILD)/cookie_persistence_negctl.log
test-cookie-persistence-prefix-negctl: $(BUILD)/cookie_persistence_prefix_negctl
	@d=$$(mktemp -d); rc=0; $< write "$$d" > $(BUILD)/cookie_persistence_prefix_negctl.log 2>&1 && $< read "$$d" >> $(BUILD)/cookie_persistence_prefix_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/cookie_persistence_prefix_negctl.log; rm -rf "$$d"; test $$rc -eq 1 && grep -q '^FAIL: new process restores exact persistent cookie' $(BUILD)/cookie_persistence_prefix_negctl.log
test-cookie-persistence: test-cookie-persistence-negctl test-cookie-persistence-prefix-negctl $(BUILD)/cookie_persistence_test
	@$(BUILD)/cookie_persistence_test
	@d=$$(mktemp -d); rc=0; $(BUILD)/cookie_persistence_test write "$$d" && $(BUILD)/cookie_persistence_test read "$$d" || rc=$$?; rm -rf "$$d"; exit $$rc

$(BUILD)/cookie_store_guest_test: tests/unit/cookie_store_guest_test.c c/apps/gui/browser/storage/cookie_guest.inc c/apps/gui/browser/storage/cookies.h include/abi/logit_abi.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(COOKIE_PERSIST_INC) -Iinclude/abi -o $@ $<
.PHONY: test-cookie-store-guest-adapter
test-cookie-store-guest-adapter: $(BUILD)/cookie_store_guest_test
	@$<
test-cookie-persistence: test-cookie-store-guest-adapter

# Explicit immutable images: the driver makes its own writable copy and boots
# that copy twice without -snapshot. A baseline must actually seed both Cookie
# doors before absence after reopen counts as the negative control.
COOKIE_PERSIST_GUEST_ISO ?=
COOKIE_PERSIST_GUEST_DISK ?=
COOKIE_PERSIST_GUEST_NEG_DISK ?=

ci-host: test-cookie-persistence
