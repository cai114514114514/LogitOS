# Same native text engine and dispatch branch; only ordinary valid font runs.
TVM_DIR := $(BUILD)/text-vertical-metrics
TVM_SRC := tests/unit/text_vertical_metrics_test.c $(sort $(FONT_WEIGHT_SRC))
TVM_INC := $(FONT_WEIGHT_INC)
.PHONY: test-text-vertical-metrics test-text-vertical-metrics-negctl
$(TVM_DIR)/current: $(TVM_SRC) c/apps/text_metrics_wiring.inc c/kernel/gui/text_measure_dispatch.inc include/abi/logit_abi.h
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TVM_INC) -o $@ $(TVM_SRC) -lm
$(TVM_DIR)/unsupported: $(TVM_SRC) c/apps/text_metrics_wiring.inc c/kernel/gui/text_measure_dispatch.inc include/abi/logit_abi.h
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DLOGIT_TEXT_METRICS_UNSUPPORTED $(TVM_INC) -o $@ $(TVM_SRC) -lm
test-text-vertical-metrics-negctl: $(TVM_DIR)/unsupported
	@$(TVM_DIR)/unsupported fsroot >$(TVM_DIR)/unsupported.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && test "$$(grep '^FAIL ' $(TVM_DIR)/unsupported.log)" = 'FAIL vertical metrics capability exists' || { cat $(TVM_DIR)/unsupported.log; exit 1; }; \
	 cat $(TVM_DIR)/unsupported.log
test-text-vertical-metrics: test-text-vertical-metrics-negctl $(TVM_DIR)/current
	@$(TVM_DIR)/current fsroot >$(TVM_DIR)/current.log 2>&1; rc=$$?; \
	 cat $(TVM_DIR)/current.log; test $$rc -eq 0
ci-host: test-text-vertical-metrics
