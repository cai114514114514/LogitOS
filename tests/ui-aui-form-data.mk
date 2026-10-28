# tests/ui-aui-form-data.mk -- the aui form and data components of the 「澄」
# overhaul (ui-aui-form-data): form groups / rows / label column / help
# (c/apps/gui/aui_form.{h,inc}, design 4.16), the empty state (4.13), toggle
# and kbd chips (4.15), squircle tiles (3.7.2, 3.7.3), the ring gauge, the
# sparkline and the multi-segment bar (4.14) (c/apps/gui/aui_data.{h,inc}).
#
# The test #includes the real aui.c over the host harness (tests/unit/aui_host,
# every registered v2_*.c handler, so PATH / SHADOW2 are the kernel's own code)
# and checks geometry through the API and colours through the rendered pixels,
# at 100 / 150 / 200 %, light and dark. UI_FORM_DUMP=<dir> additionally writes the
# review scenes (form group, kv rows, empty state, chips, ring, sparkline,
# multibar) as PPMs: UI_FORM_DUMP=<dir> $(BUILD)/ui_aui_form_data/test.
# The control builds the SAME sources and flags plus -DUI_FORM_NEGCTL (plain
# rows 40 pt high) and must fail 'form-row-h' and nothing else.

.PHONY: test-ui-aui-form-data test-ui-aui-form-data-negctl

UAFD_SRC   := tests/unit/ui_aui_form_data_test.c tests/unit/aui_host/aui_host.c \
              $(sort $(wildcard tests/unit/aui_host/v2_*.c)) $(AUI_TL_SRC) $(GFX_SRC)
UAFD_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
              -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAFD_DEPS  := $(UAFD_SRC) c/apps/gui/aui.c $(AUI_INC_SRC) c/include/ui/logit_tokens.h \
              c/include/ui/logit_elevation.h tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_form_data/test: $(UAFD_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAFD_FLAGS) $(UAFD_SRC) -lm -o $@

$(BUILD)/ui_aui_form_data/negctl: $(UAFD_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAFD_FLAGS) -DUI_FORM_NEGCTL $(UAFD_SRC) -lm -o $@

test-ui-aui-form-data-negctl: $(BUILD)/ui_aui_form_data/negctl
	@rc=0; $(BUILD)/ui_aui_form_data/negctl > $(BUILD)/ui_aui_form_data/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_form_data/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL form-row-h' $(BUILD)/ui_aui_form_data/negctl.log || { cat $(BUILD)/ui_aui_form_data/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL form-row-h' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_form_data/negctl.log | grep -qv '^FAIL form-row-h'; then \
		cat $(BUILD)/ui_aui_form_data/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: 40 pt rows fail: $$(grep -m1 '^FAIL form-row-h' $(BUILD)/ui_aui_form_data/negctl.log)"

test-ui-aui-form-data: test-ui-aui-form-data-negctl $(BUILD)/ui_aui_form_data/test
	@$(BUILD)/ui_aui_form_data/test

ci-host: test-ui-aui-form-data
