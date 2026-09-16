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
.PHONY: as-toolchain test-as-typed test-as-typed-negctl
.PHONY: test-as-typed-guest
.PHONY: test-as-generic test-as-generic-negctl
.PHONY: test-as-native-parity test-as-native-parity-negctl
.PHONY: test-as-managed test-as-managed-negctl
.PHONY: test-as-a3-only
.PHONY: test-as-native-link
.PHONY: test-as-globals test-as-globals-negctl
.PHONY: test-as-any test-as-any-negctl
.PHONY: test-as-conversions test-as-conversions-negctl
.PHONY: test-as-dict test-as-dict-negctl
.PHONY: test-as-callable test-as-callable-negctl
.PHONY: test-as-stats test-as-stats-negctl
.PHONY: test-as-collections test-as-collections-negctl
.PHONY: test-as-format test-as-format-negctl
.PHONY: test-as-optional test-as-optional-negctl
.PHONY: test-as-class test-as-class-negctl
.PHONY: test-as-lexer-lib test-as-lexer-lib-negctl
.PHONY: test-as-fstring test-as-fstring-negctl
.PHONY: test-as-range test-as-range-negctl
.PHONY: test-as-comprehension test-as-comprehension-negctl
.PHONY: test-as-assignment test-as-assignment-negctl
.PHONY: test-as-examples test-as-examples-negctl
.PHONY: test-as-process test-as-process-negctl
.PHONY: test-as-closure test-as-closure-negctl
.PHONY: test-as-inheritance test-as-inheritance-negctl
.PHONY: test-as-buffer test-as-buffer-negctl
.PHONY: test-as-bytes test-as-bytes-negctl
.PHONY: test-as-files test-as-files-negctl
.PHONY: test-as-ports test-as-ports-negctl
.PHONY: test-as-system test-as-system-negctl
.PHONY: test-as-pointer test-as-pointer-negctl
test-as-pointer-negctl: $(BUILD)/asc
	python3 tests/unit/as_pointer_test.py $(BUILD)/asc --negative-control
test-as-pointer: test-as-pointer-negctl
	python3 tests/unit/as_pointer_test.py $(BUILD)/asc
test-as-typed: test-as-pointer
.PHONY: test-as-allocation test-as-allocation-negctl
test-as-allocation-negctl: $(BUILD)/asc
	python3 tests/unit/as_allocation_test.py $(BUILD)/asc --negative-control
test-as-allocation: test-as-allocation-negctl
	python3 tests/unit/as_allocation_test.py $(BUILD)/asc
test-as-typed: test-as-allocation
.PHONY: test-as-region-runtime test-as-region-runtime-negctl
test-as-region-runtime-negctl:
	python3 tests/unit/as_region_test.py --negative-control
test-as-region-runtime: test-as-region-runtime-negctl
	python3 tests/unit/as_region_test.py
test-as-typed: test-as-region-runtime
.PHONY: test-as-region-source test-as-region-source-negctl
test-as-region-source-negctl: $(BUILD)/asc
	python3 tests/unit/as_region_source_test.py $(BUILD)/asc --negative-control
test-as-region-source: test-as-region-source-negctl
	python3 tests/unit/as_region_source_test.py $(BUILD)/asc
test-as-typed: test-as-region-source
.PHONY: test-as-region-borrow test-as-region-borrow-negctl
test-as-region-borrow-negctl: $(BUILD)/asc
	python3 tests/unit/as_region_borrow_test.py $(BUILD)/asc --negative-control
test-as-region-borrow: test-as-region-borrow-negctl
	python3 tests/unit/as_region_borrow_test.py $(BUILD)/asc
