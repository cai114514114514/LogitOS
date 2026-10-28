# tests/ui-aims-b.mk -- P13 aims for Monitor, Clock, Studio and Preview, and P14 (gate the Preview
# viewer probe) (design_language.md P13, P14; protocol_ui.md section I).
#
# monitor.c, clock/view.c, studio_view.inc / studio_project.inc and preview view.inc / picker.inc
# print `[<app>] aim <name> x y` (window-local content points) for their controls; aui prints the
# widget-drawn ones (tabs, sort segments, table rows, dialog buttons) once monitor.c arms it with
# aui_aim_names / aui_aim_prefix. The drivers (qmp_monitor.py, run-clock-ui.py, run-studio-ui.py,
# run-studio-completion.py, qmp_preview.py, qmp_preview_nav.py) click those aims instead of keeping a
# second copy of the layout.
#
# test-ui-aims-b        runs each driver's aim resolution against tests/ui-aims-b.serial (the aim and
#                       window lines of real boots of the test image): every aim published, on screen,
#                       emitted by the app source, no numeric click left but the design-pinned ones, and
#                       the Preview viewer probe behind qmp_probes_on() (P14).
# test-ui-aims-b-negctl drops the `[monitor] aim tab-memory` lines and must log
#                       `FAIL aim-missing monitor tab-memory` and nothing else.
UIAB_PY := python3 -I
UIAB_CHECK := tests/unit/ui_aims_b/check.py
UIAB_LOG = $(BUILD)/ui-aims-b-negctl.log
UIAB_DEPS := $(UIAB_CHECK) tests/ui-aims-b.serial tests/qmp/qmp_aim.py \
	tests/qmp/qmp_monitor.py tests/boot/run-clock-ui.py tests/boot/run-studio-ui.py \
	tests/boot/run-studio-completion.py tests/qmp/qmp_preview.py tests/qmp/qmp_preview_nav.py \
	c/apps/gui/monitor/monitor.c c/apps/gui/clock/view.c c/apps/gui/clock/main.c \
	c/apps/gui/studio/studio.c c/apps/gui/studio/core/studio_view.inc \
	c/apps/gui/studio/core/studio_project.inc c/apps/gui/preview/preview.c \
	c/apps/gui/preview/view.inc c/apps/gui/preview/picker.inc

test-ui-aims-b-negctl: $(UIAB_DEPS)
	@mkdir -p $(BUILD)
	@rc=0; $(UIAB_PY) $(UIAB_CHECK) --root . --negctl > $(UIAB_LOG) 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UIAB_LOG); echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL aim-missing monitor tab-memory$$' $(UIAB_LOG) || { cat $(UIAB_LOG); echo "NEGATIVE CONTROL FAILED: no 'FAIL aim-missing monitor tab-memory' line"; exit 1; }; \
	if grep '^FAIL' $(UIAB_LOG) | grep -qv '^FAIL aim-missing monitor tab-memory$$'; then \
		grep '^FAIL' $(UIAB_LOG); echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "ui-aims-b control ok: FAIL aim-missing monitor tab-memory"

test-ui-aims-b: test-ui-aims-b-negctl
	@$(UIAB_PY) $(UIAB_CHECK) --root .

ci-host: test-ui-aims-b
.PHONY: test-ui-aims-b test-ui-aims-b-negctl
