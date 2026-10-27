# tests/aui-shapes-native.mk -- aui_round/aui_stroke/aui_circle/aui_fill_a and the
# new aui_round4/aui_stroke_a/aui_hairline_dev/aui_squircle/aui_arc/aui_path
# lower to ONE SYS_GUI_PATH when the kernel reports LOGIT_GUI2_F_PATH
# (c/apps/gui/aui_paint.inc). tests/unit/aui_shapes_test.c drives the real ring-3
# aui.c over the aui-host transport with v2_path.c, which #includes the REAL
# c/kernel/gui/wm_draw2_path.c: syscall count per shape, coverage against the
# legacy tile path, big radii, one-device-pixel hairlines at 100/125/150/200%,
# the INFO=0 fallback and the damage boxes.
#
# THE CONTROL builds the SAME sources and flags plus -DAUI_FORCE_TILE_PATH (aui
# behaves as if the kernel never reported PATH). Every rounded shape then costs
# a band per edge plus four corner blits, and the syscall check must report
# 'FAIL syscalls/rounded' -- and nothing else (the control build runs only that
# check). A crash or a link error is not that failure.

.PHONY: test-aui-shapes-native test-aui-shapes-native-negctl

ASN_SRC   := tests/unit/aui_shapes_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/aui_host_scenes.c \
             c/apps/gui/aui.c tests/unit/aui_host/v2_path.c $(AUI_TL_SRC) $(GFX_SRC)
ASN_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
ASN_DEPS  := $(ASN_SRC) c/apps/gui/aui_paint.inc c/apps/gui/aui_paint.h c/apps/gui/aui_damage.h \
             c/kernel/gui/wm_draw2_path.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_shapes_test: $(ASN_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ASN_FLAGS) $(ASN_SRC) -lm -o $@

$(BUILD)/aui_shapes_negctl: $(ASN_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ASN_FLAGS) -DAUI_FORCE_TILE_PATH $(ASN_SRC) -lm -o $@

test-aui-shapes-native-negctl: $(BUILD)/aui_shapes_negctl
	@rc=0; $(BUILD)/aui_shapes_negctl > $(BUILD)/aui_shapes_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_shapes_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL syscalls/rounded' $(BUILD)/aui_shapes_negctl.log || { cat $(BUILD)/aui_shapes_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL syscalls/rounded' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_shapes_negctl.log | grep -v '^FAIL aui-shapes-native' | grep -qv '^FAIL syscalls/rounded'; then \
		cat $(BUILD)/aui_shapes_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: tile path fails: $$(grep -m1 '^FAIL syscalls/rounded' $(BUILD)/aui_shapes_negctl.log)"

test-aui-shapes-native: test-aui-shapes-native-negctl $(BUILD)/aui_shapes_test
	@$(BUILD)/aui_shapes_test

ci-host: test-aui-shapes-native
