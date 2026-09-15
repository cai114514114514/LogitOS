STACKING_DIR = $(BUILD)/layout-stacking
STACKING_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/layout_stacking_test.c
STACKING_DEPS = $(STACKING_SRC) tests/unit/modal_paint_test.c $(MODAL_DEPS) c/apps/browser/layout_stacking.inc tests/layout_stacking.mk $(BUILD)/libcss_host.a
$(STACKING_DIR)/current: $(STACKING_DEPS)
	@mkdir -p $(STACKING_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(STACKING_SRC) $(BUILD)/libcss_host.a -lm
$(STACKING_DIR)/old: $(STACKING_DEPS)
	@mkdir -p $(STACKING_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DLAYOUT_STACKING_FLAT -o $@ $(STACKING_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-layout-stacking test-layout-stacking-negctl test-layout-stacking-san
test-layout-stacking-negctl: $(STACKING_DIR)/old
	@rc=0; $(STACKING_DIR)/old > $(STACKING_DIR)/old.log 2>&1 || rc=$$?; cat $(STACKING_DIR)/old.log; test $$rc -eq 1 && grep -q '^layout-stacking: 46 checks, 24 failures$$' $(STACKING_DIR)/old.log && grep -q '^FAIL: parent8-child1: context background' $(STACKING_DIR)/old.log && grep -q '^FAIL: deep-tree:' $(STACKING_DIR)/old.log
test-layout-stacking: test-layout-stacking-negctl $(STACKING_DIR)/current
	@$(STACKING_DIR)/current
$(STACKING_DIR)/san: $(STACKING_DEPS)
	@mkdir -p $(STACKING_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(STACKING_SRC) $(BUILD)/libcss_host.a -lm
test-layout-stacking-san: test-layout-stacking $(STACKING_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(STACKING_DIR)/san
ci-host: test-layout-stacking

# Shipping-device control changes only the completed display-list ordering.
# All parser, CSS, pixel painter and native input paths are otherwise shared.
$(STACKING_DIR)/guest-layout-old.o: c/apps/browser/layout.c c/apps/browser/layout_stacking.inc $(BROWSER_INTERFACE_DEPS)
	@mkdir -p $(STACKING_DIR)
	$(CC) $(UCFLAGS) $(CSS_INC) -DLAYOUT_STACKING_FLAT -c $< -o $@
$(STACKING_DIR)/guest-old.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o $(STACKING_DIR)/guest-layout-old.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(filter-out $(BUILD)/browserobj/c/apps/browser/layout.o,$(BROWSER_OBJ)) $(STACKING_DIR)/guest-layout-old.o $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(STACKING_DIR)/guest-old.aex: $(STACKING_DIR)/guest-old.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Browser - 'B' 120 130 240 --stack-pages 2048
# Explicit private images only. This gate never builds or launches build/disk.img.
STACKING_GUEST_OUT ?= $(STACKING_DIR)
.PHONY: test-layout-stacking-guest test-layout-stacking-guest-negctl
test-layout-stacking-guest-negctl: $(STACKING_DIR)/guest-old.aex
	@test -n "$(STACKING_GUEST_OLD_DISK)" -a -n "$(STACKING_GUEST_ISO)"
	python3 tests/qmp/layout_stacking_guest.py --iso $(STACKING_GUEST_ISO) --disk $(STACKING_GUEST_OLD_DISK) --out $(STACKING_GUEST_OUT)/guest-old --expect-flat
test-layout-stacking-guest: test-layout-stacking-guest-negctl
	@test -n "$(STACKING_GUEST_DISK)"
	python3 tests/qmp/layout_stacking_guest.py --iso $(STACKING_GUEST_ISO) --disk $(STACKING_GUEST_DISK) --out $(STACKING_GUEST_OUT)/guest-current
