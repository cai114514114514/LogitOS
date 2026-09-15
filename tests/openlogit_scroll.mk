# AUI keeps input/state ownership and uses the SDK's actual pixel sampler.
OL_SCROLL_DEPS := c/apps/gui/aui_scroll_motion.h c/lib/gfx/include/openlogit_anim.h
OL_SCROLL_SRC := c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/geometry/gfx_math.c
$(BUILD)/apps/aui.o: $(OL_SCROLL_DEPS)
$(BUILD)/openlogit_scroll_test: tests/unit/openlogit_scroll_test.c $(OL_SCROLL_SRC) $(OL_SCROLL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Ic/apps/gui $(GFX_INC) $< $(OL_SCROLL_SRC) -lm -o $@
$(BUILD)/openlogit_scroll_neg: tests/unit/openlogit_scroll_test.c $(OL_SCROLL_SRC) $(OL_SCROLL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DAUI_SCROLL_INSTANT -Ic/apps/gui $(GFX_INC) $< $(OL_SCROLL_SRC) -lm -o $@
$(BUILD)/openlogit_scroll_sanitize: tests/unit/openlogit_scroll_test.c $(OL_SCROLL_SRC) $(OL_SCROLL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -Ic/apps/gui $(GFX_INC) $< $(OL_SCROLL_SRC) -lm -o $@
test-openlogit-scroll-neg: $(BUILD)/openlogit_scroll_neg
	@rc=0; $< > $(BUILD)/openlogit-scroll-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL scroll has a real SDK intermediate value' $(BUILD)/openlogit-scroll-neg.log
test-openlogit-scroll: test-openlogit-scroll-neg $(BUILD)/openlogit_scroll_test $(BUILD)/openlogit_scroll_sanitize
	$(BUILD)/openlogit_scroll_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_scroll_sanitize
ci-host: test-openlogit-scroll
test-openlogit: test-openlogit-scroll
.PHONY: test-openlogit-scroll test-openlogit-scroll-neg

test-openlogit-scroll-consumers-neg:
	@mkdir -p $(BUILD)
	@rc=0; python3 tools/check_openlogit_consumers.py --inject-missing-motion > $(BUILD)/scroll-consumers-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL c/apps/gui/ch/ch.c: scroll consumer missing animation wake integration' $(BUILD)/scroll-consumers-neg.log
test-openlogit-consumers: test-openlogit-scroll-consumers-neg
.PHONY: test-openlogit-scroll-consumers-neg

# Test-only programs enter the isolated image only for this explicit gate.
# Both link the real toolkit; the control changes just its wheel interpolation.
OL_SCROLL_GUEST_DEPS = c/apps/gui/aui.c c/apps/gui/aui.h $(OL_SCROLL_DEPS) c/apps/logit.h
$(BUILD)/scroll/fixture.o: tests/fixtures/openlogit_scroll.c c/apps/gui/aui.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/scroll/crt0.o: c/apps/crt0.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(BUILD)/scroll/aui_instant.o: $(OL_SCROLL_GUEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DAUI_SCROLL_INSTANT -c c/apps/gui/aui.c -o $@
$(BUILD)/scroll/scroll.elf: $(BUILD)/scroll/crt0.o $(BUILD)/scroll/fixture.o $(BUILD)/apps/aui.o $(GFX_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(BUILD)/scroll/instant.elf: $(BUILD)/scroll/crt0.o $(BUILD)/scroll/fixture.o $(BUILD)/scroll/aui_instant.o $(GFX_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(BUILD)/scroll/installed.stamp: $(BUILD)/scroll/scroll.elf $(BUILD)/scroll/instant.elf $(BUILD)/sdk/installed.stamp
	@mkdir -p $(SYSROOT)/bin
	cp $(BUILD)/scroll/scroll.elf $(SYSROOT)/bin/scroll-probe
	cp $(BUILD)/scroll/instant.elf $(SYSROOT)/bin/scroll-instant
	@touch $@
ifeq ($(OPENLOGIT_SCROLL_GUEST),1)
$(DISK): $(BUILD)/scroll/installed.stamp
endif
test-openlogit-scroll-os: test-openlogit-scroll
	$(MAKE) BUILD=$(BUILD) OPENLOGIT_SCROLL_GUEST=1 $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-scroll.py --build $(BUILD)
ci-boot: test-openlogit-scroll-os
.PHONY: test-openlogit-scroll-os
