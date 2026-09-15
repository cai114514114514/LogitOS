# SPDX-License-Identifier: MIT
# Preserve the existing H2 protocol/source authority; swap only its main and
# compile the real adapter through a one-call allocation-injection wrapper.
H2_HEADER_COPY_SRC := tests/unit/h2_header_copy_test.c tests/unit/h2_header_copy_runtime.c $(filter-out tests/unit/h2mux_test.c c/apps/browser/browser_rt.c,$(H2MUX_SRC))
H2_HEADER_COPY_DEPS := $(H2_HEADER_COPY_SRC) tests/unit/h2mux_test.c c/apps/browser/browser_rt.c c/apps/browser/bfetch.h c/net/http/http1.h c/net/http/http2.h tests/h2_header_copy.mk
H2_HEADER_COPY_DIR := $(BUILD)/h2-header-copy
$(H2_HEADER_COPY_DIR)/current: $(H2_HEADER_COPY_DEPS)
	@mkdir -p $(H2_HEADER_COPY_DIR)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $@ $(H2_HEADER_COPY_SRC)
$(H2_HEADER_COPY_DIR)/old: $(H2_HEADER_COPY_DEPS)
	@mkdir -p $(H2_HEADER_COPY_DIR)
	@$(CC) -O1 -g -w -DBXFER_PARTIAL_HEADERS_OK $(H2MUX_INC) -o $@ $(H2_HEADER_COPY_SRC)
.PHONY: test-h2-header-copy test-h2-header-copy-negctl
test-h2-header-copy-negctl: h2mux-link-check $(H2_HEADER_COPY_DIR)/old
	@rc=0; $(H2_HEADER_COPY_DIR)/old > $(H2_HEADER_COPY_DIR)/old.log 2>&1 || rc=$$?; \
	 cat $(H2_HEADER_COPY_DIR)/old.log; test $$rc -eq 1 && \
	 rg -q '^h2-header-copy: 30 checks, 9 failures$$' $(H2_HEADER_COPY_DIR)/old.log && \
	 rg -q '^FAIL: stream failed copy never declares complete headers$$' $(H2_HEADER_COPY_DIR)/old.log && \
	 rg -q '^FAIL: stream failed copy delivers no body$$' $(H2_HEADER_COPY_DIR)/old.log && \
	 rg -q '^FAIL: embedded failed copy never marks policies known$$' $(H2_HEADER_COPY_DIR)/old.log
test-h2-header-copy: test-h2-header-copy-negctl $(H2_HEADER_COPY_DIR)/current
	@$(H2_HEADER_COPY_DIR)/current
ci-host: test-h2-header-copy
