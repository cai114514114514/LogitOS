# The same real browser_rt and protocol sources as h2mux, with a stateful
# server fixture. The control removes ONLY the production invalidation call.
CACHE_INVALIDATION_SRC := tests/unit/cache_invalidation_test.c $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC))
.PHONY: test-cache-invalidation test-cache-invalidation-negctl

test-cache-invalidation: test-cache-invalidation-negctl h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/cache_invalidation $(CACHE_INVALIDATION_SRC)
	@$(BUILD)/cache_invalidation

test-cache-invalidation-negctl: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_NO_CACHE_INVALIDATE $(H2MUX_INC) -o $(BUILD)/cache_invalidation_negctl $(CACHE_INVALIDATION_SRC)
	@set +e; $(BUILD)/cache_invalidation_negctl >$(BUILD)/cache_invalidation_negctl.log 2>&1; rc=$$?; set -e; \
	  if [ $$rc -ne 1 ] || ! grep -q 'FAIL.*cache-invalidation: GET stale body' $(BUILD)/cache_invalidation_negctl.log; then \
	    cat $(BUILD)/cache_invalidation_negctl.log; echo "cache-invalidation control did not detect stale GET"; exit 1; fi; \
	  echo "cache-invalidation negative control: observed stale GET after mutation"; \
	  grep 'cache-invalidation:.*failures' $(BUILD)/cache_invalidation_negctl.log

.PHONY: test-cache-invalidation-asan
test-cache-invalidation-asan: h2mux-link-check
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(H2MUX_INC) -o $(BUILD)/cache_invalidation_asan $(CACHE_INVALIDATION_SRC)
	@$(BUILD)/cache_invalidation_asan

EMPTY_RESOURCE_SRC = tests/unit/empty_resource_test.c $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC))
.PHONY: test-empty-resource test-empty-resource-negctl
test-empty-resource-negctl: h2mux-link-check
	@mkdir -p $(BUILD)/site-general/script-resources
	@$(CC) -O1 -g -w -DBFETCH_EMPTY_TAKE_LEGACY $(H2MUX_INC) -o $(BUILD)/site-general/script-resources/empty-old $(EMPTY_RESOURCE_SRC)
	@rc=0; $(BUILD)/site-general/script-resources/empty-old > $(BUILD)/site-general/script-resources/empty-old.log 2>&1 || rc=$$?; tail -8 $(BUILD)/site-general/script-resources/empty-old.log; test $$rc -eq 1 && grep -q 'empty-resource completed response take' $(BUILD)/site-general/script-resources/empty-old.log
test-empty-resource: test-empty-resource-negctl
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/site-general/script-resources/empty $(EMPTY_RESOURCE_SRC)
	@$(BUILD)/site-general/script-resources/empty
