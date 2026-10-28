# tests/ui-aims-a.mk -- P13 aims for Finder and TextEdit (design_language.md P13, 7.1, 7.10;
# protocol_ui.md section I).
#
# files.c and textedit.c print `[files] aim <name> x y` / `[textedit] aim <name> x y`
# (window-local content points) for their hand-drawn controls; the Finder and
# TextEdit drivers (qmp_files.py, run-agent-session.py, run-project-work.py,
# run-textedit-work.py, run-textedit-document.py) click those aims instead of
# keeping a second copy of the layout. accept-project.py clicks nothing.
#
# test-ui-aims-a        runs each driver's aim resolution against tests/ui-aims-a.serial
#                       (the aim and window lines of a real boot of the tour image):
#                       every aim published, on screen, emitted by the app source, and
#                       no numeric click left but the pinned close light.
# test-ui-aims-a-negctl drops the `[files] aim work-field` lines and must log
#                       `FAIL aim-missing files work-field` and nothing else.
UIAA_PY := python3 -I
UIAA_CHECK := tests/unit/ui_aims_a/check.py
UIAA_LOG = $(BUILD)/ui-aims-a-negctl.log
UIAA_DEPS := $(UIAA_CHECK) tests/ui-aims-a.serial tests/qmp/qmp_aim.py \
	tests/qmp/qmp_files.py tests/boot/run-agent-session.py tests/boot/run-project-work.py \
	tests/boot/run-textedit-work.py tests/boot/run-textedit-document.py \
	c/apps/gui/files/files.c c/apps/gui/files/files_project.inc \
	c/apps/gui/textedit/textedit.c c/apps/gui/textedit/textedit_work.inc

test-ui-aims-a-negctl: $(UIAA_DEPS)
	@mkdir -p $(BUILD)
	@rc=0; $(UIAA_PY) $(UIAA_CHECK) --root . --negctl > $(UIAA_LOG) 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UIAA_LOG); echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL aim-missing files work-field$$' $(UIAA_LOG) || { cat $(UIAA_LOG); echo "NEGATIVE CONTROL FAILED: no 'FAIL aim-missing files work-field' line"; exit 1; }; \
	if grep '^FAIL' $(UIAA_LOG) | grep -qv '^FAIL aim-missing files work-field$$'; then \
		grep '^FAIL' $(UIAA_LOG); echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "ui-aims-a control ok: FAIL aim-missing files work-field"

test-ui-aims-a: test-ui-aims-a-negctl
	@$(UIAA_PY) $(UIAA_CHECK) --root .

ci-host: test-ui-aims-a
.PHONY: test-ui-aims-a test-ui-aims-a-negctl
