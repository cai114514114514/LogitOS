# Included after sysroot.mk: its stamp rebuilds the libc tree from scratch,
# then this stamp installs the SDK. Ordering prevents libc regeneration from
# silently deleting headers/archive after the disk has already packed them.
OPENLOGIT_3D_OBJ := $(patsubst c/lib/gfx3d/%.c,$(BUILD)/sdk/%.o,$(OL3D_SRC))
OPENLOGIT_PUBLIC := c/lib/gfx/gfx.h $(filter-out c/lib/gfx/openlogit_sw.h,$(wildcard c/lib/gfx/openlogit*.h)) c/lib/gfx3d/openlogit_3d.h c/apps/gui/openlogit_window.h c/apps/logit.h $(wildcard include/abi/*.h)
OPENLOGIT_EXAMPLES := $(wildcard examples/openlogit/*)
$(BUILD)/sdk/%.o: c/lib/gfx3d/%.c $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/libopenlogit.a: $(GFX_OBJ) $(OPENLOGIT_3D_OBJ)
	@mkdir -p $(dir $@)
	$(SYSROOT_AR) rcsD --format=gnu $@ $(GFX_OBJ) $(OPENLOGIT_3D_OBJ)
$(BUILD)/sdk/island.o: examples/openlogit/island.c $(OPENLOGIT_EXAMPLES) $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iexamples/openlogit -c $< -o $@
$(BUILD)/sdk/effects.o: examples/openlogit/effects.c examples/openlogit/example_log.h $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/olscc.o: c/apps/coreutils/olscc.c $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/island.elf: $(BUILD)/sdk/island.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x63000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/olscc.elf: $(BUILD)/sdk/olscc.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x64000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/installed.stamp: $(SYSROOT_WORK)/sysroot.stamp $(BUILD)/sdk/libopenlogit.a $(BUILD)/sdk/island.elf $(BUILD)/sdk/olscc.elf $(OPENLOGIT_PUBLIC) $(OPENLOGIT_EXAMPLES) tools/mkopenlogit_sdk.py
	python3 tools/mkopenlogit_sdk.py --sysroot $(SYSROOT) --build $(BUILD)
	@touch $@
$(DISK): $(BUILD)/sdk/installed.stamp
$(BUILD)/sdk/installed.stamp: $(BUILD)/sdk/effects.o
openlogit-sdk: $(BUILD)/sdk/installed.stamp
ci-host: test-openlogit-game
.PHONY: openlogit-sdk test-openlogit-game
$(BUILD)/island_game_test: tests/unit/island_game_test.c examples/openlogit/island_game.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Iexamples/openlogit $< -lm -o $@
test-openlogit-game: $(BUILD)/island_game_test
	$<

test-openlogit-game-os: test-openlogit-game test-openlogit-3d test-openlogit-consumers $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-game.py --build $(BUILD)
ci-boot: test-openlogit-game-os
.PHONY: test-openlogit-game-os

test-openlogit-effects-os: test-openlogit-effects test-openlogit-consumers $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-effects.py --build $(BUILD)
ci-boot: test-openlogit-effects-os
.PHONY: test-openlogit-effects-os
