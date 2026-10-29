# Version-3 native frontend and toolchain; kept in the original as executable.
AS_NATIVE_MANIFEST := c/apps/as/runtime/sources.def
AS_NATIVE_UNITS := $(shell sed -n 's/^AT_RUNTIME_SOURCE(\([a-z_]*\))$$/\1/p' $(AS_NATIVE_MANIFEST))
AS_NATIVE_SRCS := $(addprefix c/apps/as/runtime/,$(addsuffix .c,$(AS_NATIVE_UNITS)))
AS_NATIVE_OBJS := $(addprefix $(BUILD)/aether-toolchain/obj/,$(addsuffix .o,$(AS_NATIVE_UNITS)))
AS_NATIVE_HEADERS := $(wildcard c/apps/as/runtime/*.h)
AS_NATIVE_COPY := $(patsubst c/apps/as/runtime/%,$(BUILD)/aether-toolchain/%,$(AS_NATIVE_SRCS) $(AS_NATIVE_HEADERS) $(AS_NATIVE_MANIFEST))
AS_SYSTEM_CONSTANT_INPUTS := c/apps/as/common/system_constants.def $(wildcard include/abi/*.h)
$(BUILD)/asc $(BUILD)/as-native-link-test: $(AS_SYSTEM_CONSTANT_INPUTS)
$(BUILD)/as-native-link-test: $(AS_NATIVE_HEADERS)
$(BUILD)/aether-toolchain/obj/capability.o $(BUILD)/as-typed-capture.o: $(wildcard include/abi/*.h)
$(BUILD)/asobj/c/apps/as/sema/constants.o $(BUILD)/asobj/c/apps/as/editor/completion.o: $(AS_SYSTEM_CONSTANT_INPUTS)
# abi_layout.inc is generated and is not a .h, so no wildcard picks it up. Without
# this a regenerated ABI leaves constants.o holding yesterday's _Static_asserts --
# the check that exists to catch a moved kernel field reporting on a struct nobody
# has. The same line used to name legacy/builtins.o.
$(BUILD)/asobj/c/apps/as/sema/constants.o $(BUILD)/c/apps/as/sema/constants.o: c/apps/as/sema/abi_layout.inc
$(BUILD)/apps/as/editor/completion.o: c/apps/as/common/system_constants.def
# abi_layout.inc is named here as well as on the .o lines above, and it has to be:
# $(ASC) is built from the SOURCE list in one compiler invocation, not from those
# objects, so a dependency that mentions only the objects leaves asc up to date
# against a regenerated ABI. Found by a negative control that did not fire --
# perturbing an assertion changed nothing, because nothing was recompiled.
$(BUILD)/asc: $(AS_HDRS) $(AS_NATIVE_MANIFEST) c/apps/as/sources.mk c/apps/as/sema/abi_layout.inc
$(BUILD)/asobj/c/apps/as/cli/commands.o: $(AS_NATIVE_MANIFEST)
# A rule here used to copy fsroot/as/compat2/NAME.lacache to $(BUILD)/NAME.la,
# so sixteen of the seventeen standard-library modules shipped as frozen A2
# bytecode while their A3 sources sat beside them unread. Both the caches and
# the engine that preferred them are gone; the library ships as source and is
# linked into each native .aex by the host compiler.

AS_TYPED_LIBRARY := $(patsubst fsroot/as/lib/%.as,$(BUILD)/aether-toolchain/lib/%.as,$(AS_LIB_SRCS))
as-toolchain: $(AS_TYPED_LIBRARY)
$(BUILD)/aether-toolchain/lib/%.as: fsroot/as/lib/%.as
	@mkdir -p $(dir $@)
	cp $< $@
.PHONY: as-toolchain
.PHONY: test-as-promotion test-as-promotion-negctl test-as-promotion-guest
test-as-promotion-negctl: $(BUILD)/asc as-toolchain
	python3 tests/unit/as_promotion_test.py $(BUILD)/asc --negative-control
test-as-promotion: test-as-promotion-negctl
	python3 tests/unit/as_promotion_test.py $(BUILD)/asc
$(BUILD)/mixed.aex: tests/fixtures/astyped/mixed_numeric/main.as $(BUILD)/asc $(AS_NATIVE_SRCS) $(AS_NATIVE_HEADERS) | as-toolchain
	$(BUILD)/asc build $< --target logitos-x86_64 --toolchain $(BUILD)/aether-toolchain -o $@
test-as-promotion-guest: test-as-promotion test-as-eval $(BUILD)/as.aex $(BUILD)/studio.aex $(BUILD)/mixed.aex
	python3 tests/boot/run-as-numeric.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --native $(BUILD)/mixed.aex --out $(BUILD)/numeric-guest-$$$$
ci-boot: test-as-promotion-guest
.PHONY: test-as-lexer-lib-negctl
# These probes exercise the ownership backend only. Source-language Region is
# still gated on static lifetime analysis; a C runtime pass cannot claim that.
AS_REGION_TRACKED_SRCS := $(filter %/region.c %/region_borrow.c,$(AS_NATIVE_SRCS))
AS_REGION_TRACKED_OBJS := $(patsubst c/apps/as/runtime/%.c,$(BUILD)/region-tracked/%.o,$(AS_REGION_TRACKED_SRCS))
$(BUILD)/region-tracked/%.o: c/apps/as/runtime/%.c $(AS_NATIVE_HEADERS)
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -Dcalloc=test_region_allocate -Dfree=test_region_free -c $< -o $@
# native.h also exists elsewhere in the OS-wide include search. Put the
# runtime first so this standalone probe sees the exact headers under test.
$(BUILD)/as-region-runtime.o: tests/unit/as_region_runtime_test.c $(AS_NATIVE_HEADERS)
	@mkdir -p $(dir $@)
	$(COMPILE) -Ic/apps/as/runtime $(UCFLAGS) -c $< -o $@
$(BUILD)/as-region-runtime.elf: $(BUILD)/as-region-runtime.o $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-region-tracked.elf: $(BUILD)/as-region-runtime.o $(AS_REGION_TRACKED_OBJS) $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(AS_REGION_TRACKED_OBJS) $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-region-runtime.aex: $(BUILD)/as-region-runtime.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ region-runtime --cli
$(BUILD)/as-region-tracked.aex: $(BUILD)/as-region-tracked.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ region-tracked --cli
AS_REGION_GUEST_OUT ?= $(BUILD)/region-runtime-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-settings test-as-settings-negctl
.PHONY: test-as-durability test-as-durability-negctl
test-as-durability-negctl: $(BUILD)/asc
	python3 tests/unit/as_durability_test.py $(BUILD)/asc --negative-control
test-as-durability: test-as-durability-negctl
test-as-typed test-durability test-hugefile test-fscrash: test-as-durability
test-as-settings-negctl: $(BUILD)/asc
	python3 tests/unit/as_settings_test.py $(BUILD)/asc --negative-control
test-as-settings: test-as-settings-negctl
test-as-typed test-settings-os test-desktop-os: test-as-settings
AS_SETTINGS_GUEST_OUT ?= $(BUILD)/settings-native-$(shell date +%Y%m%d-%H%M%S)
AS_CHAT_GUEST_OUT ?= $(BUILD)/chat-native-$(shell date +%Y%m%d-%H%M%S)
test-ch test-ch-refusal: test-as-preview-launchers
.PHONY: test-as-commands test-as-commands-negctl
test-as-commands-negctl: $(BUILD)/asc
	python3 tests/unit/as_command_test.py $(BUILD)/asc --negative-control
test-as-commands: test-as-commands-negctl
	python3 tests/unit/as_command_test.py $(BUILD)/asc

# Every migrated example ships as A3 machine code. Depend on library/runtime
# artifacts so a normal disk rebuild cannot retain a stale executable.
.PHONY: as-native-examples as-native-example-files test-as-example-pack test-as-example-pack-negctl
# The disk recipe and guest acceptance consume this same mapping; no second
# hand-maintained list decides where the native examples are installed.
as-native-example-files:
	@$(foreach entry,$(AS_NATIVE_EXAMPLE_PACK),printf '%s\n' '$(entry)';)
test-as-example-pack-negctl:
	python3 tests/unit/as_examples_pack_test.py --negative-control
test-as-example-pack: test-as-example-pack-negctl
	python3 tests/unit/as_examples_pack_test.py
AS_PACKAGED_GUEST_OUT ?= $(BUILD)/packaged-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-packaged-guest
test-as-packaged-guest: test-as-example-pack as-native-examples $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-packaged.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_PACKAGED_GUEST_OUT)
as-native-examples: $(AS_NATIVE_EXAMPLE_AEX)
$(DISK): $(AS_NATIVE_EXAMPLE_AEX) $(AS_LIB_SRCS) tools/as_examples.py
$(BUILD)/as-native/%.aex: fsroot/as/examples/%.as $(AS_LIB_SRCS) $(BUILD)/asc $(AS_NATIVE_SRCS) $(AS_NATIVE_HEADERS) tools/as_examples.py $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a $(BUILD)/aether-toolchain/crt0.o | as-toolchain
	@mkdir -p $(dir $@)
	$(BUILD)/asc build $< --stdlib fsroot/as/lib --target logitos-x86_64 -o $@
$(BUILD)/ash.aex: $(BUILD)/as-native/ash.aex
	cp $< $@
test-ash: test-as-commands
.PHONY: test-as-shell test-as-shell-negctl
test-as-shell-negctl: $(BUILD)/asc
	python3 tests/unit/as_shell_test.py $(BUILD)/asc --negative-control
test-as-shell: test-as-shell-negctl
	python3 tests/unit/as_shell_test.py $(BUILD)/asc
test-ash: test-as-shell
# The original lexer-tool gate now executes A3. Reuse its existing native
# lexer negative control and include this CLI adaptation in routine regression.
test-selfhost-lex: test-as-lexer-lib-negctl
test-as-lexer-lib-negctl: $(BUILD)/asc
	python3 tests/unit/as_lexer_lib_test.py $(BUILD)/asc --negative-control
# AS_NATIVE_FRONTEND shares the compiler directory inventory in sources.mk.
$(BUILD)/as-native-link-test: tests/unit/as_native_link_test.c $(AS_NATIVE_FRONTEND) $(AS_HDRS) $(AS_NATIVE_MANIFEST) c/apps/as/sources.mk
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Ic/apps/as $< $(AS_NATIVE_FRONTEND) -o $@ -lm
AS_VIEWER_GUEST_OUT ?= $(BUILD)/viewer-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-viewer-negctl test-as-viewer-guest
test-as-viewer-negctl:
	python3 tests/unit/as_viewer_test.py --negative-control
test-as-viewer-guest: test-as-viewer-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_VIEWER_GUEST_OUT) --case example-asview --case optional
AS_CLOCK_GUEST_OUT ?= $(BUILD)/clock-guest-$(shell date +%Y%m%d-%H%M%S)
AS_INPUT_GUEST_OUT ?= $(BUILD)/input-guest-$(shell date +%Y%m%d-%H%M%S)
AS_CAPCHECK_GUEST_OUT ?= $(BUILD)/capcheck-guest-$(shell date +%Y%m%d-%H%M%S)
AS_BARRIER_GUEST_OUT ?= $(BUILD)/barrier-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-barrier-negctl
test-as-barrier-negctl: $(BUILD)/asc
	python3 tests/unit/as_barrier_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-barrier-negctl
.PHONY: test-as-capcheck-negctl test-as-capcheck-guest
test-as-capcheck-negctl: $(BUILD)/asc
	python3 tests/unit/as_capcheck_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-capcheck-negctl
test-as-capcheck-guest: test-as-capcheck-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_CAPCHECK_GUEST_OUT) --case example-capcheck
.PHONY: test-as-input-negctl
test-as-input-negctl: $(BUILD)/asc
	python3 tests/unit/as_input_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-input-negctl
.PHONY: test-as-clock-oracle test-as-clock-guest
test-as-clock-oracle:
	python3 tests/unit/as_clock_test.py --negative-control
# Controls are part of this invocation, not optional flags: each mode must
# reject a stopped clock and a tick/ms mix-up before the guest gate is green.
test-as-clock-guest: test-as-clock-oracle as-toolchain $(BUILD)/as-typed-capture.aex $(AS_TYPED_GUEST_BASE)/logit.iso $(addprefix $(AS_TYPED_GUEST_BASE)/,login.aex sh.aex echo.aex cat.aex)
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_CLOCK_GUEST_OUT) --case example-monotonic --case clock-dead --case clock-ticks
AS_TYPED_GUEST_OUT ?= $(BUILD)/typed-guest-$(shell date +%Y%m%d-%H%M%S)
AS_NATIVE_CLI_GUEST_OUT ?= $(BUILD)/native-cli-guest-$(shell date +%Y%m%d-%H%M%S)
$(BUILD)/as-typed-capture.o: tests/unit/as_typed_guest_capture.c
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@
$(BUILD)/as-typed-capture.elf: $(BUILD)/as-typed-capture.o $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-typed-capture.aex: $(BUILD)/as-typed-capture.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ native-capture --cli
as-toolchain: $(AS_NATIVE_COPY) $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a $(BUILD)/aether-toolchain/mkaex.py
$(AS_NATIVE_COPY): $(BUILD)/aether-toolchain/%: c/apps/as/runtime/%
	@mkdir -p $(dir $@)
	cp $< $@
$(BUILD)/aether-toolchain/mkaex.py: tools/mkaex.py
	@mkdir -p $(dir $@)
	cp $< $@
$(BUILD)/aether-toolchain/native.a: $(AS_NATIVE_OBJS) $(AS_NATIVE_MANIFEST)
	@mkdir -p $(dir $@)
	$(AGENT_AR) rcs $@ $(AS_NATIVE_OBJS)
# Every native object emits a compiler .d file. The former all-header edge
# rebuilt all 27 objects for allocation.h even though the product depfiles
# name only allocation.o (command.h names two). Keep these objects in
# product.mk's missing-.d recovery inventory: simply removing the edge would
# let an old surviving .o without its .d silently miss a future header edit.
# The archive and static guest programs still relink when a real consumer
# changes; this only avoids recompiling the unrelated runtime units.
$(BUILD)/aether-toolchain/obj/%.o: c/apps/as/runtime/%.c
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@
$(BUILD)/aether-toolchain/crt0.o: c/apps/crt0_cli.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
# Float `**` lowers to an llvm.pow.f64 libcall; the freestanding guest has no
# libSystem, so musl's pow (and its data tables) ride in this archive. Archive
# semantics pull pow.o only into programs that actually power -- AetherScript
# emitted no libm libcall before float **, which is why this held no libm.
# powf deliberately stays absent: third_party/libm ships powf_data.c but no
# powf.c, so f32 power widens to f64 in the backend instead (numeric.c).
$(BUILD)/aether-toolchain/libc.a: $(LIBC_OBJS) $(LIBM_OBJ)
	@mkdir -p $(dir $@)
	$(AGENT_AR) rcs $@ $(LIBC_OBJS) $(LIBM_OBJ)