test-as-typed: test-as-region-borrow
# These probes exercise the ownership backend only. Source-language Region is
# still gated on static lifetime analysis; a C runtime pass cannot claim that.
AS_REGION_TRACKED_SRCS := $(filter %/region.c %/region_borrow.c,$(AS_NATIVE_SRCS))
AS_REGION_TRACKED_OBJS := $(patsubst c/apps/as/runtime/%.c,$(BUILD)/region-tracked/%.o,$(AS_REGION_TRACKED_SRCS))
$(BUILD)/region-tracked/%.o: c/apps/as/runtime/%.c $(AS_NATIVE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Dcalloc=test_region_allocate -Dfree=test_region_free -c $< -o $@
# native.h also exists elsewhere in the OS-wide include search. Put the
# runtime first so this standalone probe sees the exact headers under test.
$(BUILD)/as-region-runtime.o: tests/unit/as_region_runtime_test.c $(AS_NATIVE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -Ic/apps/as/runtime $(UCFLAGS) -c $< -o $@
$(BUILD)/as-region-runtime.elf: $(BUILD)/as-region-runtime.o $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-region-tracked.elf: $(BUILD)/as-region-runtime.o $(AS_REGION_TRACKED_OBJS) $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(AS_REGION_TRACKED_OBJS) $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-region-runtime.aex: $(BUILD)/as-region-runtime.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ region-runtime --cli
$(BUILD)/as-region-tracked.aex: $(BUILD)/as-region-tracked.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ region-tracked --cli
AS_REGION_GUEST_OUT ?= $(BUILD)/region-runtime-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-region-runtime-guest
test-as-region-runtime-guest: test-as-region-runtime $(BUILD)/as-region-runtime.aex $(BUILD)/as-region-tracked.aex $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-region-runtime.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_REGION_GUEST_OUT)
test-as-a3-only: test-as-region-runtime-guest
.PHONY: test-as-abi test-as-abi-negctl
test-as-abi-negctl: $(BUILD)/asc
	python3 tests/unit/as_abi_test.py $(BUILD)/asc --negative-control
test-as-abi: test-as-abi-negctl
	python3 tests/unit/as_abi_test.py $(BUILD)/asc
test-as-typed: test-as-abi
.PHONY: test-as-sys-lib test-as-sys-lib-negctl
test-as-sys-lib-negctl: $(BUILD)/asc
	python3 tests/unit/as_sys_test.py $(BUILD)/asc --negative-control
test-as-sys-lib: test-as-sys-lib-negctl
	python3 tests/unit/as_sys_test.py $(BUILD)/asc
test-as-typed: test-as-sys-lib
.PHONY: test-as-settings test-as-settings-negctl test-as-settings-guest
.PHONY: test-as-durability test-as-durability-negctl test-as-durability-guest
test-as-durability-negctl: $(BUILD)/asc
	python3 tests/unit/as_durability_test.py $(BUILD)/asc --negative-control
test-as-durability: test-as-durability-negctl
test-as-typed test-durability test-hugefile test-fscrash: test-as-durability
test-as-durability-guest: test-as-durability as-toolchain $(AS_TYPED_GUEST_BASE)/logit.iso $(BUILD)/login.aex $(BUILD)/sh.aex $(BUILD)/echo.aex $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-durability.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(BUILD)/durability-native-guest-$$$$
test-as-a3-only: test-as-durability-guest
test-as-settings-negctl: $(BUILD)/asc
	python3 tests/unit/as_settings_test.py $(BUILD)/asc --negative-control
test-as-settings: test-as-settings-negctl
test-as-typed test-settings-os test-desktop-os: test-as-settings
AS_SETTINGS_GUEST_OUT ?= $(BUILD)/settings-native-$(shell date +%Y%m%d-%H%M%S)
test-as-settings-guest: test-as-settings as-toolchain $(AS_TYPED_GUEST_BASE)/logit.iso $(BUILD)/login.aex $(BUILD)/sh.aex $(BUILD)/echo.aex $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-settings.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_SETTINGS_GUEST_OUT)
test-as-a3-only: test-as-settings-guest
.PHONY: test-as-chat-launcher-guest
AS_CHAT_GUEST_OUT ?= $(BUILD)/chat-native-$(shell date +%Y%m%d-%H%M%S)
test-ch test-ch-refusal: test-as-preview-launchers
test-as-chat-launcher-guest: test-as-preview-launchers as-toolchain $(AS_TYPED_GUEST_BASE)/logit.iso $(BUILD)/login.aex $(BUILD)/sh.aex $(BUILD)/echo.aex $(BUILD)/rm.aex $(BUILD)/ch.aex
	python3 tests/boot/run-as-chat-launcher.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_CHAT_GUEST_OUT)
