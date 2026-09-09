# A shared host-apparatus gate for tools/filesize.sh.  The negative control is
# a prerequisite rather than a ci-host decoration: it must run every time the
# positive claim runs, because the regression was a command that failed while
# its enclosing build still succeeded.
.PHONY: test-filesize test-filesize-negctl

test-filesize-negctl:
	@bash tests/unit/filesize_test.sh $(BUILD)/filesize-test --negctl

test-filesize: test-filesize-negctl
	@bash tests/unit/filesize_test.sh $(BUILD)/filesize-test

ci-host: test-filesize
