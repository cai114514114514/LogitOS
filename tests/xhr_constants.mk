# SPDX-License-Identifier: MIT
# Share WEBAPI_TEST_SRC and its seven-byte in-memory transport. The only product
# seam in the control is omission of prototype constants; body delivery remains.
XHR_CONSTANTS_SRC := tests/unit/xhr_constants_test.c $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC))
XHR_CONSTANTS_DEP := $(XHR_CONSTANTS_SRC) tests/unit/webapi_test.c tests/xhr_constants.mk
XHR_CONSTANTS_DIR := $(BUILD)/xhr-constants
$(XHR_CONSTANTS_DIR)/current: $(XHR_CONSTANTS_DEP) $(RUST_LIB_HOST)
	@mkdir -p $(XHR_CONSTANTS_DIR)
	@$(CC) -O1 -g -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $@ $(XHR_CONSTANTS_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
$(XHR_CONSTANTS_DIR)/old: $(XHR_CONSTANTS_DEP) $(RUST_LIB_HOST)
	@mkdir -p $(XHR_CONSTANTS_DIR)
	@$(CC) -O1 -g -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DXHR_CONSTANTS_CONSTRUCTOR_ONLY -o $@ $(XHR_CONSTANTS_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-xhr-constants test-xhr-constants-negctl
test-xhr-constants-negctl: $(XHR_CONSTANTS_DIR)/old
	@rc=0; $(XHR_CONSTANTS_DIR)/old > $(XHR_CONSTANTS_DIR)/old.log 2>&1 || rc=$$?; \
	 cat $(XHR_CONSTANTS_DIR)/old.log; test $$rc -eq 1 && \
	 rg -q '^xhr-constants: 15 checks, 9 failures$$' $(XHR_CONSTANTS_DIR)/old.log && \
	 rg -q '^FAIL: one-shot instance-constant header hook fires once$$' $(XHR_CONSTANTS_DIR)/old.log && \
	 rg -q '^ok  : progress delivers body independently of header hook$$' $(XHR_CONSTANTS_DIR)/old.log
test-xhr-constants: test-xhr-constants-negctl $(XHR_CONSTANTS_DIR)/current
	@$(XHR_CONSTANTS_DIR)/current
ci-host: test-xhr-constants
test-webapi: test-xhr-constants
