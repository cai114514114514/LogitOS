# tests/ui-qmp-kit.mk -- the Python side of the UI overhaul's test kit
# (design_language.md P13; protocol_ui.md sections C, E, I):
#   tests/qmp/qmp_aim.py     aim parser (`[app] aim <name> x y`)
#   tests/qmp/qmp_ui.py      APP_TITLES tuples, titles_of / window_of
#   tools/ui_tour.py         the screenshot tour
#   tools/ui_shotdiff.py     per-shot diff + --expect-identical
#   tools/ui_glyph_check.py  glyph coverage + English-in-UI lint
#
# test-ui-qmp-kit    unit tests over fixtures (spliced WM lines, newest-wins,
#                    window_of with/without the app line, shotdiff 0 / 1 px,
#                    glyph (cmd resolves to text.ttf), english lint, tour
#                    naming + manifest + lock).
# negctl             feeds the tools one violation each: a U+1F9E0 literal
#                    (the spec said U+1F600, but DejaVuSans carries it), a
#                    one-pixel pair under --expect-identical, 'Apply' in
#                    aui_button; must log FAIL glyph U+1F9E0, FAIL shot-diff
#                    and FAIL english Apply and nothing else.
#
# tour-disk builds build/tour.img = release content + Gallery + Widgets, with
# no /etc/qmp-probes. Gallery and Widgets are NOT on the product or release
# disk (owner's decision, Makefile near APP_RULE widgets) and are never added
# to $(APPS); the sub-make sets UI_TOUR=1 and GALLERY_AEX.

UIQK_PY := python3 -I
UIQK_TEST := tests/unit/ui_qmp_kit/test_kit.py
UIQK_LOG = $(BUILD)/ui-qmp-kit-negctl.log
UIQK_SRC := tests/qmp/qmp_aim.py tests/qmp/qmp_ui.py tests/qmp/qmp_window.py \
            tools/ui_tour.py tools/ui_shotdiff.py tools/ui_glyph_check.py

test-ui-qmp-kit-negctl: $(UIQK_TEST) $(UIQK_SRC)
	@mkdir -p $(BUILD)
	@rc=0; $(UIQK_PY) $(UIQK_TEST) --root . --negctl > $(UIQK_LOG) 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UIQK_LOG); echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	for want in 'FAIL glyph U+1F9E0 ' 'FAIL shot-diff ' 'FAIL english Apply '; do \
		grep -q "^$$want" $(UIQK_LOG) || { cat $(UIQK_LOG); echo "NEGATIVE CONTROL FAILED: no '$$want' line"; exit 1; }; \
	done; \
	if grep '^FAIL' $(UIQK_LOG) | grep -qv -e '^FAIL glyph U+1F9E0 ' -e '^FAIL shot-diff ' -e '^FAIL english Apply '; then \
		grep '^FAIL' $(UIQK_LOG); echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "ui-qmp-kit control ok: $$(grep '^FAIL' $(UIQK_LOG) | cut -d' ' -f1-3 | tr '\n' ';')"

test-ui-qmp-kit: test-ui-qmp-kit-negctl
	@$(UIQK_PY) $(UIQK_TEST) --root .

ci-host: test-ui-qmp-kit
.PHONY: test-ui-qmp-kit test-ui-qmp-kit-negctl

# --- tour image ------------------------------------------------------------
UI_DISK_PACK += $(if $(UI_TOUR),$(BUILD)/widgets.aex:widgets.aex)
$(DISK): $(if $(UI_TOUR),$(BUILD)/widgets.aex)

tour-disk:
	$(MAKE) DISK=$(BUILD)/disk_tour.img RELEASE_DISK=$(BUILD)/tour.img GALLERY_AEX=$(BUILD)/gallery.aex UI_TOUR=1 $(BUILD)/tour.img
.PHONY: tour-disk
