# tests/ui-aims-c.mk -- P13 aims for the Browser chrome, Terminal, Assistant and Chat
# (design_language.md P13, 7.x; protocol_ui.md section I).
#
# browser (main.c draw_tab_strip, address.inc), terminal.c, assistant.c and ch.c print
# `[<app>] aim <name> x y` (window-local content points) for the controls a test clicks:
#   browser   address, caret (while editing), tab-<k>, new-tab
#   terminal  input
#   assistant field, send, suggestion-<i>, store, task-<i>
#   ch        field, send
# qmp_addrbar.py, qmp_tabs.py and qmp_ch.py click those aims instead of a second copy of
# the layout; qmp_term.py, qmp_cjk_term.py and qmp_audio_term.py require the Terminal's
# input aim. The Browser has no back / forward / reload / star buttons (keyboard
# shortcuts only; the bookmark star is an indicator), so those names are not published.
#
# test-ui-aims-c        runs each driver's aim resolution against tests/ui-aims-c.serial
#                       (the aim and window lines of real boots of the test image): every
#                       aim published, on screen, emitted by the app source, and no numeric
#                       click left in qmp_tabs.py / qmp_ch.py.
# test-ui-aims-c-negctl drops the `[browser] aim caret` lines and must log
#                       `FAIL aim-missing browser caret` and nothing else.
UIAC_PY := python3 -I
UIAC_CHECK := tests/unit/ui_aims_c/check.py
UIAC_LOG = $(BUILD)/ui-aims-c-negctl.log
UIAC_DEPS := $(UIAC_CHECK) tests/ui-aims-c.serial tests/qmp/qmp_aim.py \
	tests/qmp/qmp_addrbar.py tests/qmp/qmp_tabs.py tests/qmp/qmp_term.py \
	tests/qmp/qmp_cjk_term.py tests/qmp/qmp_audio_term.py tests/qmp/qmp_ch.py \
	c/apps/gui/browser/app/main.c c/apps/gui/browser/app/address.inc \
	c/apps/gui/terminal/terminal.c c/apps/gui/assistant/assistant.c c/apps/gui/ch/ch.c

test-ui-aims-c-negctl: $(UIAC_DEPS)
	@mkdir -p $(BUILD)
	@rc=0; $(UIAC_PY) $(UIAC_CHECK) --root . --negctl > $(UIAC_LOG) 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UIAC_LOG); echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL aim-missing browser caret$$' $(UIAC_LOG) || { cat $(UIAC_LOG); echo "NEGATIVE CONTROL FAILED: no 'FAIL aim-missing browser caret' line"; exit 1; }; \
	if grep '^FAIL' $(UIAC_LOG) | grep -qv '^FAIL aim-missing browser caret$$'; then \
		grep '^FAIL' $(UIAC_LOG); echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "ui-aims-c control ok: FAIL aim-missing browser caret"

test-ui-aims-c: test-ui-aims-c-negctl
	@$(UIAC_PY) $(UIAC_CHECK) --root .

ci-host: test-ui-aims-c
.PHONY: test-ui-aims-c test-ui-aims-c-negctl
