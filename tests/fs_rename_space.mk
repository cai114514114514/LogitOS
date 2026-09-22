FS_RENAME_SPACE_SRC := tests/unit/fs_rename_space_test.c $(FS_CORE)
FS_RENAME_SPACE_DEPS := $(FS_RENAME_SPACE_SRC) tests/unit/fs_rename/kheap.h \
                        $(wildcard c/fs/logitfs/*.h c/fs/logitfs/*.inc c/fs/cache/*.h tests/unit/fsstub/*.h) \
                        c/drivers/block/crc32.h tests/fs_rename_space.mk

$(BUILD)/fs_rename_space_test: $(FS_RENAME_SPACE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(FS_CFLAGS) -Itests/unit/fs_rename -o $@ $(FS_RENAME_SPACE_SRC) $(FS_STUB)

$(BUILD)/fs_rename_space_control: $(FS_RENAME_SPACE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(FS_CFLAGS) -Itests/unit/fs_rename -DLFS_RENAME_ABORT_NEGCTL \
	    -o $@ $(FS_RENAME_SPACE_SRC) $(FS_STUB)

.PHONY: test-fs-rename-space test-fs-rename-space-negctl test-fs-crash-negctl
test-fs-rename-space-negctl: $(BUILD)/fs_rename_space_control
	@rc=0; ASAN_OPTIONS=detect_leaks=0 $< > $(BUILD)/fs-rename-space-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg 'FAIL: space=1 cross=0 .*refused rename preserves bystander bytes' $(BUILD)/fs-rename-space-control.log
	@rg 'FAIL: space=8 cross=1 .*log=6: later commit has no leaked allocation bits' $(BUILD)/fs-rename-space-control.log

test-fs-rename-space: test-fs-rename-space-negctl $(BUILD)/fs_rename_space_test
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/fs_rename_space_test

# The old crash gate had no control prerequisite. Restore the historical
# immediate-free bug in a scratch copy and require its specific byte oracle.
test-fs-crash-negctl:
	@python3 tests/unit/fs_crash_control.py --build "$(BUILD)" --cc "$(CC)" \
	    --flags "$(FS_CFLAGS) $(FS_STUB)" --sources $(FS_CORE)

test-fs-crash: test-fs-crash-negctl

ci-host: test-fs-rename-space
