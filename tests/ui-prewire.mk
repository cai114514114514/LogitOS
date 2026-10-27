# tests/ui-prewire.mk -- static wiring check for the UI overhaul backlog
# (design_language.md): every ui-* fragment has a literal -include line, the two
# recipe-side asset hooks and the AUI_INC_SRC wildcard exist, and no
# prerequisite-side UI_ hook exists. tools/ui_prewire_check.py parses the
# Makefile. The control adds an id that has no -include line and must log
# "FAIL missing-include ui-does-not-exist".
UIPW_IDS := ui-prewire ui-tokens-headers ui-qmp-kit ui-run2-units ui-wm-split \
	ui-aui-core ui-aui-text-api ui-aui-motion-api ui-art-assets \
	ui-bench-frame-fix ui-aui-theme ui-aui-buttons ui-aui-controls \
	ui-aui-fields ui-aui-lists ui-aui-overlays ui-aui-nav ui-aui-form-data \
	ui-kernel-text ui-wm-abi ui-glass-adapt ui-aims-a ui-aims-b ui-aims-c \
	ui-wm-glass ui-wm-menubar ui-wm-dock ui-wm-chrome ui-wm-motion \
	ui-wm-overlays ui-wm-wallpaper-reveal ui-notify ui-spotlight \
	ui-aui-icons ui-ime-chrome ui-wm-glass-c2 ui-gallery ui-app-finder \
	ui-app-settings-a ui-app-settings-b ui-app-clock ui-app-monitor \
	ui-desktop-look ui-budget-gate ui-app-preview ui-app-browser \
	ui-app-terminal ui-app-textedit ui-app-studio ui-app-assistant \
	ui-app-greeter ui-app-chat ui-app-widgets ui-modeb-core \
	ui-polish-defaults ui-modeb-apps-a ui-modeb-apps-b ui-polish-review

UIPW_CHECK := python3 -I tools/ui_prewire_check.py

test-ui-prewire-negctl: tools/ui_prewire_check.py Makefile
	@mkdir -p $(BUILD)
	@rc=0; $(UIPW_CHECK) --ids $(UIPW_IDS) ui-does-not-exist > $(BUILD)/ui-prewire-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/ui-prewire-negctl.log; exit 1; }
	@grep -q '^FAIL missing-include ui-does-not-exist$$' $(BUILD)/ui-prewire-negctl.log || { echo "negctl did not fail with 'FAIL missing-include ui-does-not-exist'"; cat $(BUILD)/ui-prewire-negctl.log; exit 1; }
	@test "$$(grep -c '^FAIL ' $(BUILD)/ui-prewire-negctl.log)" = 1 || { echo "negctl failed for another reason too"; cat $(BUILD)/ui-prewire-negctl.log; exit 1; }
	@echo 'ui-prewire control: unwired id rejected, failed FAIL missing-include.'

test-ui-prewire: test-ui-prewire-negctl
	$(UIPW_CHECK) --ids $(UIPW_IDS)

ci-host: test-ui-prewire
.PHONY: test-ui-prewire test-ui-prewire-negctl