test-as-a3-only: test-as-chat-launcher-guest
.PHONY: test-as-image-lib test-as-image-lib-negctl
test-as-image-lib-negctl: $(BUILD)/asc
	python3 tests/unit/as_image_test.py $(BUILD)/asc --negative-control
test-as-image-lib: test-as-image-lib-negctl
	python3 tests/unit/as_image_test.py $(BUILD)/asc
test-as-typed: test-as-image-lib
.PHONY: test-as-gui-lib test-as-gui-lib-negctl
test-as-gui-lib-negctl: $(BUILD)/asc
	python3 tests/unit/as_gui_test.py $(BUILD)/asc --negative-control
test-as-gui-lib: test-as-gui-lib-negctl
	python3 tests/unit/as_gui_test.py $(BUILD)/asc
test-as-typed: test-as-gui-lib
.PHONY: test-as-temporary-roots test-as-temporary-roots-negctl
test-as-temporary-roots-negctl: $(BUILD)/asc
	python3 tests/unit/as_temporary_roots_test.py $(BUILD)/asc --negative-control
test-as-temporary-roots: test-as-temporary-roots-negctl
	python3 tests/unit/as_temporary_roots_test.py $(BUILD)/asc
test-as-typed: test-as-temporary-roots
.PHONY: test-as-native-cli test-as-native-cli-negctl
.PHONY: test-as-snapshot
test-as-snapshot: $(BUILD)/asc as-toolchain
	python3 tests/unit/as_snapshot_test.py $(BUILD)/asc
test-as-typed: test-as-snapshot
test-as-native-cli-negctl: $(BUILD)/asc as-toolchain
	python3 tests/unit/as_cli_test.py $(BUILD)/asc --negative-control
test-as-native-cli: test-as-native-cli-negctl
	python3 tests/unit/as_cli_test.py $(BUILD)/asc
test-as-typed: test-as-native-cli
.PHONY: test-as-layout test-as-layout-negctl
test-as-layout-negctl: $(BUILD)/asc
	python3 tests/unit/as_layout_test.py $(BUILD)/asc --negative-control
test-as-layout: test-as-layout-negctl
	python3 tests/unit/as_layout_test.py $(BUILD)/asc
test-as-typed: test-as-layout
test-as-system-negctl: $(BUILD)/asc
	python3 tests/unit/as_system_test.py $(BUILD)/asc --negative-control
test-as-system: test-as-system-negctl
	python3 tests/unit/as_system_test.py $(BUILD)/asc
test-as-typed: test-as-system
test-as-ports-negctl: $(BUILD)/asc
	python3 tests/unit/as_port_test.py $(BUILD)/asc --negative-control
test-as-ports: test-as-ports-negctl
	python3 tests/unit/as_port_test.py $(BUILD)/asc
test-as-typed: test-as-ports
.PHONY: test-as-commands test-as-commands-negctl
test-as-commands-negctl: $(BUILD)/asc
	python3 tests/unit/as_command_test.py $(BUILD)/asc --negative-control
test-as-commands: test-as-commands-negctl
	python3 tests/unit/as_command_test.py $(BUILD)/asc
