# The menu bar gate -- click "LogitOS", hover to "File", Esc.
#
# NOT YET WIRED INTO THE MAKEFILE, same reasoning tests/motion.mk gives (and
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

.PHONY: test-menu
test-menu: $(ISO) $(DISK)
	@mkdir -p $(MENU_OUT)
	python3 tests/qmp/qmp_menu.py --iso $(ISO) --disk $(DISK) --out $(MENU_OUT)
