# SPDX-License-Identifier: MIT
ifneq ($(PTINTERP_CONTROL),)
ifeq ($(PTINTERP_CONTROL),fork-fp)
$(BUILD)/c/boot/enter_user.o: c/boot/enter_user.asm tests/unit/ptinterp_variant.py
	@mkdir -p $(dir $@)
	python3 tests/unit/ptinterp_variant.py --out $(BUILD)/interp-control --control $(PTINTERP_CONTROL)
	$(ASM) $(ASFLAGS) $(BUILD)/interp-control/c/boot/enter_user.asm -o $@
else
ifeq ($(PTINTERP_CONTROL),stack)
PTINTERP_CONTROL_FILE := c/kernel/exec/exec
else
PTINTERP_CONTROL_FILE := c/kernel/exec/elf
endif
$(BUILD)/$(PTINTERP_CONTROL_FILE).o: $(PTINTERP_CONTROL_FILE).c tests/unit/ptinterp_variant.py
	@mkdir -p $(dir $@)
	python3 tests/unit/ptinterp_variant.py --out $(BUILD)/interp-control --control $(PTINTERP_CONTROL)
	$(CC) $(CFLAGS) -c $(BUILD)/interp-control/$(PTINTERP_CONTROL_FILE).c -o $@
endif
endif
.PHONY: test-ptinterp-host test-ptinterp-negctl ptinterp-fixtures
# The runner executes actual-source controls first, then the identical positive
# assertions. A crash cannot stand in for a named semantic control failure.
test-ptinterp-host: test-ptinterp-negctl
test-ptinterp-negctl:
	python3 tests/unit/ptinterp_run.py --build $(BUILD)/ptinterp
ptinterp-fixtures:
	python3 tests/unit/ptinterp_run.py --build $(BUILD)/ptinterp --fixtures-only

.PHONY: ptinterp-image ptinterp-disk test-ptinterp-os test-ptinterp-all
ptinterp-disk: test-kernel-disk-recipe ptinterp-fixtures
	$(MAKE) -n -W tools/mkfs.py BUILD=$(WIDE_BASE_BUILD) $(WIDE_BASE_BUILD)/disk.img > $(BUILD)/ptinterp-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/ptinterp-disk.make $(BUILD)/ptinterp-disk.img \
	  $(BUILD)/ptinterp/driver.elf:/bin/ptinterp-check \
	  $(BUILD)/ptinterp/program.elf:/bin/ptinterp $(BUILD)/ptinterp/program.aex:/bin/ptinterp-aex \
	  $(BUILD)/ptinterp/fixed.elf:/bin/ptinterp-fixed $(BUILD)/ptinterp/gui.aex:/apps/ptinterp.aex \
	  $(BUILD)/ptinterp/tiny.aex:/apps/ptinterp-tiny.aex \
	  $(BUILD)/ptinterp/ld-logit-test.so:/lib/ld-logit-test.so \
	  $(BUILD)/ptinterp/ld-logit-test.so:/lib/ld-logit-deny.so \
	  $(BUILD)/ptinterp/program.elf:/lib/ld-logit-loop.so $(BUILD)/ptinterp/junk.so:/lib/ld-logit-junk.so \
	  $(BUILD)/ptinterp/missing.elf:/bin/interp-missing $(BUILD)/ptinterp/denied.elf:/bin/interp-denied \
	  $(BUILD)/ptinterp/nested.elf:/bin/interp-nested $(BUILD)/ptinterp/junk.elf:/bin/interp-junk
ptinterp-image: $(ISO) $(BUILD)/esp.img ptinterp-disk
test-ptinterp-os: test-ptinterp-host
	$(MAKE) BUILD=$(BUILD) WIDE_BASE_BUILD=$(WIDE_BASE_BUILD) ptinterp-image
	python3 tests/boot/run-ptinterp.py --build $(BUILD) --disk $(BUILD)/ptinterp-disk.img --out $(BUILD)/ptinterp-results
test-ptinterp-all:
	python3 tests/boot/run-ptinterp-acceptance.py --build $(BUILD) $(if $(PTINTERP_APPS_DISK),--apps-disk $(PTINTERP_APPS_DISK)) --apps $(WIDE_BASE_BUILD)
