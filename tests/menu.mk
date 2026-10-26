# The menu bar gate -- click "LogitOS", hover to "File", Esc.
#
# (Since wired: the Makefile has `-include tests/menu.mk`.) It was NOT YET WIRED, same reasoning tests/motion.mk gives (and
# the same file this unit's brief named as a concurrent line): several other
# lines are editing the Makefile this week, and one `-include` line is a
# cheaper merge for whoever owns it than a conflict is for everybody. To
# register it, add next to the other `-include tests/*.mk` lines near the
# bottom of the Makefile:
#
#     -include tests/menu.mk
#
# Until then the gate runs directly, and its own header documents that:
#
#     python3 tests/qmp/qmp_menu.py
#
# WHAT IT MEASURES, in one line each -- the long version is the docstring at
# the top of tests/qmp/qmp_menu.py:
#   * clicking "LogitOS" actually puts a dropdown on screen where the guest's
#     own serial report says it did (not merely that the guest CLAIMS to have
#     opened one);
#   * hovering (no click) to "File" switches menus live, and the picture
#     agrees;
#   * Escape closes it, and the covered region is repainted BYTE FOR BYTE
#     against the true pre-menu baseline -- not "looks close", which is what
#     a partial-redraw ghost would pass.
#
# All three geometries are read off the guest's own "[wm] menu ..." serial
# lines rather than re-derived from AA font metrics in Python -- see
# menu_bar_layout()/menu_report_open() in c/kernel/gui/wm.c.

MENU_OUT ?= build/menu

# THE SHADOW CONTROL (2026-10-26). A closed dropdown used to repaint its
# panel and not its drop shadow, leaving a dark streak under each menu title
# that had been open. -DWM_MENU_SHADOW_NEGCTL builds that old damage back in;
# the gate's two "shadow below the panel is repainted" checks must fail on it.
ifeq ($(MENUSHADOWNEGCTL),1)
CFLAGS += -DWM_MENU_SHADOW_NEGCTL
endif
MENU_CTL_BUILD := $(BUILD)-menushadow

.PHONY: test-menu test-menu-negctl
test-menu-negctl: $(DISK)
	@$(MAKE) --no-print-directory BUILD=$(MENU_CTL_BUILD) MENUSHADOWNEGCTL=1 \
		$(MENU_CTL_BUILD)/logit.iso > $(BUILD)/menu_ctl_build.log 2>&1
	@rc=0; python3 tests/qmp/qmp_menu.py --iso $(MENU_CTL_BUILD)/logit.iso --disk $(DISK) \
		--out $(MENU_OUT)-ctl > $(BUILD)/menu_ctl.log 2>&1 || rc=$$?; cat $(BUILD)/menu_ctl.log; \
		if [ $$rc -ne 0 ] && grep -q "FAIL.*shadow below the panel is repainted" $(BUILD)/menu_ctl.log; then \
			echo "negative control ok: panel-only damage leaves the shadow behind"; \
		else echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-menu: test-menu-negctl $(ISO) $(DISK)
	@mkdir -p $(MENU_OUT)
	python3 tests/qmp/qmp_menu.py --iso $(ISO) --disk $(DISK) --out $(MENU_OUT)
