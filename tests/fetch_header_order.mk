# Derive the complete transport list from the existing real-adapter gate;
# only replace its main (its socket/server fixture is textually reused).
FETCH_HEADER_SRC := tests/unit/fetch_header_order_test.c $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC))
.PHONY: test-fetch-header-order test-fetch-header-order-negctl

test-fetch-header-order: test-fetch-header-order-negctl h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/fetch_header_order $(FETCH_HEADER_SRC)
	@$(BUILD)/fetch_header_order

test-fetch-header-order-negctl: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_HEADERS_WITH_BODY $(H2MUX_INC) -o $(BUILD)/fetch_header_order_negctl $(FETCH_HEADER_SRC)
	@set +e; $(BUILD)/fetch_header_order_negctl >$(BUILD)/fetch_header_order_negctl.log 2>&1; rc=$$?; set -e; \
	  if [ $$rc -ne 1 ] || ! grep -q 'FAIL.*header-order: sink called' $(BUILD)/fetch_header_order_negctl.log; then \
	    cat $(BUILD)/fetch_header_order_negctl.log; echo "fetch-header-order control did not catch the header/body boundary"; exit 1; fi; \
	  echo "fetch-header-order negative control: observed pre-header body failure"; \
	  grep 'FAIL.*header-order:' $(BUILD)/fetch_header_order_negctl.log