test-as-typed: test-as-commands

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
test-as-typed: test-as-example-pack
AS_PACKAGED_GUEST_OUT ?= $(BUILD)/packaged-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-packaged-guest
test-as-packaged-guest: test-as-example-pack as-native-examples $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-packaged.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_PACKAGED_GUEST_OUT)
test-as-a3-only: test-as-packaged-guest
as-native-examples: $(AS_NATIVE_EXAMPLE_AEX)
$(DISK): $(AS_NATIVE_EXAMPLE_AEX) $(AS_LIB_SRCS) tools/as_examples.py
$(BUILD)/as-native/%.aex: fsroot/as/examples/%.as $(AS_LIB_SRCS) $(BUILD)/asc $(AS_NATIVE_SRCS) $(AS_NATIVE_HEADERS) tools/as_examples.py $(BUILD)/aether-toolchain/native.a $(BUILD)/aether-toolchain/libc.a $(BUILD)/aether-toolchain/crt0.o | as-toolchain
	@mkdir -p $(dir $@)
	$(BUILD)/asc build $< --stdlib fsroot/as/lib --target logitos-x86_64 -o $@
$(BUILD)/ash.aex: $(BUILD)/as-native/ash.aex
	cp $< $@
test-ash: test-as-commands
test-as-typed: test-ash
.PHONY: test-as-shell test-as-shell-negctl
test-as-shell-negctl: $(BUILD)/asc
	python3 tests/unit/as_shell_test.py $(BUILD)/asc --negative-control
test-as-shell: test-as-shell-negctl
	python3 tests/unit/as_shell_test.py $(BUILD)/asc
test-ash: test-as-shell
.PHONY: test-as-binary test-as-binary-negctl
.PHONY: test-as-imports test-as-imports-negctl
test-as-imports-negctl: $(BUILD)/asc
	python3 tests/unit/as_import_test.py $(BUILD)/asc --negative-control
test-as-imports: test-as-imports-negctl
	python3 tests/unit/as_import_test.py $(BUILD)/asc
test-as-typed: test-as-imports
# The original lexer-tool gate now executes A3. Reuse its existing native
# lexer negative control and include this CLI adaptation in routine regression.
test-selfhost-lex: test-as-lexer-lib-negctl
test-as-typed: test-selfhost-lex
test-as-binary-negctl: $(BUILD)/asc
	python3 tests/unit/as_binary_test.py $(BUILD)/asc --negative-control
test-as-binary: test-as-binary-negctl
	python3 tests/unit/as_binary_test.py $(BUILD)/asc
test-as-files-negctl: $(BUILD)/asc
	python3 tests/unit/as_file_test.py $(BUILD)/asc --negative-control
test-as-files: test-as-files-negctl
	python3 tests/unit/as_file_test.py $(BUILD)/asc
test-as-bytes-negctl: $(BUILD)/asc
	python3 tests/unit/as_bytes_test.py $(BUILD)/asc --negative-control
test-as-bytes: test-as-bytes-negctl
	python3 tests/unit/as_bytes_test.py $(BUILD)/asc
.PHONY: test-as-constants test-as-constants-negctl
.PHONY: test-as-capability test-as-capability-negctl
.PHONY: test-as-memory test-as-memory-negctl
test-as-memory-negctl: $(BUILD)/asc
	python3 tests/unit/as_memory_test.py $(BUILD)/asc --negative-control
test-as-memory: test-as-memory-negctl
	python3 tests/unit/as_memory_test.py $(BUILD)/asc
test-as-capability-negctl: $(BUILD)/asc
	python3 tests/unit/as_capability_test.py $(BUILD)/asc --negative-control
test-as-capability: test-as-capability-negctl
	python3 tests/unit/as_capability_test.py $(BUILD)/asc
test-as-constants-negctl: $(BUILD)/asc
	python3 tests/unit/as_constants_test.py $(BUILD)/asc --negative-control
test-as-constants: test-as-constants-negctl $(BUILD)/as-native-link-test
	python3 tests/unit/as_constants_test.py $(BUILD)/asc
test-as-buffer-negctl: $(BUILD)/asc
	python3 tests/unit/as_buffer_test.py $(BUILD)/asc --negative-control
test-as-buffer: test-as-buffer-negctl
	python3 tests/unit/as_buffer_test.py $(BUILD)/asc
test-as-inheritance-negctl: $(BUILD)/asc
	python3 tests/unit/as_inheritance_test.py $(BUILD)/asc --negative-control
