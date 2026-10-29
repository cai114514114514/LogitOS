# Ordinary schema edits must update only their active upgrade transaction.
# Reuse the real page/event/timer source set from test-idb: a private JS timer
# imitation would not establish that the database opens and requests settle.
IDB_UPGRADE_DIR = $(BUILD)/idb-upgrade-scope
IDB_UPGRADE_SRC = tests/unit/idb_upgrade_scope_test.c $(filter-out tests/unit/webapi_idb_test.c,$(IDB_TEST_SRC))
IDB_UPGRADE_DEP = $(IDB_UPGRADE_SRC) tests/unit/webapi_idb_test.c tests/unit/idb_upgrade_scope_check.py tests/idb_upgrade_scope.mk $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(IDB_UPGRADE_DIR)/current: $(IDB_UPGRADE_DEP)
	@mkdir -p $(IDB_UPGRADE_DIR)
	@$(CC) -O2 -w $(IDB_CF) -Itests/unit -o $@ $(IDB_UPGRADE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(IDB_UPGRADE_DIR)/old: $(IDB_UPGRADE_DEP)
	@mkdir -p $(IDB_UPGRADE_DIR)
	@$(CC) -O2 -w $(IDB_CF) -Itests/unit -DJS_IDB_STATIC_UPGRADE_SCOPE -o $@ $(IDB_UPGRADE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(IDB_UPGRADE_DIR)/san: $(IDB_UPGRADE_DEP)
	@mkdir -p $(IDB_UPGRADE_DIR)
	@$(CC) -O1 -g -w $(IDB_CF) -Itests/unit -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(IDB_UPGRADE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-idb-upgrade-scope test-idb-upgrade-scope-negctl test-idb-upgrade-scope-san
test-idb-upgrade-scope-negctl: $(IDB_UPGRADE_DIR)/old
	@rc=0; $(IDB_UPGRADE_DIR)/old > $(IDB_UPGRADE_DIR)/old.log 2>&1 || rc=$$?; test "$$rc" -eq 1
	@python3 tests/unit/idb_upgrade_scope_check.py $(IDB_UPGRADE_DIR)/old.log
test-idb-upgrade-scope: test-idb-upgrade-scope-negctl $(IDB_UPGRADE_DIR)/current
	@$(IDB_UPGRADE_DIR)/current > $(IDB_UPGRADE_DIR)/current.log 2>&1
	@cat $(IDB_UPGRADE_DIR)/current.log
test-idb-upgrade-scope-san: test-idb-upgrade-scope $(IDB_UPGRADE_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 $(IDB_UPGRADE_DIR)/san > $(IDB_UPGRADE_DIR)/san.log 2>&1
	@cat $(IDB_UPGRADE_DIR)/san.log
ci-host: test-idb-upgrade-scope
