# Intel C600/X79 D31:F3 SMBus and read-only DDR3 SPD discovery.  QEMU has no
# 8086:1d22 controller model, so this gate is explicitly a production-source
# host model plus kernel link; physical operation needs a real serial log.
.PHONY: test-x79-chipset-host test-x79-chipset-negctl
ci-host: test-x79-chipset-host
test-x79-chipset-host: test-x79-chipset-negctl
	@python3 tests/unit/x79_chipset_run.py --build $(BUILD)/x79-chipset/positive

test-x79-chipset-negctl:
	@python3 tests/unit/x79_chipset_run.py --build $(BUILD)/x79-chipset/negative --negative-only