test-as-inheritance: test-as-inheritance-negctl
	python3 tests/unit/as_inheritance_test.py $(BUILD)/asc
test-as-closure-negctl: $(BUILD)/asc
	python3 tests/unit/as_closure_test.py $(BUILD)/asc --negative-control
test-as-closure: test-as-closure-negctl
	python3 tests/unit/as_closure_test.py $(BUILD)/asc
test-as-process-negctl: $(BUILD)/asc
	python3 tests/unit/as_process_test.py $(BUILD)/asc --negative-control
test-as-process: test-as-process-negctl
	python3 tests/unit/as_process_test.py $(BUILD)/asc
test-as-examples-negctl: $(BUILD)/asc
	python3 tests/unit/as_example_test.py $(BUILD)/asc --negative-control
test-as-examples: test-as-examples-negctl
	python3 tests/unit/as_example_test.py $(BUILD)/asc
test-as-assignment-negctl: $(BUILD)/asc
	python3 tests/unit/as_assignment_test.py $(BUILD)/asc --negative-control
test-as-assignment: test-as-assignment-negctl
	python3 tests/unit/as_assignment_test.py $(BUILD)/asc
test-as-comprehension-negctl: $(BUILD)/asc
	python3 tests/unit/as_comprehension_test.py $(BUILD)/asc --negative-control
test-as-comprehension: test-as-comprehension-negctl
	python3 tests/unit/as_comprehension_test.py $(BUILD)/asc
test-as-range-negctl: $(BUILD)/asc
	python3 tests/unit/as_range_test.py $(BUILD)/asc --negative-control
test-as-range: test-as-range-negctl
	python3 tests/unit/as_range_test.py $(BUILD)/asc
test-as-fstring-negctl: $(BUILD)/asc
	python3 tests/unit/as_fstring_test.py $(BUILD)/asc --negative-control
test-as-fstring: test-as-fstring-negctl
	python3 tests/unit/as_fstring_test.py $(BUILD)/asc
test-as-lexer-lib-negctl: $(BUILD)/asc
	python3 tests/unit/as_lexer_lib_test.py $(BUILD)/asc --negative-control
test-as-lexer-lib: test-as-lexer-lib-negctl
	python3 tests/unit/as_lexer_lib_test.py $(BUILD)/asc
test-as-class-negctl: $(BUILD)/asc
	python3 tests/unit/as_class_test.py $(BUILD)/asc --negative-control
test-as-class: test-as-class-negctl
	python3 tests/unit/as_class_test.py $(BUILD)/asc
test-as-optional-negctl: $(BUILD)/asc
	python3 tests/unit/as_optional_test.py $(BUILD)/asc --negative-control
test-as-optional: test-as-optional-negctl
	python3 tests/unit/as_optional_test.py $(BUILD)/asc
test-as-format-negctl: $(BUILD)/asc
	python3 tests/unit/as_format_test.py $(BUILD)/asc --negative-control
test-as-format: test-as-format-negctl
	python3 tests/unit/as_format_test.py $(BUILD)/asc
test-as-collections-negctl: $(BUILD)/asc
	python3 tests/unit/as_collection_lib_test.py $(BUILD)/asc --negative-control
test-as-collections: test-as-collections-negctl
	python3 tests/unit/as_collection_lib_test.py $(BUILD)/asc
test-as-stats-negctl: $(BUILD)/asc
	python3 tests/unit/as_stats_test.py $(BUILD)/asc --negative-control
test-as-stats: test-as-stats-negctl
	python3 tests/unit/as_stats_test.py $(BUILD)/asc
test-as-callable-negctl: $(BUILD)/asc
	python3 tests/unit/as_callable_test.py $(BUILD)/asc --negative-control
test-as-callable: test-as-callable-negctl
	python3 tests/unit/as_callable_test.py $(BUILD)/asc
test-as-dict-negctl: $(BUILD)/asc
	python3 tests/unit/as_dict_test.py $(BUILD)/asc --negative-control
