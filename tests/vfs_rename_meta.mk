VFS_RENAME_META_SRC := tests/unit/vfs_rename_meta_test.c $(VFS_TEST_SRC)
VFS_RENAME_META_DEPS := $(VFS_RENAME_META_SRC) $(wildcard c/fs/vfs/*.h c/fs/ramfs/*.h) \
                        c/drivers/core/io_domain.h c/drivers/core/io_lock.h tests/vfs_rename_meta.mk
VFS_RENAME_META_FLAGS := -O1 -g -Wall -Wextra $(FS_INC) $(KCORE_INC)

$(BUILD)/vfs_rename_meta_test: $(VFS_RENAME_META_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(VFS_RENAME_META_FLAGS) -o $@ $(VFS_RENAME_META_SRC)

$(BUILD)/vfs_rename_meta_control: $(VFS_RENAME_META_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(VFS_RENAME_META_FLAGS) -DVFS_RENAME_META_ROOT_ONLY -o $@ $(VFS_RENAME_META_SRC)

$(BUILD)/vfs_rename_meta_asan: $(VFS_RENAME_META_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(VFS_RENAME_META_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ $(VFS_RENAME_META_SRC)

.PHONY: test-vfs-rename-meta test-vfs-rename-meta-negctl test-vfs-rename-meta-asan

# Require both the lost permissions and the post-rename length failure. A crash
# or fixture failure must not impersonate the root-only metadata mutation.
test-vfs-rename-meta-negctl: $(BUILD)/vfs_rename_meta_control
	@rc=0; $< > $(BUILD)/vfs-rename-meta-control.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg -F 'FAIL: descendant file metadata follows rename' $(BUILD)/vfs-rename-meta-control.log
	@rg -F 'FAIL: metadata path overflow is refused before backend rename' $(BUILD)/vfs-rename-meta-control.log

test-vfs-rename-meta: test-vfs-rename-meta-negctl $(BUILD)/vfs_rename_meta_test
	@$(BUILD)/vfs_rename_meta_test

test-vfs-rename-meta-asan: test-vfs-rename-meta-negctl $(BUILD)/vfs_rename_meta_asan
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/vfs_rename_meta_asan

# The runner owns the disk manifest; derive its prerequisites so adding a
# program cannot silently pack an older .aex. Each run retains its own disk.
VFS_RENAME_META_PROGRAMS := $(shell python3 -c 'import runpy; print(*runpy.run_path("tests/boot/run-vfs-rename-meta.py")["PROGRAMS"])')
VFS_RENAME_META_AEX := $(addprefix $(BUILD)/,$(addsuffix .aex,$(VFS_RENAME_META_PROGRAMS)))

ci-host: test-vfs-rename-meta test-vfs-rename-meta-asan
