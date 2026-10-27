# tests/ui-aui-text-api.mk -- the aui text API of design P1 / P2 / 3.6
# (c/apps/gui/aui_text.{h,inc}): weighted entry points (aui_text_w 6-arg,
# aui_text_in_w, aui_text_ellipsis_w, aui_text_measure_w), tabular digits
# (aui_text_tnum, aui_text_tnum_width, cell cache per px/weight/scale),
# aui_text_baseline24 / aui_text_vc, aui_text_wrap_lh / aui_text_measure_wrap_lh.
#
# The test #includes the real aui.c over the host transport with
# tests/unit/aui_host/v2_text.c registered and records every text run; a fake
# proportional font (digits of different widths, bold 8 % wider) answers the
# width call and the carets query. The control builds the SAME sources and flags
# plus -DUI_TEXT_API_NEGCTL (digits at their natural advances, no cells) and must
# fail the 'tnum-cells' checks and nothing else.

.PHONY: test-ui-aui-text-api test-ui-aui-text-api-negctl

UATA_SRC   := tests/unit/ui_aui_text_api_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/v2_text.c \
              $(AUI_TL_SRC) $(GFX_SRC)
UATA_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
              -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UATA_DEPS  := $(UATA_SRC) c/apps/gui/aui.c c/apps/gui/aui_text.inc c/apps/gui/aui_text.h \
              c/include/ui/logit_tokens.h tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_text_api_test: $(UATA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UATA_FLAGS) $(UATA_SRC) -lm -o $@

$(BUILD)/ui_aui_text_api_negctl: $(UATA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UATA_FLAGS) -DUI_TEXT_API_NEGCTL $(UATA_SRC) -lm -o $@

test-ui-aui-text-api-negctl: $(BUILD)/ui_aui_text_api_negctl
	@rc=0; $(BUILD)/ui_aui_text_api_negctl > $(BUILD)/ui_aui_text_api_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_text_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL tnum-cells' $(BUILD)/ui_aui_text_api_negctl.log || { cat $(BUILD)/ui_aui_text_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL tnum-cells' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_text_api_negctl.log | grep -qv '^FAIL tnum-cells'; then \
		cat $(BUILD)/ui_aui_text_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: natural digit advances fail $$(grep -c '^FAIL tnum-cells' $(BUILD)/ui_aui_text_api_negctl.log) cell checks: $$(grep -m1 '^FAIL tnum-cells' $(BUILD)/ui_aui_text_api_negctl.log | cut -c1-110)"

test-ui-aui-text-api: test-ui-aui-text-api-negctl $(BUILD)/ui_aui_text_api_test
	@$(BUILD)/ui_aui_text_api_test

ci-host: test-ui-aui-text-api
