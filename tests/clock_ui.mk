# Clock is a normal installed application, with separate translation units.
# The agent integration still augments clock.elf's target-specific LD later.
CLOCK_UI_OBJ := $(addprefix $(BUILD)/apps/clock/,main.o model.o) $(addprefix $(BUILD)/apps/gui/clock/,dial.o view.o layout.o motion.o)
CLOCK_UI_HEADERS := $(wildcard c/apps/clock/*.h c/apps/gui/clock/*.h) c/apps/gui/aui.h c/apps/gui/openlogit_window.h
$(BUILD)/apps/clock/%.o: c/apps/clock/%.c $(CLOCK_UI_HEADERS) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/apps/gui/clock/%.o: c/apps/gui/clock/%.c $(CLOCK_UI_HEADERS) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/clock.elf: $(CLOCK_UI_OBJ) $(BUILD)/apps/aui.o $(GFX_OBJ) c/apps/crt0.asm tests/clock_ui.mk
	$(ASM) -f elf64 c/apps/crt0.asm -o $(BUILD)/apps/clock.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x40000000 -o $@ $(BUILD)/apps/clock.crt0.o $(CLOCK_UI_OBJ) $(BUILD)/apps/aui.o $(GFX_OBJ)
$(BUILD)/clock.aex: $(BUILD)/clock.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Clock - C 100 160 255
CLOCK_UI_TEST_SRC := tests/unit/clock_ui_test.c c/apps/clock/model.c c/apps/gui/clock/layout.c c/apps/gui/clock/motion.c $(GFX_SRC)
$(BUILD)/clock_ui_test: $(CLOCK_UI_TEST_SRC) $(CLOCK_UI_HEADERS) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined $(GFX_INC) -Ic/apps/clock -Ic/apps/gui/clock $(CLOCK_UI_TEST_SRC) -lm -o $@
$(BUILD)/clock_ui_negative: $(CLOCK_UI_TEST_SRC) $(CLOCK_UI_HEADERS) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -DCLOCK_MOTION_DISABLED $(GFX_INC) -Ic/apps/clock -Ic/apps/gui/clock $(CLOCK_UI_TEST_SRC) -lm -o $@
test-clock-ui-negative: $(BUILD)/clock_ui_negative
	@rc=0; $< > $(BUILD)/clock-ui-negative.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL shared easing' $(BUILD)/clock-ui-negative.log
test-clock-ui: test-clock-ui-negative $(BUILD)/clock_ui_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/clock_ui_test
ci-host: test-clock-ui
.PHONY: test-clock-ui test-clock-ui-negative
test-clock-ui-os: test-clock-ui $(ISO) $(DISK)
	python3 tests/boot/run-clock-ui.py --build $(BUILD) --width 1280
	python3 tests/boot/run-clock-ui.py --build $(BUILD) --width 1920
ci-boot: test-clock-ui-os
.PHONY: test-clock-ui-os
