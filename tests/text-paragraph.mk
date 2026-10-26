# tests/text-paragraph.mk -- c/lib/text/para.{h,c} (tl_para_layout): greedy wrap
# over tl_break_utf8, max lines, head/middle/tail ellipsis at cluster boundaries.
#
#   make test-text-paragraph          fake fixed-advance and CJK-wide carets
#                                     callbacks vs a brute-force greedy reference
#   make test-text-paragraph-negctl   SAME sources and flags with -DTL_PARA_BYTE_CUT
#                                     (truncate at a byte index); MUST fail with
#                                     'FAIL cluster-split'. A prerequisite of the
#                                     positive target.

TPARA_SRC := tests/unit/para_test.c c/lib/text/para.c c/lib/text/linebreak.c
TPARA_INC := -Ic/lib/text
TPARA_CFL := -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined

.PHONY: test-text-paragraph test-text-paragraph-negctl

test-text-paragraph-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(TPARA_CFL) -DTL_PARA_BYTE_CUT -o $(BUILD)/para_test_negctl $(TPARA_SRC) $(TPARA_INC)
	@if $(BUILD)/para_test_negctl >$(BUILD)/para_test_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes a byte-index truncation"; exit 1; \
	 elif ! grep -q '^FAIL cluster-split' $(BUILD)/para_test_negctl.log; then \
	    cat $(BUILD)/para_test_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL cluster-split'"; exit 1; \
	 else \
	    echo "negative control ok: $$(grep -c '^FAIL cluster-split' $(BUILD)/para_test_negctl.log) cluster-split failures with byte-index cuts"; \
	    grep -m1 '^FAIL cluster-split' $(BUILD)/para_test_negctl.log | sed 's/^/      /'; \
	 fi

test-text-paragraph: test-text-paragraph-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(TPARA_CFL) -o $(BUILD)/para_test $(TPARA_SRC) $(TPARA_INC)
	@$(BUILD)/para_test

ci-host: test-text-paragraph
