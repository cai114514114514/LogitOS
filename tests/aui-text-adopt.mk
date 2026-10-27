# tests/aui-text-adopt.mk -- aui text on GUI ABI v2: aui_text_wrap / measure_wrap,
# aui_text_ellipsis_ex (TAIL / MIDDLE / HEAD, a real U+2026 at a cluster boundary,
# one carets query), baseline alignment from the metrics query, aui_text_a through
# TEXT_RUN2 (c/apps/gui/aui_text.{inc,h}).
#
# The harness #includes the real aui.c over the host transport with
# tests/unit/aui_host/v2_text.c registered (box-glyph TEXT_RUN2 and a carets answer
# consistent with the box advances). The control builds the SAME sources and flags
# plus -DAUI_TEXT_LEGACY_ELLIPSIS (the old byte cut with "...") and must fail the
# 'utf8' checks (invalid UTF-8, no U+2026, more than two measure syscalls) and the
# middle-extension one, and nothing else.

.PHONY: test-aui-text-adopt test-aui-text-adopt-negctl

AATX_SRC   := tests/unit/aui_text_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/v2_text.c \
              $(AUI_TL_SRC) $(GFX_SRC)
AATX_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
              -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
AATX_DEPS  := $(AATX_SRC) c/apps/gui/aui.c c/apps/gui/aui_text.inc c/apps/gui/aui_text.h \
              tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_text_test: $(AATX_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AATX_FLAGS) $(AATX_SRC) -lm -o $@

$(BUILD)/aui_text_negctl: $(AATX_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AATX_FLAGS) -DAUI_TEXT_LEGACY_ELLIPSIS $(AATX_SRC) -lm -o $@

test-aui-text-adopt-negctl: $(BUILD)/aui_text_negctl
	@rc=0; $(BUILD)/aui_text_negctl > $(BUILD)/aui_text_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_text_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	for k in 'utf8 valid' 'utf8 measure-count' 'utf8 U+2026'; do \
		grep -q "^FAIL $$k" $(BUILD)/aui_text_negctl.log || { cat $(BUILD)/aui_text_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL $$k' line"; exit 1; }; done; \
	if grep '^FAIL' $(BUILD)/aui_text_negctl.log | grep -v '^FAIL utf8' | grep -qv '^FAIL middle extension'; then \
		cat $(BUILD)/aui_text_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the legacy byte cut fails: $$(grep -m1 '^FAIL utf8 valid' $(BUILD)/aui_text_negctl.log)"

test-aui-text-adopt: test-aui-text-adopt-negctl $(BUILD)/aui_text_test
	@$(BUILD)/aui_text_test

ci-host: test-aui-text-adopt
