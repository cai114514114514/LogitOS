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
# Each group's own argument string, and the list of groups.
#
# WHY THEY ARE NOT PASSED ON THE COMMAND LINE. All groups together are one
# `sh -c` argument, and Linux caps a single argument at 128 KiB (MAX_ARG_STRLEN):
# with the css group the total reached 174,702 bytes. execve then fails with E2BIG,
# the shell never starts, `|| echo BUILD_CONFIG_FAILED` never runs, and the check
# silently reported "nothing changed" -- editing a compiler flag stopped forcing a
# rebuild, and build-config-update could not run at all. macOS allows ~1 MiB, so it
# never showed on the development host. Instead make appends each group's string
# (the largest is ~27 KB) to a scratch file and the helper reads it with argparse's
# @file syntax, so it is still ONE process and the command line is a few dozen
# bytes. $(file ...) would be simpler but needs GNU Make 4; this file supports 3.81.
BUILD_CONFIG_GROUPS :=
# One file per make process, so a nested $(MAKE) cannot truncate its parent's.
BUILD_CONFIG_ARGFILE = $(BUILD)/.config/args.$(shell echo $$PPID)
BUILD_CONFIG_HELPER := $(dir $(lastword $(MAKEFILE_LIST)))config.py

.PHONY: build-config-update build-config-force
build-config-force: build-config-update
define build_config_append_group
	@printf '%s\n' $(BUILD_CONFIG_ARGS_$(1)) >> $(call build_quote,$(BUILD_CONFIG_ARGFILE))

endef
build-config-update:
	@mkdir -p $(call build_quote,$(BUILD)/.config) && : > $(call build_quote,$(BUILD_CONFIG_ARGFILE))
	$(foreach group,$(BUILD_CONFIG_GROUPS),$(call build_config_append_group,$(group)))
	@python3 $(call build_quote,$(BUILD_CONFIG_HELPER)) --directory=$(call build_quote,$(BUILD)/.config) $(call build_quote,@$(BUILD_CONFIG_ARGFILE)); rc=$$?; rm -f $(call build_quote,$(BUILD_CONFIG_ARGFILE)); exit $$rc

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
BUILD_CONFIG_GROUPS += $(1)
BUILD_CONFIG_ARGS_$(1) := --group=$(call build_quote,$(1)) --tool=$$(call build_quote,$$($(2))) --value=$$(call build_quote,$$(foreach name,$(3),$$($$(name)))) --outputs=$$(call build_quote,$(4))
$(call config_stamp,$(1)):
	@:
endef

define build_config_finish
BUILD_CONFIG_PREP := $$(shell mkdir -p $$(call build_quote,$$(BUILD)/.config) && : > $$(call build_quote,$$(BUILD_CONFIG_ARGFILE)))
BUILD_CONFIG_FILL := $$(foreach group,$$(BUILD_CONFIG_GROUPS),$$(shell printf '%s\n' $$(BUILD_CONFIG_ARGS_$$(group)) >> $$(call build_quote,$$(BUILD_CONFIG_ARGFILE))))
BUILD_CONFIG_CHANGED := $$(shell python3 $$(call build_quote,$$(BUILD_CONFIG_HELPER)) --check --directory=$$(call build_quote,$$(BUILD)/.config) $$(call build_quote,@$$(BUILD_CONFIG_ARGFILE)) || echo BUILD_CONFIG_FAILED)
BUILD_CONFIG_DROP := $$(shell rm -f $$(call build_quote,$$(BUILD_CONFIG_ARGFILE)))
$$(if $$(filter BUILD_CONFIG_FAILED,$$(BUILD_CONFIG_CHANGED)),$$(error Could not inspect build configuration))
$$(foreach group,$$(patsubst group=%,%,$$(filter group=%,$$(BUILD_CONFIG_CHANGED))),$$(eval $$(call config_stamp,$$(group)): build-config-update))
$$(foreach output,$$(patsubst output=%,%,$$(filter output=%,$$(BUILD_CONFIG_CHANGED))),$$(eval $$(output): build-config-force))
endef
