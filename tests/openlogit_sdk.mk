# Included after sysroot.mk: its stamp rebuilds the libc tree from scratch,
# then this stamp installs the SDK. Ordering prevents libc regeneration from
# silently deleting headers/archive after the disk has already packed them.
OPENLOGIT_3D_OBJ := $(patsubst c/lib/gfx3d/%.c,$(BUILD)/sdk/%.o,$(OL3D_SRC))
OPENLOGIT_PUBLIC := $(wildcard c/lib/gfx/include/*.h) $(wildcard c/lib/gfx3d/include/openlogit*.h) c/apps/gui/openlogit_window.h c/apps/logit.h c/apps/text_metrics_wiring.inc $(wildcard include/abi/*.h)
OPENLOGIT_EXAMPLES := $(shell find examples/openlogit -type f)
include tests/openlogit_2d_perf.mk
$(BUILD)/sdk/%.o: c/lib/gfx3d/%.c $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/libopenlogit.a: $(GFX_OBJ) $(OPENLOGIT_3D_OBJ) tests/openlogit_sdk.mk
	@mkdir -p $(dir $@)
	@# Recreate the archive: ar replacement alone retains removed/renamed members,
	@# silently linking the old monolithic VM after a directory split.
	@rm -f $@.tmp
	$(SYSROOT_AR) rcsD --format=gnu $@.tmp $(GFX_OBJ) $(OPENLOGIT_3D_OBJ)
	@mv $@.tmp $@
$(BUILD)/sdk/island.o: examples/openlogit/island.c $(OPENLOGIT_EXAMPLES) $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iexamples/openlogit -c $< -o $@
$(BUILD)/sdk/effects.o: examples/openlogit/effects.c examples/openlogit/example_log.h $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/materials.o: examples/openlogit/materials.c examples/openlogit/ui_shaders.h examples/openlogit/example_log.h $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/olscc.o: c/apps/coreutils/olscc.c $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/lslcc.o: c/apps/coreutils/lslcc.c $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/studio/%.o: examples/openlogit/studio/%.c $(OPENLOGIT_EXAMPLES) $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/vector/%.o: examples/openlogit/vector/%.c $(OPENLOGIT_EXAMPLES) $(OPENLOGIT_PUBLIC)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/sdk/vector-studio.elf: $(addprefix $(BUILD)/sdk/vector/,main.o artwork.o ui.o) $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x68000000 -o $@ $(SYSROOT_WORK)/crt1.o $(addprefix $(BUILD)/sdk/vector/,main.o artwork.o ui.o) --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/scene-studio.elf: $(addprefix $(BUILD)/sdk/studio/,main.o scene.o ui.o) $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x67000000 -o $@ $(SYSROOT_WORK)/crt1.o $(addprefix $(BUILD)/sdk/studio/,main.o scene.o ui.o) --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/lslcc.elf: $(BUILD)/sdk/lslcc.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/island.elf: $(BUILD)/sdk/island.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x63000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/olscc.elf: $(BUILD)/sdk/olscc.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x64000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/materials.elf: $(BUILD)/sdk/materials.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x65000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/sdk/installed.stamp: $(SYSROOT_WORK)/sysroot.stamp $(BUILD)/sdk/libopenlogit.a $(BUILD)/sdk/island.elf $(BUILD)/sdk/olscc.elf $(OPENLOGIT_PUBLIC) $(OPENLOGIT_EXAMPLES) tools/mkopenlogit_sdk.py
	python3 tools/mkopenlogit_sdk.py --sysroot $(SYSROOT) --build $(BUILD)
	@touch $@
$(DISK): $(BUILD)/sdk/installed.stamp
$(BUILD)/sdk/installed.stamp: $(BUILD)/sdk/effects.o $(BUILD)/sdk/materials.elf $(BUILD)/sdk/lslcc.elf $(BUILD)/sdk/scene-studio.elf
openlogit-sdk: $(BUILD)/sdk/installed.stamp
$(BUILD)/sdk/installed.stamp: $(BUILD)/sdk/vector-studio.elf
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

test-openlogit-material-os: test-openlogit-material test-openlogit-consumers $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-material.py --build $(BUILD)
ci-boot: test-openlogit-material-os
.PHONY: test-openlogit-material-os

test-openlogit-scene-os: test-openlogit-scene test-openlogit-consumers $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-scene.py --build $(BUILD)
ci-boot: test-openlogit-scene-os
.PHONY: test-openlogit-scene-os

test-openlogit-vector-os: test-openlogit-canvas test-openlogit-consumers $(ISO) $(DISK)
	python3 tests/boot/run-openlogit-vector.py --build $(BUILD)
ci-boot: test-openlogit-vector-os
.PHONY: test-openlogit-vector-os
