OPENLOGIT_BASELINE_ARCHIVE ?= $(BUILD)/baseline/libopenlogit.a
$(BUILD)/bench/openlogit_2d.o: tests/bench/openlogit_2d.c $(OPENLOGIT_PUBLIC) examples/openlogit/example_log.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/bench/2d-before.elf: $(BUILD)/bench/openlogit_2d.o $(OPENLOGIT_BASELINE_ARCHIVE) $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x69000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(OPENLOGIT_BASELINE_ARCHIVE) $(LIBC_OBJS) $(LIBM_OBJ) --end-group
$(BUILD)/bench/2d-after.elf: $(BUILD)/bench/openlogit_2d.o $(BUILD)/sdk/libopenlogit.a $(SYSROOT_WORK)/crt1.o $(LIBC_OBJS) $(LIBM_OBJ)
	$(LD) -nostdlib -e _start -Ttext=0x69000000 -o $@ $(SYSROOT_WORK)/crt1.o $< --start-group $(BUILD)/sdk/libopenlogit.a $(LIBC_OBJS) $(LIBM_OBJ) --end-group
openlogit-2d-perf-binaries: $(BUILD)/bench/2d-before.elf $(BUILD)/bench/2d-after.elf
.PHONY: openlogit-2d-perf-binaries