test-as-dict: test-as-dict-negctl
	python3 tests/unit/as_dict_test.py $(BUILD)/asc
test-as-conversions-negctl: $(BUILD)/asc
	python3 tests/unit/as_conversion_test.py $(BUILD)/asc --negative-control
test-as-conversions: test-as-conversions-negctl
	python3 tests/unit/as_conversion_test.py $(BUILD)/asc
test-as-any-negctl: $(BUILD)/asc
	python3 tests/unit/as_any_test.py $(BUILD)/asc --negative-control
test-as-any: test-as-any-negctl
	python3 tests/unit/as_any_test.py $(BUILD)/asc
test-as-globals-negctl: $(BUILD)/asc
	python3 tests/unit/as_globals_test.py $(BUILD)/asc --negative-control
test-as-globals: test-as-globals-negctl
	python3 tests/unit/as_globals_test.py $(BUILD)/asc
# AS_NATIVE_FRONTEND shares the compiler directory inventory in sources.mk.
$(BUILD)/as-native-link-test: tests/unit/as_native_link_test.c $(AS_NATIVE_FRONTEND) $(AS_HDRS) $(AS_NATIVE_MANIFEST) c/apps/as/sources.mk
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Ic/apps/as $< $(AS_NATIVE_FRONTEND) -o $@
test-as-native-link: $(BUILD)/as-native-link-test
	$< run tests/fixtures/astyped/managed/main.as --stdlib fsroot/as/lib --toolchain c/apps/as/runtime
# This is the final hard gate, not a claim that today's migration is complete.
# Keep runtime/guest evidence ahead of the inventory; names alone prove nothing.
test-as-a3-only: test-as-typed-guest
	python3 tests/unit/as_a3_cutover_test.py $(BUILD)/asc --report $(BUILD)/a3-cutover.json
test-as-managed-negctl: $(BUILD)/asc
	python3 tests/unit/as_managed_test.py $(BUILD)/asc --negative-control
test-as-managed: test-as-managed-negctl
	python3 tests/unit/as_managed_test.py $(BUILD)/asc
test-as-native-parity-negctl: $(BUILD)/asc
	python3 tests/unit/as_native_parity_test.py $(BUILD)/asc --negative-control
test-as-native-parity: test-as-native-parity-negctl
	python3 tests/unit/as_native_parity_test.py $(BUILD)/asc
AS_VIEWER_GUEST_OUT ?= $(BUILD)/viewer-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-viewer-negctl test-as-viewer-guest
test-as-viewer-negctl:
	python3 tests/unit/as_viewer_test.py --negative-control
test-as-typed: test-as-viewer-negctl
test-as-viewer-guest: test-as-viewer-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_VIEWER_GUEST_OUT) --case example-asview --case optional
AS_CLOCK_GUEST_OUT ?= $(BUILD)/clock-guest-$(shell date +%Y%m%d-%H%M%S)
AS_INPUT_GUEST_OUT ?= $(BUILD)/input-guest-$(shell date +%Y%m%d-%H%M%S)
AS_CAPCHECK_GUEST_OUT ?= $(BUILD)/capcheck-guest-$(shell date +%Y%m%d-%H%M%S)
AS_BARRIER_GUEST_OUT ?= $(BUILD)/barrier-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-barrier-negctl test-as-barrier-guest
test-as-barrier-negctl: $(BUILD)/asc
	python3 tests/unit/as_barrier_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-barrier-negctl
test-as-barrier-guest: test-as-barrier-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_BARRIER_GUEST_OUT) --case example-barriers
.PHONY: test-as-capcheck-negctl test-as-capcheck-guest
test-as-capcheck-negctl: $(BUILD)/asc
	python3 tests/unit/as_capcheck_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-capcheck-negctl
test-as-capcheck-guest: test-as-capcheck-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_CAPCHECK_GUEST_OUT) --case example-capcheck
.PHONY: test-as-input-negctl test-as-input-guest
test-as-input-negctl: $(BUILD)/asc
	python3 tests/unit/as_input_test.py $(BUILD)/asc --negative-control
