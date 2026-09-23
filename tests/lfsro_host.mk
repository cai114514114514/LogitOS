# The read-only second-disk reader handles signed VFS sizes and scans each
# directory block once. Both controls link the same implementation and test:
# one restores the uint32-to-int cast, the other restores per-entry device reads.
LRO_SRC := tests/unit/lfsro_host_test.c c/fs/logitfs/lfsro.c
LRO_DEP := $(LRO_SRC) c/fs/logitfs/lfsro.h c/fs/logitfs/logitfs_fmt.h \
           tests/unit/lfsro_stub/blkdev.h tests/lfsro_host.mk
LRO_INC := -Itests/unit/lfsro_stub -Itests/unit/fsstub $(FS_INC)
LRO_FLAGS := -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer

$(BUILD)/lfsro_host_test: $(LRO_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(LRO_FLAGS) $(LRO_INC) -o $@ $(LRO_SRC)

$(BUILD)/lfsro_size_control: $(LRO_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(LRO_FLAGS) -DLRO_SIZE_NEGCTL $(LRO_INC) -o $@ $(LRO_SRC)

$(BUILD)/lfsro_scan_control: $(LRO_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(LRO_FLAGS) -DLRO_SCAN_NEGCTL $(LRO_INC) -o $@ $(LRO_SRC)

.PHONY: test-lfsro-host test-lfsro-size-negctl test-lfsro-scan-negctl
test-lfsro-size-negctl: $(BUILD)/lfsro_size_control
	@rc=0; $< > $(BUILD)/lfsro-size-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg -F 'FAIL: out-of-range file size is refused instead of wrapping negative' $(BUILD)/lfsro-size-control.log
	@rg -F 'FAIL: out-of-range directory entry size follows LogitFS zero convention' $(BUILD)/lfsro-size-control.log

test-lfsro-scan-negctl: $(BUILD)/lfsro_scan_control
	@rc=0; $< > $(BUILD)/lfsro-scan-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg -F 'FAIL: count reads the inode and shared directory block once each' $(BUILD)/lfsro-scan-control.log
	@rg -F 'FAIL: nth entry reads the inode and directory block once each' $(BUILD)/lfsro-scan-control.log
	@rg -F 'directory reads: count 33, last entry 33' $(BUILD)/lfsro-scan-control.log

test-lfsro-host: test-lfsro-size-negctl test-lfsro-scan-negctl $(BUILD)/lfsro_host_test
	@$(BUILD)/lfsro_host_test

test-fs-host ci-host: test-lfsro-host
