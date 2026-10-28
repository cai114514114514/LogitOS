# tests/ui-aui-fields.mk -- text field, large field, search field, caret, overlay
# scrollbar and gutterless scroll containers (design 4.4 and 4.12;
# c/apps/gui/aui_fields.{h,inc}).
#
# The test #includes the real aui.c over the host harness (every registered
# tests/unit/aui_host/v2_*.c handler, so v2 mode has PATH and the rims are real
# 1-device-pixel paths) and checks, in INFO=0 and v2 at 100/125/150/200 %: the
# caret's blink phases, damage (its own rect) and silence after 8 s idle; the
# field box (28 / 36 pt, r 8 / 10, fill, rim, focus rim and ring, invalid,
# disabled); the search capsule (clear button, Esc clears then blurs); the
# overlay scrollbar (hidden at rest, shown after a wheel notch, hover widening,
# fade completing, light and dark colours, Reduce Motion); and that a scroll
# container's content is as wide as the container ('gutterless').
#
# The control builds the SAME sources and flags plus -DUI_FIELDS_NEGCTL (the
# old 12 pt gutter in the clip) and must print 'FAIL gutterless' and nothing else.

.PHONY: test-ui-aui-fields test-ui-aui-fields-negctl

UAF_SRC   := tests/unit/ui_aui_fields_test.c tests/unit/aui_host/aui_host.c \
             $(sort $(wildcard tests/unit/aui_host/v2_*.c)) $(AUI_TL_SRC) $(GFX_SRC)
UAF_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
             -Itests/unit/aui_host -Ic/apps/gui -Ic/apps -Ic/include \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAF_DEPS  := $(UAF_SRC) c/apps/gui/aui.c c/apps/gui/aui_fields.inc c/apps/gui/aui_fields.h \
             c/include/ui/logit_tokens.h tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_fields_test: $(UAF_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAF_FLAGS) $(UAF_SRC) -lm -o $@

$(BUILD)/ui_aui_fields_negctl: $(UAF_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAF_FLAGS) -DUI_FIELDS_NEGCTL $(UAF_SRC) -lm -o $@

test-ui-aui-fields-negctl: $(BUILD)/ui_aui_fields_negctl
	@rc=0; $(BUILD)/ui_aui_fields_negctl > $(BUILD)/ui_aui_fields_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_fields_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL gutterless' $(BUILD)/ui_aui_fields_negctl.log || { cat $(BUILD)/ui_aui_fields_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL gutterless' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_fields_negctl.log | grep -qv '^FAIL gutterless'; then \
		cat $(BUILD)/ui_aui_fields_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the kept gutter fails $$(grep -c '^FAIL gutterless' $(BUILD)/ui_aui_fields_negctl.log) gutterless checks: $$(grep -m1 '^FAIL gutterless' $(BUILD)/ui_aui_fields_negctl.log | cut -c1-110)"

test-ui-aui-fields: test-ui-aui-fields-negctl $(BUILD)/ui_aui_fields_test
	@$(BUILD)/ui_aui_fields_test

ci-host: test-ui-aui-fields
