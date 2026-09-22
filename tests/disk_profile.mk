# SPDX-License-Identifier: MIT
# Main disk packaging owns /browser as persistent guest state. New user-state
# roots require an explicit preserve rule; this is not a general OS updater.
$(BUILD)/lfs_snapshot: tools/lfs_snapshot.c c/fs/logitfs/fsck.c c/fs/logitfs/fsck.h c/fs/logitfs/logitfs_fmt.h c/drivers/block/crc32.c c/drivers/block/crc32.h tests/unit/fsstub/kheap.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -Itests/unit/fsstub $(FS_INC) -Ic/drivers/block -o $@ tools/lfs_snapshot.c c/fs/logitfs/fsck.c c/drivers/block/crc32.c

$(DISK): $(BUILD)/lfs_snapshot tools/disk_guard.py tools/disk_profile.py Makefile

.PHONY: test-disk-profile test-disk-profile-negctl
test-disk-profile-negctl: $(BUILD)/lfs_snapshot
	@rc=0; python3 tests/unit/disk_profile_test.py --helper $(BUILD)/lfs_snapshot --negative > $(BUILD)/disk-profile-negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/disk-profile-negctl.log; test $$rc -eq 1 && grep -q '^FAIL: rebuilding without preservation loses the existing browser profile' $(BUILD)/disk-profile-negctl.log
test-disk-profile: test-disk-profile-negctl
	@python3 tests/unit/disk_profile_test.py --helper $(BUILD)/lfs_snapshot
ci-host: test-disk-profile

# The dense control exercises the same journal and byte oracles, then must fail
# only because it allocated the whole image. This catches a sparse gate whose
# source-data checks pass while the write path silently regresses to full I/O.
$(BUILD)/lfs_snapshot_dense: tools/lfs_snapshot.c c/fs/logitfs/fsck.c c/fs/logitfs/fsck.h c/fs/logitfs/logitfs_fmt.h c/drivers/block/crc32.c c/drivers/block/crc32.h tests/unit/fsstub/kheap.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DLFS_SNAPSHOT_DENSE -Itests/unit/fsstub $(FS_INC) -Ic/drivers/block -o $@ tools/lfs_snapshot.c c/fs/logitfs/fsck.c c/drivers/block/crc32.c

.PHONY: test-snapshot-sparse test-snapshot-sparse-negctl
test-snapshot-sparse-negctl: $(BUILD)/lfs_snapshot_dense
	@rc=0; python3 tests/unit/snapshot_sparse_test.py --helper $(BUILD)/lfs_snapshot_dense > $(BUILD)/snapshot-sparse-negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/snapshot-sparse-negctl.log; test $$rc -eq 1 && grep -q '^FAIL: snapshot remains dense:' $(BUILD)/snapshot-sparse-negctl.log
test-snapshot-sparse: test-snapshot-sparse-negctl $(BUILD)/lfs_snapshot
	@python3 tests/unit/snapshot_sparse_test.py --helper $(BUILD)/lfs_snapshot --dense-helper $(BUILD)/lfs_snapshot_dense
ci-host: test-snapshot-sparse

# The dense writer is the control: it produces the same logical image but
# consumes the zero tail on the host. Keep it a prerequisite so the physical
# allocation claim cannot turn into a test that only reads image contents.
.PHONY: test-disk-sparse test-disk-sparse-negctl
test-disk-sparse-negctl:
	@mkdir -p $(BUILD)
	@rc=0; python3 tests/unit/disk_sparse_test.py --dense-control > $(BUILD)/disk-sparse-negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/disk-sparse-negctl.log; test $$rc -eq 1 && grep -q '^FAIL: sparse disk still allocated the zero tail' $(BUILD)/disk-sparse-negctl.log
test-disk-sparse: test-disk-sparse-negctl
	@python3 tests/unit/disk_sparse_test.py
ci-host: test-disk-sparse

# Full browser producer/consumer acceptance, with a real system repack between
# boots. OUT defaults to a new path because overwriting old evidence is refused.
DISK_PROFILE_GUEST_OUT ?= $(BUILD)/disk-profile-guest-$$(date +%Y%m%d-%H%M%S)
.PHONY: test-disk-profile-os
test-disk-profile-os: test-disk-profile $(ISO) $(DISK)
	@python3 tests/qmp/cookie_persistence_guest.py --iso $(ISO) --disk $(DISK) \
	    --snapshot-helper $(BUILD)/lfs_snapshot --rebuild-between --out $(DISK_PROFILE_GUEST_OUT)

.PHONY: test-studio-persistence test-studio-persistence-negctl
test-studio-persistence-negctl: $(BUILD)/lfs_snapshot
	@rc=0; python3 tests/unit/studio_persistence_test.py --helper $(BUILD)/lfs_snapshot --negative > $(BUILD)/studio-persistence-negctl.log 2>&1 || rc=$$?; \
	 tail -2 $(BUILD)/studio-persistence-negctl.log; test $$rc -ne 0 && rg -q 'AssertionError: project source survived rebuild' $(BUILD)/studio-persistence-negctl.log
test-studio-persistence: test-studio-persistence-negctl
	@python3 tests/unit/studio_persistence_test.py --helper $(BUILD)/lfs_snapshot
