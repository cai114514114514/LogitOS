# tests/ui-art-assets.mk -- ui-art-assets: the generated app icons and the night wallpaper
# (design_language.md 3.7.4 app icons, 3.7.5 the Logit mark, 5.5 desktop and wallpaper).
#
# What lives where:
#   tests/settings2.mk   builds tools/mkicons ($(MKICONS)), generates $(BUILD)/icons/<stem>.png
#                        (APP_ICON_FILES) and packs them as /icons/<stem>.png on every disk;
#                        fsroot/icons/{settings,preview}.png are gone (mkfs aborts on a duplicate).
#   this fragment        the night wallpaper (packed on the RELEASE image only, as
#                        /wallpaper-dark.png and /wallpaper-dark@2x.png), the contact sheet, and
#                        the gate.
#
# test-ui-art-assets runs tests/unit/ui_art_check.py on a fresh render: determinism, sizes, the
# squircle corner, the gradient ends, glyph coverage, the rim, the contact sheet, wallpaper sizes,
# sky luma, star placement and the unchanged light wallpaper. Its control is the SAME command run
# against mkicons built with -DUI_ART_NEGCTL (no rim): the checker must print `FAIL rim`, no other
# FAIL, and exit non-zero, or the control itself fails. The checker is invoked identically (same
# flags, same sources); only the binary differs.
.PHONY: test-ui-art-assets test-ui-art-assets-negctl ui-art-sheet

UIA_NEG := $(BUILD)/tools/mkicons-negctl
UIA_WALL := $(BUILD)/wallpaper-dark.png $(BUILD)/wallpaper-dark@2x.png

$(BUILD)/wallpaper-dark.png: tools/mkwallpaper.py
	@mkdir -p $(BUILD)
	python3 tools/mkwallpaper.py --night $@ 1280 800
$(BUILD)/wallpaper-dark@2x.png: tools/mkwallpaper.py
	@mkdir -p $(BUILD)
	python3 tools/mkwallpaper.py --night $@ 2560 1600

UI_RELEASE_PACK += $(BUILD)/wallpaper-dark.png:/wallpaper-dark.png $(BUILD)/wallpaper-dark@2x.png:/wallpaper-dark@2x.png
$(RELEASE_DISK): $(UIA_WALL)

# The contact sheet (13 icons at 256/64/32 on #F4F4F8 and #1E1E24) for the reviewer.
$(BUILD)/icons/sheet.png: $(MKICONS)
	@mkdir -p $(dir $@)
	$(MKICONS) --sheet $@ --sheet-only $(BUILD)/icons
ui-art-sheet: $(BUILD)/icons/sheet.png

$(UIA_NEG): $(MKICONS_SRC) $(GFX_SRC) $(GFX_HEADERS) tests/unit/rust_host_shim.c $(RUST_LIB_HOST)
	@mkdir -p $(dir $@)
	$(CC) -O2 -ffp-contract=off -Wall -Wextra -Wno-missing-field-initializers -DUI_ART_NEGCTL -Ic/include $(GFX_INC) -Itools/mkicons \
	    tools/mkicons/mkicons.c $(GFX_SRC) tests/unit/rust_host_shim.c $(RUST_LIB_HOST) -lm -o $@

test-ui-art-assets: test-ui-art-assets-negctl $(MKICONS) tools/mkwallpaper.py
	@mkdir -p $(BUILD)/icons $(BUILD)/icons-check
	@python3 -I tests/unit/ui_art_check.py --mkicons $(MKICONS) --wallpaper tools/mkwallpaper.py \
	    --out $(BUILD)/icons-check --sheet $(BUILD)/icons/sheet.png

test-ui-art-assets-negctl: $(UIA_NEG) tools/mkwallpaper.py
	@mkdir -p $(BUILD)/icons-negctl
	@python3 -I tests/unit/ui_art_check.py --mkicons $(UIA_NEG) --wallpaper tools/mkwallpaper.py \
	    --out $(BUILD)/icons-negctl --sheet $(BUILD)/icons-negctl/sheet.png --expect-fail rim

ci-host: test-ui-art-assets
