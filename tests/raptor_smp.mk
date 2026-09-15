# Core-i7-14700KF-shaped SMP topology: 8P/12E/28T host model plus an
# optional QEMU sparse-ID execution build. QEMU cannot emulate Intel hybrid
# P/E CPUID leaves, so the guest gate covers MADT/AP startup/timer/IPI only.
ifeq ($(RAPTORSMPVERIFY),1)
CFLAGS += -DLOGIT_RAPTOR_SMP_QEMU_TEST
endif

.PHONY: test-raptor-smp-negctl test-raptor-smp-host test-raptor-smp-synthetic-guest

test-raptor-smp-negctl:
	@python3 tests/unit/raptor_smp_run.py --build $(BUILD)/raptor-smp/negative --negative-only

test-raptor-smp-host: test-raptor-smp-negctl
	@python3 tests/unit/raptor_smp_run.py --build $(BUILD)/raptor-smp/positive

test-raptor-smp-synthetic-guest: test-raptor-smp-host $(ISO) $(BUILD)/esp.img $(DISK)
	@test "$(X2APICVERIFY)" = 1 && test "$(RAPTORSMPVERIFY)" = 1 || { \
	    echo 'Use X2APICVERIFY=1 RAPTORSMPVERIFY=1 with an independent BUILD'; \
	    exit 1; }
	@python3 tests/boot/run-raptor-smp.py --iso $(ISO) --esp $(BUILD)/esp.img \
	    --disk $(DISK) --out $(BUILD)/raptor-smp/guest

ci-host: test-raptor-smp-host
