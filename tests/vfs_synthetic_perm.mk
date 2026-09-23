# The real VFS permission path must refuse a synthetic command before the
# provider sees it. The control restores the former dispatch-first order and
# requires the specific side-effect assertion to go red.
VFS_SYN_PERM_DEPS := tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) \
                     $(wildcard c/fs/vfs/*.h c/fs/ramfs/*.h) \
                     tests/vfs_synthetic_perm.mk

$(BUILD)/vfs_synthetic_control: $(VFS_SYN_PERM_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DVFS_SYN_WRITE_EARLY_NEGCTL -o $@ \
	    tests/unit/vfs_mount_test.c $(VFS_TEST_SRC) $(FS_INC) $(KCORE_INC)

.PHONY: test-vfs-synthetic-negctl
test-vfs-synthetic-negctl: $(BUILD)/vfs_synthetic_control
	@rc=0; $< > $(BUILD)/vfs-synthetic-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg -F 'FAIL: a refused synthetic command never reaches its provider' $(BUILD)/vfs-synthetic-control.log
	@rg -F 'FAIL: the permitted command reaches its provider exactly once' $(BUILD)/vfs-synthetic-control.log

test-vfs-mount test-vfs-mount-asan: test-vfs-synthetic-negctl
ci-host: test-vfs-mount
