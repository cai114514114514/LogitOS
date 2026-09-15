# SPDX-License-Identifier: MIT
# Main disk packaging owns /browser as persistent guest state. New user-state
# roots require an explicit preserve rule; this is not a general OS updater.
$(BUILD)/lfs_snapshot: tools/lfs_snapshot.c c/fs/fsck.c c/fs/fsck.h c/fs/logitfs_fmt.h c/drivers/block/crc32.c c/drivers/block/crc32.h tests/unit/fsstub/kheap.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -Itests/unit/fsstub -Ic/fs -Ic/drivers/block -o $@ tools/lfs_snapshot.c c/fs/fsck.c c/drivers/block/crc32.c

$(DISK): $(BUILD)/lfs_snapshot tools/disk_guard.py tools/disk_profile.py

.PHONY: test-disk-profile test-disk-profile-negctl
test-disk-profile-negctl: $(BUILD)/lfs_snapshot
	@rc=0; python3 tests/unit/disk_profile_test.py --helper $(BUILD)/lfs_snapshot --negative > $(BUILD)/disk-profile-negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/disk-profile-negctl.log; test $$rc -eq 1 && grep -q '^FAIL: rebuilding without preservation loses the existing browser profile' $(BUILD)/disk-profile-negctl.log
test-disk-profile: test-disk-profile-negctl
	@python3 tests/unit/disk_profile_test.py --helper $(BUILD)/lfs_snapshot
ci-host: test-disk-profile

# Full browser producer/consumer acceptance, with a real system repack between
# boots. OUT defaults to a new path because overwriting old evidence is refused.
DISK_PROFILE_GUEST_OUT ?= $(BUILD)/disk-profile-guest-$$(date +%Y%m%d-%H%M%S)
.PHONY: test-disk-profile-os
test-disk-profile-os: test-disk-profile $(ISO) $(DISK)
	@python3 tests/qmp/cookie_persistence_guest.py --iso $(ISO) --disk $(DISK) \
	    --snapshot-helper $(BUILD)/lfs_snapshot --rebuild-between --out $(DISK_PROFILE_GUEST_OUT)
