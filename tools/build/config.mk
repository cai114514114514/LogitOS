# SPDX-License-Identifier: MIT
# Register after the product flags have reached their final values:
#   include tools/build/config.mk
#   $(eval $(call build_config,kernel,CC,CFLAGS,$(KERNEL_C_OBJ)))
#   $(KERNEL_C_OBJ): $(call config_stamp,kernel)
#   $(eval $(call build_config_finish))
# Arguments 2 and 3 are VARIABLE NAMES, not their values. This keeps eval's
# second expansion from interpreting a dollar or # embedded in compiler flags.
# Register a separate group for a target-specific flag variant: snapshotting a
# shared variable from the first target to request the check would depend on -j.

build_quote = '$(subst ','"'"',$(1))'
config_stamp = $(BUILD)/.config/$(1).json
BUILD_CONFIG_ARGS :=
BUILD_CONFIG_HELPER := $(dir $(lastword $(MAKEFILE_LIST)))config.py

.PHONY: build-config-update build-config-force
build-config-force: build-config-update
build-config-update:
	@python3 $(call build_quote,$(BUILD_CONFIG_HELPER)) --directory=$(call build_quote,$(BUILD)/.config) $(BUILD_CONFIG_ARGS)

# A single read-only process forces only outdated outputs for this invocation.
# A dry run therefore reports the real rebuild without consuming the change.
# GNU Make 3.81 compares whole seconds on macOS: changed stamp contents alone
# missed a compiler replacement in the same second in test-build-config. The
# helper also compares OUTPUT nanoseconds, so objects not rebuilt before an
# interruption (or not requested at all) stay dirty on the next invocation.
# Force only those objects: one unrequested object must not make its already
# rebuilt siblings compile on every invocation until the whole group is built.
# Its output list uses Make's ordinary whitespace-separated filename contract.
define build_config
BUILD_CONFIG_ARGS += --group=$(call build_quote,$(1)) --tool=$$(call build_quote,$$($(2))) --value=$$(call build_quote,$$(foreach name,$(3),$$($$(name)))) --outputs=$$(call build_quote,$(4))
$(call config_stamp,$(1)):
	@:
endef

define build_config_finish
BUILD_CONFIG_CHANGED := $$(shell python3 $$(call build_quote,$$(BUILD_CONFIG_HELPER)) --check --directory=$$(call build_quote,$$(BUILD)/.config) $$(BUILD_CONFIG_ARGS) || echo BUILD_CONFIG_FAILED)
$$(if $$(filter BUILD_CONFIG_FAILED,$$(BUILD_CONFIG_CHANGED)),$$(error Could not inspect build configuration))
$$(foreach group,$$(patsubst group=%,%,$$(filter group=%,$$(BUILD_CONFIG_CHANGED))),$$(eval $$(call config_stamp,$$(group)): build-config-update))
$$(foreach output,$$(patsubst output=%,%,$$(filter output=%,$$(BUILD_CONFIG_CHANGED))),$$(eval $$(output): build-config-force))
endef
