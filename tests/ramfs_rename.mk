# Reproduce the shipped root-only rename against the same public VFS suite.
# Require the content and atomic-refusal failures, not merely a nonzero exit.
RAMFS_RENAME_DEPS := tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) \
                    $(wildcard c/fs/vfs/*.h c/fs/ramfs/*.h) \
                    c/drivers/core/io_lock.h tests/ramfs_rename.mk

$(BUILD)/ramfs_rename_control: $(RAMFS_RENAME_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DRAMFS_RENAME_ROOT_ONLY -o $@ \
	    tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) $(FS_INC) $(KCORE_INC)

test-ramfs-rename-negctl: $(BUILD)/ramfs_rename_control
	@rc=0; $< > $(BUILD)/ramfs-rename-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg -F 'FAIL: renamed nested file preserves content' $(BUILD)/ramfs-rename-control.log
	@rg -F 'FAIL: cannot move a directory into its descendant' $(BUILD)/ramfs-rename-control.log
	@rg -F 'FAIL: overlong descendant aborts the entire rename' $(BUILD)/ramfs-rename-control.log

test-vfs-mount test-vfs-mount-asan: test-ramfs-rename-negctl
.PHONY: test-ramfs-rename-negctl
