# tests/aui_damage.mk -- aui flushes a SET of damage rectangles per frame, not
# one bounding box (c/apps/gui/aui_damage.h). Coverage is checked as a
# randomised property; tightness by the cases that motivated it. The control
# restores the single box, keeps coverage, and must fail tightness.

.PHONY: test-aui-damage test-aui-damage-negctl

test-aui-damage-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAUI_FLUSH_SINGLE_RECT -Ic/apps/gui \
		-o $(BUILD)/aui_damage_negctl tests/unit/aui_damage_test.c
	@if $(BUILD)/aui_damage_negctl > $(BUILD)/aui_damage_negctl.log 2>&1; then \
		cat $(BUILD)/aui_damage_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: the suite passes with one bounding box"; exit 1; \
	elif grep -q '^FAIL coverage' $(BUILD)/aui_damage_negctl.log; then \
		cat $(BUILD)/aui_damage_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: coverage broke"; exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/aui_damage_negctl.log) tightness checks fail on one box"; \
	fi

test-aui-damage: test-aui-damage-negctl
	@$(CC) -O2 -Wall -Wextra -Ic/apps/gui -o $(BUILD)/aui_damage_test tests/unit/aui_damage_test.c
	@$(BUILD)/aui_damage_test

ci-host: test-aui-damage
