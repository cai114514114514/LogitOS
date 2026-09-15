# SPDX-License-Identifier: MIT
DETACHED_IMAGES_BASE = $(filter-out tests/unit/dynamic_stylesheets_test.c,$(DYNAMIC_SHEETS_SRC)) tests/unit/detached_images_test.c
DETACHED_IMAGES_SRC = $(DETACHED_IMAGES_BASE) $(filter-out $(DETACHED_IMAGES_BASE),$(IMGCHK_SRC))
DETACHED_IMAGES_DEPS = tests/detached_images.mk $(DETACHED_IMAGES_SRC) $(DYNAMIC_SHEETS_DEPS) c/apps/browser/browser_images.inc tests/fixtures/browser/detached-images.html
DETACHED_IMAGES_DIR = $(BUILD)/detached-images
$(DETACHED_IMAGES_DIR)/current: $(DETACHED_IMAGES_DEPS)
	@mkdir -p $(DETACHED_IMAGES_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -o $@ $(DETACHED_IMAGES_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(DETACHED_IMAGES_DIR)/old: $(DETACHED_IMAGES_DEPS)
	@mkdir -p $(DETACHED_IMAGES_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -DBROWSER_NO_DOM_IMAGE_LOADING -o $@ $(DETACHED_IMAGES_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-detached-images test-detached-images-negctl
test-detached-images-negctl: $(DETACHED_IMAGES_DIR)/old
	@rc=0; $< > $(DETACHED_IMAGES_DIR)/old.log 2>&1 || rc=$$?; tail -20 $(DETACHED_IMAGES_DIR)/old.log; \
	 test $$rc -eq 1 && grep -q '^FAIL detached source fires one load' $(DETACHED_IMAGES_DIR)/old.log && \
	 grep -q '^FAIL: image preloads actually reached transport' $(DETACHED_IMAGES_DIR)/old.log
test-detached-images: test-detached-images-negctl $(DETACHED_IMAGES_DIR)/current
	@$(DETACHED_IMAGES_DIR)/current > $(DETACHED_IMAGES_DIR)/current.log 2>&1; rc=$$?; tail -24 $(DETACHED_IMAGES_DIR)/current.log; exit $$rc
ci-host: test-detached-images

.PHONY: test-detached-images-asan
test-detached-images-asan: $(DETACHED_IMAGES_DEPS)
	@mkdir -p $(DETACHED_IMAGES_DIR)
	$(CC) $(filter-out -O2,$(RUNTIME_SCROLL_CF)) -O1 -g -fsanitize=address -fno-omit-frame-pointer $(IMG_HOST_INC) -o $(DETACHED_IMAGES_DIR)/asan $(DETACHED_IMAGES_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	ASAN_OPTIONS=detect_leaks=0 $(DETACHED_IMAGES_DIR)/asan
