BROWSER_DOWNLOAD_DEPS := c/apps/browser/tabs.c c/apps/browser/tabs.h c/apps/download_name.h tests/unit/browser_downloads_test.c
$(BUILD)/browser_downloads_test: $(BROWSER_DOWNLOAD_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -Ic/apps/browser c/apps/browser/tabs.c tests/unit/browser_downloads_test.c -o $@
$(BUILD)/browser_downloads_neg: $(BROWSER_DOWNLOAD_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -DBROWSER_DOWNLOAD_NEGCTL -Ic/apps/browser c/apps/browser/tabs.c tests/unit/browser_downloads_test.c -o $@
.PHONY: test-browser-downloads test-browser-downloads-negctl
test-browser-downloads-negctl: $(BUILD)/browser_downloads_neg
	@rc=0; $(BUILD)/browser_downloads_neg > $(BUILD)/browser_downloads_neg.log 2>&1 || rc=$$?; cat $(BUILD)/browser_downloads_neg.log; test $$rc -eq 1 && grep -q 'FAIL first committed download' $(BUILD)/browser_downloads_neg.log
test-browser-downloads: test-browser-downloads-negctl $(BUILD)/browser_downloads_test
	@$(BUILD)/browser_downloads_test
ci-host: test-browser-downloads

# Boot the product browser, click native links/buttons, then inspect bytes on
# its private writable disk. Source image hashes must remain unchanged.
.PHONY: test-download-keys-os
test-download-keys-os: test-web-crypto-keys test-browser-downloads $(ISO) $(DISK)
	python3 tests/qmp/download_keys_guest.py --build $(BUILD)
ci-boot: test-download-keys-os
