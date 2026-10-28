# tests/ui-aui-nav.mk -- the aui navigation components of the 「澄」 overhaul
# (ui-aui-nav; design 4.1 floating sidebar, 4.2 toolbar, 4.17 split view and
# disclosure, 4.18 inspector, signature 6.4 the gliding selection pill).
#
# tests/unit/ui_aui_nav_test.c builds the REAL ring-3 aui.c over the aui_host
# transport in v2 mode (the real wm_draw2 / wm_backdrop handlers, picked up by
# the same wildcard as the AUI host harness) behind a recorder,
# -DLOGIT_SYS_HOOK=nav_sys, and runs every case in a forked child at 100, 125,
# 150 and 200 %. Its header lists the checks.
#
#   test-ui-aui-nav          the checks; THE CONTROL runs first
#   test-ui-aui-nav-negctl   the SAME sources and flags plus -DUI_NAV_NEGCTL
#                            (a second pill painted on the target row): must
#                            exit 1 with 'FAIL single-pill' and no other check
#   ui-aui-nav-dump          review scenes (the 7.1 Finder layout at 100/200 %,
#                            light/dark, hover, scrolled, unfocused, mid-glide)
#                            as PPM + JPEG with the real ui*.ttf text and the
#                            real icons.c symbols, in $(BUILD)/ui_aui_nav/dump
#
# The real symbols come from c/kernel/gui/icons.c, built as its own objects
# (kernel include paths) and linked into both binaries; only --dump uses them.

.PHONY: test-ui-aui-nav test-ui-aui-nav-negctl ui-aui-nav-dump

UANAV_DIR   := $(BUILD)/ui_aui_nav
UANAV_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=nav_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
               $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UANAV_SRC   := tests/unit/ui_aui_nav_test.c tests/unit/aui_host/aui_host.c \
               $(sort $(wildcard tests/unit/aui_host/v2_*.c)) c/apps/gui/aui.c $(AUI_TL_SRC) $(GFX_SRC)
UANAV_OBJ   := $(UANAV_DIR)/nav_icons.o $(UANAV_DIR)/icons.o
UANAV_IFLAGS := -O1 -g -Wall -Wextra -Itests/unit/aui_host -Ic/kernel/gui -Ic/kernel/gui/fb -Ic/kernel/cpu \
                -Ic/apps -Iinclude -Iinclude/abi $(GFX_INC)
UANAV_DEPS  := tests/unit/aui_host/aui_host.h c/apps/gui/aui.h c/apps/logit.h $(AUI_INC_SRC) \
               $(wildcard c/include/ui/*.h) $(GFX_HEADERS)

$(UANAV_DIR)/nav_icons.o: tests/unit/ui_aui_nav_icons.c c/kernel/gui/icons.h tests/unit/aui_host/aui_host.h
	@mkdir -p $(dir $@)
	@$(CC) $(UANAV_IFLAGS) -c $< -o $@

$(UANAV_DIR)/icons.o: c/kernel/gui/icons.c c/kernel/gui/icons.h $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	@$(CC) $(UANAV_IFLAGS) -c $< -o $@

$(UANAV_DIR)/test: $(UANAV_SRC) $(UANAV_OBJ) $(UANAV_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UANAV_FLAGS) $(UANAV_SRC) $(UANAV_OBJ) -lm -o $@

$(UANAV_DIR)/negctl: $(UANAV_SRC) $(UANAV_OBJ) $(UANAV_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UANAV_FLAGS) -DUI_NAV_NEGCTL $(UANAV_SRC) $(UANAV_OBJ) -lm -o $@

test-ui-aui-nav-negctl: $(UANAV_DIR)/negctl
	@rc=0; $(UANAV_DIR)/negctl > $(UANAV_DIR)/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UANAV_DIR)/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL single-pill' $(UANAV_DIR)/negctl.log || { cat $(UANAV_DIR)/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL single-pill' line"; exit 1; }; \
	if grep '^FAIL' $(UANAV_DIR)/negctl.log | grep -v '^FAIL ui-aui-nav:' | grep -qv '^FAIL single-pill'; then \
		cat $(UANAV_DIR)/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than single-pill failed"; exit 1; fi; \
	echo "negative control ok: two pills fail: $$(grep -m1 '^FAIL single-pill' $(UANAV_DIR)/negctl.log)"

test-ui-aui-nav: test-ui-aui-nav-negctl $(UANAV_DIR)/test
	@$(UANAV_DIR)/test

ui-aui-nav-dump: $(UANAV_DIR)/test
	@rm -rf $(UANAV_DIR)/dump && mkdir -p $(UANAV_DIR)/dump
	@$(UANAV_DIR)/test --dump $(UANAV_DIR)/dump
	@python3 tests/unit/ui_aui_nav_dump.py $(UANAV_DIR)/dump fsroot/fonts

ci-host: test-ui-aui-nav
