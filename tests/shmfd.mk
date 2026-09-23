# Process-local SHM handles. Host gate calls the production table helper with
# mock reference counts: a guessed global slot must neither map nor close.
SHMFD_SRC := tests/unit/shmfd_test.c c/kernel/exec/shmfd.c
SHMFD_INC := $(KEXEC_INC) $(KCPU_INC) $(KMM_INC) -Iinclude/abi

.PHONY: test-shmfd test-shmfd-negctl
test-shmfd-negctl:
	@mkdir -p $(BUILD)/shmfd
	@$(CC) -std=c11 -O2 -DSHMFD_NEGCTL_GLOBAL $(SHMFD_INC) -o $(BUILD)/shmfd/old $(SHMFD_SRC)
	@rc=0; $(BUILD)/shmfd/old > $(BUILD)/shmfd/old.log 2>&1 || rc=$$?; \
	  cat $(BUILD)/shmfd/old.log; \
	  test $$rc -eq 1 && \
	  rg -q '^FAIL foreign process cannot map guessed segment slot$$' $(BUILD)/shmfd/old.log && \
	  rg -q '^FAIL foreign process cannot close guessed segment slot$$' $(BUILD)/shmfd/old.log

test-shmfd: test-shmfd-negctl
	@$(CC) -std=c11 -O2 $(SHMFD_INC) -o $(BUILD)/shmfd/current $(SHMFD_SRC)
	@$(BUILD)/shmfd/current

ci-host: test-shmfd
