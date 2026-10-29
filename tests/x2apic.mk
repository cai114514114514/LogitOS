# Firmware-active x2APIC and sparse MADT type-9 acceptance. Ordinary images
# preserve IA32_APIC_BASE exactly as firmware handed it off. X2APICVERIFY is a
# QEMU-only switch used to execute the MSR backend with a synthetic wide ID.
ifeq ($(X2APICVERIFY),1)
CFLAGS += -DLOGIT_X2APIC_QEMU_TEST
endif

.PHONY: test-x2apic-negctl test-x2apic-host test-x2apic-synthetic-guest

test-x2apic-negctl:
	@python3 tests/unit/x2apic_run.py --build $(BUILD)/x2apic/negative --negative-only
	@python3 tests/unit/pci_intx_run.py --build $(BUILD)/x2apic/ioapic-route-negative --x2apic-only
	@python3 tests/unit/bkl_pci_run.py --build $(BUILD)/x2apic/ioapic-init --x2apic-only
	@python3 tests/unit/irq_unbind_run.py --build $(BUILD)/x2apic/irq-unbind-negative --negative-only
	@python3 tests/unit/pci_msi_safety_run.py --build $(BUILD)/x2apic/msi-safety-negative --negative-only

test-x2apic-host: test-x2apic-negctl
	@python3 tests/unit/x2apic_run.py --build $(BUILD)/x2apic/positive
	@python3 tests/unit/pci_intx_run.py --build $(BUILD)/x2apic/ioapic-route-positive
	@python3 tests/unit/irq_unbind_run.py --build $(BUILD)/x2apic/irq-unbind-positive
	@python3 tests/unit/pci_msi_safety_run.py --build $(BUILD)/x2apic/msi-safety-positive

# Object rules do not track CFLAGS. Always use an isolated BUILD and the
# explicit test-only switch for this synthetic transition.
test-x2apic-synthetic-guest: test-x2apic-host $(ISO) $(BUILD)/esp.img $(DISK)
	@test "$(X2APICVERIFY)" = 1 || { \
	    echo 'Use X2APICVERIFY=1 with an independent BUILD; ordinary kernels never force x2APIC'; \
	    exit 1; }
	@python3 tests/boot/run-x2apic.py --iso $(ISO) --esp $(BUILD)/esp.img \
	    --disk $(DISK) --out $(BUILD)/x2apic/guest

ci-host: test-x2apic-host
