# SPDX-License-Identifier: MIT
# Reuse the streaming gate's production sources and transport. A missing
# fixture/compile error cannot satisfy either old-behavior negative control.
RESPONSE_BOUNDARY_DIR = $(BUILD)/response-boundary
RESPONSE_BOUNDARY_SRC = tests/unit/response_boundary_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
RESPONSE_BOUNDARY_DEPS = $(RESPONSE_BOUNDARY_SRC) tests/unit/stream_net.h tests/fixtures/browser/response-boundary.js $(RUST_LIB_HOST) $(wildcard c/apps/browser/*.inc c/apps/browser/*.h)
RESPONSE_BOUNDARY_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
# cookiegate.mk is included BEFORE STREAM_TEST_SRC is declared. Expanding the
# dependency list there silently loses js_webapi.c and leaves an old executable
# "up to date" after editing it. Resolve the authoritative source list later.
.SECONDEXPANSION:
$(RESPONSE_BOUNDARY_DIR)/current: $$(RESPONSE_BOUNDARY_DEPS)
	@mkdir -p $(RESPONSE_BOUNDARY_DIR)
	$(CC) $(RESPONSE_BOUNDARY_CF) -o $@ $(RESPONSE_BOUNDARY_SRC) $(RUST_LIB_HOST) -lm
$(RESPONSE_BOUNDARY_DIR)/public-internals: $$(RESPONSE_BOUNDARY_DEPS)
	@mkdir -p $(RESPONSE_BOUNDARY_DIR)
	$(CC) $(RESPONSE_BOUNDARY_CF) -DWEBAPI_RESPONSE_PUBLIC_INTERNALS -o $@ $(RESPONSE_BOUNDARY_SRC) $(RUST_LIB_HOST) -lm
$(RESPONSE_BOUNDARY_DIR)/public-network: $$(RESPONSE_BOUNDARY_DEPS)
	@mkdir -p $(RESPONSE_BOUNDARY_DIR)
	$(CC) $(RESPONSE_BOUNDARY_CF) -DWEBAPI_RESPONSE_PUBLIC_NETWORK -o $@ $(RESPONSE_BOUNDARY_SRC) $(RUST_LIB_HOST) -lm
$(RESPONSE_BOUNDARY_DIR)/lose-guard: $$(RESPONSE_BOUNDARY_DEPS)
	@mkdir -p $(RESPONSE_BOUNDARY_DIR)
	$(CC) $(RESPONSE_BOUNDARY_CF) -DWEBAPI_RESPONSE_LOSE_GUARD -o $@ $(RESPONSE_BOUNDARY_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-response-boundary test-response-boundary-negctl
test-response-boundary-negctl: $(RESPONSE_BOUNDARY_DIR)/public-internals $(RESPONSE_BOUNDARY_DIR)/public-network $(RESPONSE_BOUNDARY_DIR)/lose-guard
	@rc=0; $(RESPONSE_BOUNDARY_DIR)/public-internals > $(RESPONSE_BOUNDARY_DIR)/public-internals.log 2>&1 || rc=$$?; \
	 grep '^FAIL:' $(RESPONSE_BOUNDARY_DIR)/public-internals.log; test $$rc -eq 1 && \
	 grep -q '^FAIL: public internal flag cannot bypass validation$$' $(RESPONSE_BOUNDARY_DIR)/public-internals.log && \
	 grep -q '^ok  : opaque fetch resolves$$' $(RESPONSE_BOUNDARY_DIR)/public-internals.log && \
	 grep -q '^ok  : shared page reaches completion$$' $(RESPONSE_BOUNDARY_DIR)/public-internals.log
	@rc=0; $(RESPONSE_BOUNDARY_DIR)/public-network > $(RESPONSE_BOUNDARY_DIR)/public-network.log 2>&1 || rc=$$?; \
	 grep '^FAIL:' $(RESPONSE_BOUNDARY_DIR)/public-network.log; test $$rc -eq 1 && \
	 grep -q '^FAIL: opaque fetch resolves$$' $(RESPONSE_BOUNDARY_DIR)/public-network.log && \
	 grep -q '^ok  : public internal flag cannot bypass validation$$' $(RESPONSE_BOUNDARY_DIR)/public-network.log && \
	 grep -q '^ok  : shared page reaches completion$$' $(RESPONSE_BOUNDARY_DIR)/public-network.log
	@rc=0; $(RESPONSE_BOUNDARY_DIR)/lose-guard > $(RESPONSE_BOUNDARY_DIR)/lose-guard.log 2>&1 || rc=$$?; \
	 grep '^FAIL:' $(RESPONSE_BOUNDARY_DIR)/lose-guard.log; test $$rc -eq 1 && \
	 grep -q '^FAIL: network clone retains immutable headers$$' $(RESPONSE_BOUNDARY_DIR)/lose-guard.log && \
	 grep -q '^ok  : opaque fetch resolves$$' $(RESPONSE_BOUNDARY_DIR)/lose-guard.log && \
	 grep -q '^ok  : shared page reaches completion$$' $(RESPONSE_BOUNDARY_DIR)/lose-guard.log
test-response-boundary: test-response-boundary-negctl $(RESPONSE_BOUNDARY_DIR)/current
	@$(RESPONSE_BOUNDARY_DIR)/current
ci-host: test-response-boundary
test-cookie-cors: test-response-boundary