test-as-typed test-as-packaged-guest: test-as-input-negctl
# The bounded native interaction gate preserves the separate queue saturation
# gate. Both launch the shipped A3 examples through their native artifacts.
test-as-input-guest: test-as-input-negctl as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_INPUT_GUEST_OUT) --case example-events --case example-evqstat
.PHONY: test-as-clock-oracle test-as-clock-guest
test-as-clock-oracle:
	python3 tests/unit/as_clock_test.py --negative-control
test-as-typed: test-as-clock-oracle
# Controls are part of this invocation, not optional flags: each mode must
# reject a stopped clock and a tick/ms mix-up before the guest gate is green.
test-as-clock-guest: test-as-clock-oracle as-toolchain $(BUILD)/as-typed-capture.aex $(AS_TYPED_GUEST_BASE)/logit.iso $(addprefix $(AS_TYPED_GUEST_BASE)/,login.aex sh.aex echo.aex cat.aex)
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_CLOCK_GUEST_OUT) --case example-monotonic --case clock-dead --case clock-ticks
AS_TYPED_GUEST_OUT ?= $(BUILD)/typed-guest-$(shell date +%Y%m%d-%H%M%S)
test-as-typed-guest: test-as-typed as-toolchain $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-typed.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_TYPED_GUEST_OUT)
AS_NATIVE_CLI_GUEST_OUT ?= $(BUILD)/native-cli-guest-$(shell date +%Y%m%d-%H%M%S)
.PHONY: test-as-native-cli-guest
test-as-native-cli-guest: test-as-native-cli $(BUILD)/as.aex $(BUILD)/as-typed-capture.aex
	python3 tests/boot/run-as-native-cli.py --build $(BUILD) --base $(AS_TYPED_GUEST_BASE) --out $(AS_NATIVE_CLI_GUEST_OUT)
test-as-a3-only: test-as-native-cli-guest
$(BUILD)/as-typed-capture.o: tests/unit/as_typed_guest_capture.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/as-typed-capture.elf: $(BUILD)/as-typed-capture.o $(BUILD)/aether-toolchain/crt0.o $(BUILD)/aether-toolchain/libc.a
	$(LD) -nostdlib -e _start -Ttext=0x50000000 $(BUILD)/aether-toolchain/crt0.o $< $(BUILD)/aether-toolchain/libc.a -o $@
$(BUILD)/as-typed-capture.aex: $(BUILD)/as-typed-capture.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ native-capture --cli
# The failure control is a prerequisite, so an ordinary invocation always
# verifies the oracle before accepting the positive native/compiler results.
test-as-typed-negctl:
	python3 tests/unit/as_typed_negative.py
test-as-generic-negctl:
	python3 tests/unit/as_generic_negative.py
test-as-generic: test-as-generic-negctl $(BUILD)/asc
	python3 tests/unit/as_generic_test.py $(BUILD)/asc
test-as-typed: test-as-typed-negctl test-as-generic test-as-native-parity test-as-managed test-as-native-link test-as-globals test-as-any test-as-conversions test-as-dict test-as-callable test-as-stats test-as-collections test-as-format test-as-optional test-as-class test-as-lexer-lib test-as-fstring test-as-range test-as-comprehension test-as-assignment test-as-examples test-as-process test-as-closure test-as-inheritance test-as-buffer test-as-bytes test-as-files test-as-binary test-as-constants test-as-capability test-as-memory $(BUILD)/asc
	python3 tests/unit/as_typed_test.py $(BUILD)/asc
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
$(BUILD)/aether-toolchain/obj/%.o: c/apps/as/runtime/%.c $(AS_NATIVE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/aether-toolchain/crt0.o: c/apps/crt0_cli.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(BUILD)/aether-toolchain/libc.a: $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AGENT_AR) rcs $@ $(LIBC_OBJS)
