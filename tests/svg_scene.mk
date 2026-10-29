# SPDX-License-Identifier: MIT
# The registry shim substitutes allocation/registration only: geometry and
# rasterization are the same SVG and GFX sources the guest browser links.
SVG_SCENE_SRC := tests/unit/svg_scene_test.c c/lib/image/svg.c $(GFX_SRC)
SVG_SCENE_DEP := $(SVG_SCENE_SRC) c/lib/image/svg_scene.inc c/lib/image/img.h c/lib/gfx/include/gfx.h tests/svg_scene.mk
SVG_SCENE_DIR := $(BUILD)/svg-scene
$(SVG_SCENE_DIR)/current: $(SVG_SCENE_DEP)
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g -Wall -Wextra $(IMG_HOST_INC) $(SVG_SCENE_SRC) -o $@
$(SVG_SCENE_DIR)/old: $(SVG_SCENE_DEP)
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g $(IMG_HOST_INC) -DSVG_SCENE_NO_CURRENTCOLOR -DSVG_SCENE_NO_TRANSFORM -DSVG_SCENE_NO_USE -DSVG_SCENE_NO_CLIP $(SVG_SCENE_SRC) -o $@
$(SVG_SCENE_DIR)/asan: $(SVG_SCENE_DEP)
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(IMG_HOST_INC) $(SVG_SCENE_SRC) -o $@
.PHONY: test-svg-scene test-svg-scene-negctl test-svg-scene-asan
test-svg-scene-negctl: $(SVG_SCENE_DIR)/old
	@rc=0; $(SVG_SCENE_DIR)/old > $(SVG_SCENE_DIR)/old.log 2>&1 || rc=$$?; \
	 cat $(SVG_SCENE_DIR)/old.log; test $$rc -eq 1 && \
	 rg -q '^svg-scene: 91 checks, 29 failures$$' $(SVG_SCENE_DIR)/old.log && \
	 rg -q '^FAIL: currentColor root inherited$$' $(SVG_SCENE_DIR)/old.log && \
	 rg -q '^FAIL: transform translates geometry$$' $(SVG_SCENE_DIR)/old.log && \
	 rg -q '^FAIL: use local geometry and x y$$' $(SVG_SCENE_DIR)/old.log && \
	 rg -q '^FAIL: clip user space clips right pixels$$' $(SVG_SCENE_DIR)/old.log
test-svg-scene: test-svg-scene-negctl $(SVG_SCENE_DIR)/current
	@$(SVG_SCENE_DIR)/current
test-svg-scene-asan: test-svg-scene-negctl $(SVG_SCENE_DIR)/asan
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(SVG_SCENE_DIR)/asan
ci-host: test-svg-scene

SVG_SCENE_BUDGET_SRC := tests/unit/svg_scene_budget_test.c $(filter-out tests/unit/svg_scene_test.c,$(SVG_SCENE_SRC))
$(SVG_SCENE_DIR)/budget: $(SVG_SCENE_DEP) tests/unit/svg_scene_budget_test.c
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g $(IMG_HOST_INC) -DSV_WORK_LIMIT=8192 $(SVG_SCENE_BUDGET_SRC) -o $@
$(SVG_SCENE_DIR)/budget-old: $(SVG_SCENE_DEP) tests/unit/svg_scene_budget_test.c
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g $(IMG_HOST_INC) -DSV_WORK_LIMIT=8192 -DSVG_SCENE_UNMETERED_LAYERS $(SVG_SCENE_BUDGET_SRC) -o $@
.PHONY: test-svg-scene-budget test-svg-scene-budget-negctl
test-svg-scene-budget-negctl: $(SVG_SCENE_DIR)/budget-old
	@rc=0; $(SVG_SCENE_DIR)/budget-old > $(SVG_SCENE_DIR)/budget-old.log 2>&1 || rc=$$?; \
	 cat $(SVG_SCENE_DIR)/budget-old.log; test $$rc -eq 1 && \
	 rg -q '^svg-scene-budget: 5 checks, 1 failures$$' $(SVG_SCENE_DIR)/budget-old.log && \
	 rg -q '^FAIL: surface work budget rejects empty opacity groups$$' $(SVG_SCENE_DIR)/budget-old.log
test-svg-scene-budget: test-svg-scene-budget-negctl $(SVG_SCENE_DIR)/budget
	@$(SVG_SCENE_DIR)/budget
test-svg-scene: test-svg-scene-budget
test-svg-scene-asan: test-svg-scene-budget

$(SVG_SCENE_DIR)/budget-asan: $(SVG_SCENE_DEP) tests/unit/svg_scene_budget_test.c
	@mkdir -p $(SVG_SCENE_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(IMG_HOST_INC) -DSV_WORK_LIMIT=8192 $(SVG_SCENE_BUDGET_SRC) -o $@
.PHONY: test-svg-scene-budget-asan
test-svg-scene-budget-asan: test-svg-scene-budget-negctl $(SVG_SCENE_DIR)/budget-asan
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(SVG_SCENE_DIR)/budget-asan
test-svg-scene-asan: test-svg-scene-budget-asan

# Frozen artifacts are explicit inputs: never default a guest gate to build/
# or attach to an existing QEMU. The driver uses -snapshot for both images.
.PHONY: test-svg-scene-guest test-svg-scene-guest-negctl
test-svg-scene-guest-negctl:
	@test -n "$(SVG_SCENE_OLD_SNAPSHOT)" || { echo 'Set SVG_SCENE_OLD_SNAPSHOT to a frozen baseline directory'; exit 2; }
	@python3 tests/unit/svg_scene_guest.py --snapshot "$(SVG_SCENE_OLD_SNAPSHOT)" --out "$(SVG_SCENE_DIR)/guest-old" --expect-old
test-svg-scene-guest: test-svg-scene-guest-negctl
	@test -n "$(SVG_SCENE_SNAPSHOT)" || { echo 'Set SVG_SCENE_SNAPSHOT to the frozen current directory'; exit 2; }
	@python3 tests/unit/svg_scene_guest.py --snapshot "$(SVG_SCENE_SNAPSHOT)" --out "$(SVG_SCENE_DIR)/guest-current"
