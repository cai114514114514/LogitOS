# tests/ui-aui-buttons.mk -- design 4.3: the button variants of c/apps/gui/aui_button.*
# (Primary, Secondary, Plain, Destructive, Icon, Toggle icon, AI, Popup). The harness
# drives the real ring-3 aui.c over the aui host with every v2 handler (the real
# SYS_GUI_PATH / SHADOW2 / GLASS2 / TEXT_RUN2 code) and records, per variant x state x
# mode x Reduce Transparency, the token colours, the capsule radius h/2, the 13/500
# label, the 0.97 press scale (248), the 102 disabled group and the AI stroke.
#
# THE CONTROL builds the SAME sources and flags plus -DUI_BUTTONS_NEGCTL (the radius is
# capped at min(h/2, 11), the pre-「澄」 rule): it must report 'FAIL capsule-radius' and
# nothing else.

.PHONY: test-ui-aui-buttons test-ui-aui-buttons-negctl

UAB_SRC   := tests/unit/ui_aui_buttons_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/aui_host_scenes.c \
             c/apps/gui/aui.c $(sort $(wildcard tests/unit/aui_host/v2_*.c)) $(AUI_TL_SRC) $(GFX_SRC)
UAB_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui -Ic/include $(KGUI_INC)
UAB_DEPS  := $(UAB_SRC) c/apps/gui/aui_button.inc c/apps/gui/aui_button.h c/apps/gui/aui_motion.h \
             c/include/ui/logit_tokens.h c/include/ui/logit_elevation.h tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_buttons_test: $(UAB_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAB_FLAGS) $(UAB_SRC) -lm -o $@

$(BUILD)/ui_aui_buttons_negctl: $(UAB_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAB_FLAGS) -DUI_BUTTONS_NEGCTL $(UAB_SRC) -lm -o $@

test-ui-aui-buttons-negctl: $(BUILD)/ui_aui_buttons_negctl
	@rc=0; $(BUILD)/ui_aui_buttons_negctl > $(BUILD)/ui_aui_buttons_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_buttons_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL capsule-radius' $(BUILD)/ui_aui_buttons_negctl.log || { cat $(BUILD)/ui_aui_buttons_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL capsule-radius' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_buttons_negctl.log | grep -v '^FAIL ui-aui-buttons' | grep -qv '^FAIL capsule-radius'; then \
		cat $(BUILD)/ui_aui_buttons_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: a capped radius fails: $$(grep -m1 '^FAIL capsule-radius' $(BUILD)/ui_aui_buttons_negctl.log)"

test-ui-aui-buttons: test-ui-aui-buttons-negctl $(BUILD)/ui_aui_buttons_test
	@$(BUILD)/ui_aui_buttons_test

ci-host: test-ui-aui-buttons
