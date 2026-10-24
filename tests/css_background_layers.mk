# Same DOM/CSS/layout/cache/painter road as bitmap backgrounds. An accepted
# first-layer-only list is the fault, not a different test-side renderer.
CSS_LAYER_DIR := $(BUILD)/css-background-layers
CSS_LAYER_SRC = $(filter-out tests/unit/css_background_image_test.c,$(CSS_BACKGROUND_IMAGE_SRC)) tests/unit/css_background_layers_test.c
CSS_LAYER_DEPS = $(CSS_LAYER_SRC) $(BROWSER_STYLE_MODULES) $(BROWSER_HEADERS) $(BUILD)/libcss_host.a tests/unit/css_background_test.c tests/unit/backgroundhost/logit.h c/apps/gui/browser/app/background.c tests/fixtures/engine-expansion/css-layers.html tests/fixtures/engine-expansion/css-layers.css tests/fixtures/engine-expansion/css-layers-pixels.csv tests/fixtures/engine-expansion/assets/stripes.bmp tests/css_background_layers.mk
define CSS_LAYER_BUILD
	@mkdir -p $(CSS_LAYER_DIR)
	$(CC) -O1 -g -w $(1) -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_LAYER_SRC) $(BUILD)/libcss_host.a -lm
endef
$(CSS_LAYER_DIR)/current: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,)
$(CSS_LAYER_DIR)/san: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-fsanitize=address,undefined -fno-sanitize-recover=all)
$(CSS_LAYER_DIR)/first-only: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-DCSS_BACKGROUND_FIRST_LAYER)
$(CSS_LAYER_DIR)/native-attachment: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-DCSS_BACKGROUND_NATIVE_ATTACHMENT)
$(CSS_LAYER_DIR)/old-color-text: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-DCSS_BACKGROUND_OLD_COLOR_TEXT)
$(CSS_LAYER_DIR)/ready-only: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-DCSS_BACKGROUND_READY_ONLY_ROUTING)
$(CSS_LAYER_DIR)/static-signature: $(CSS_LAYER_DEPS)
	$(call CSS_LAYER_BUILD,-DPAINT_BACKGROUND_STATIC_SIGNATURE)
.PHONY: test-css-background-layers test-css-background-layers-san test-css-background-layers-negctl
test-css-background-layers-negctl: $(CSS_LAYER_DIR)/first-only $(CSS_LAYER_DIR)/native-attachment $(CSS_LAYER_DIR)/old-color-text $(CSS_LAYER_DIR)/ready-only $(CSS_LAYER_DIR)/static-signature
	@rc=0; $(CSS_LAYER_DIR)/first-only > $(CSS_LAYER_DIR)/first-only.log 2>&1 || rc=$$?; cat $(CSS_LAYER_DIR)/first-only.log; test $$rc -eq 1 && grep -q '^FAIL: third image layer keeps asynchronous discovery alive' $(CSS_LAYER_DIR)/first-only.log && grep -q '^FAIL: first image composites above the lower image' $(CSS_LAYER_DIR)/first-only.log
	@rc=0; $(CSS_LAYER_DIR)/native-attachment > $(CSS_LAYER_DIR)/native-attachment.log 2>&1 || rc=$$?; cat $(CSS_LAYER_DIR)/native-attachment.log; test $$rc -eq 1 && grep -q '^FAIL: refused attachment cannot publish native shorthand color' $(CSS_LAYER_DIR)/native-attachment.log
	@rc=0; $(CSS_LAYER_DIR)/old-color-text > $(CSS_LAYER_DIR)/old-color-text.log 2>&1 || rc=$$?; cat $(CSS_LAYER_DIR)/old-color-text.log; test $$rc -eq 1 && grep -qx 'FAIL: computed background color agrees with the rendered complex shorthand' $(CSS_LAYER_DIR)/old-color-text.log && grep -qx 'css-background-layers: 113 checks, 1 failures' $(CSS_LAYER_DIR)/old-color-text.log
	@rc=0; $(CSS_LAYER_DIR)/ready-only > $(CSS_LAYER_DIR)/ready-only.log 2>&1 || rc=$$?; cat $(CSS_LAYER_DIR)/ready-only.log; test $$rc -eq 1 && grep -qx 'FAIL: zero width gradient preserves the visible ancestor' $(CSS_LAYER_DIR)/ready-only.log && grep -qx 'FAIL: zero height gradient preserves the visible ancestor' $(CSS_LAYER_DIR)/ready-only.log && grep -qx 'css-background-layers: 113 checks, 2 failures' $(CSS_LAYER_DIR)/ready-only.log
	@rc=0; $(CSS_LAYER_DIR)/static-signature > $(CSS_LAYER_DIR)/static-signature.log 2>&1 || rc=$$?; cat $(CSS_LAYER_DIR)/static-signature.log; test $$rc -eq 1 && grep -qx 'FAIL: late first background completion dirties its entire affected box' $(CSS_LAYER_DIR)/static-signature.log && grep -qx 'FAIL: late third background completion dirties its entire affected box' $(CSS_LAYER_DIR)/static-signature.log && grep -qx 'FAIL: late shared background completion dirties every affected box' $(CSS_LAYER_DIR)/static-signature.log && grep -qx 'css-background-layers: 113 checks, 3 failures' $(CSS_LAYER_DIR)/static-signature.log
test-css-background-layers: test-css-background-layers-negctl $(CSS_LAYER_DIR)/current
	@$(CSS_LAYER_DIR)/current $(CSS_LAYER_DIR)/current.ppm
test-css-background-layers-san: test-css-background-layers-negctl $(CSS_LAYER_DIR)/san
	@ASAN_OPTIONS=detect_leaks=$(ASAN_LEAKS) UBSAN_OPTIONS=halt_on_error=1 $(CSS_LAYER_DIR)/san $(CSS_LAYER_DIR)/san.ppm
ci-host: test-css-background-layers
