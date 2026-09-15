# Pure local policy fixtures. No remote requests, auth flows or widget solving.
IFRAME_POLICY_SRC := tests/unit/iframe_policy_test.c c/apps/browser/iframe_policy.c c/apps/browser/js_url.c
IFRAME_POLICY_DEP := $(IFRAME_POLICY_SRC) c/apps/browser/iframe_policy.h c/apps/browser/js_url.h
$(BUILD)/iframe_policy_test: $(IFRAME_POLICY_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DURL_CORE_ONLY -Ic/apps/browser $(IFRAME_POLICY_SRC) -o $@
$(BUILD)/iframe_policy_negctl: $(IFRAME_POLICY_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -DURL_CORE_ONLY -DIFRAME_POLICY_NO_ENFORCEMENT -Ic/apps/browser $(IFRAME_POLICY_SRC) -o $@
.PHONY: test-iframe-policy test-iframe-policy-negctl
test-iframe-policy-negctl: $(BUILD)/iframe_policy_negctl
	@rc=0; $< control > $(BUILD)/iframe_policy_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && \
	 rg -q '^FAIL: deny XFO$$' $(BUILD)/iframe_policy_negctl.log && \
	 rg -q '^FAIL: deny parent source$$' $(BUILD)/iframe_policy_negctl.log && \
	 rg -q '^FAIL: deny child ancestors$$' $(BUILD)/iframe_policy_negctl.log && \
	 rg '^iframe-policy: 3 checks, 3 failures$$' $(BUILD)/iframe_policy_negctl.log
test-iframe-policy: test-iframe-policy-negctl $(BUILD)/iframe_policy_test
	@$(BUILD)/iframe_policy_test
ci-host: test-iframe-policy
