# The same regression gate first watches missing nested includes go red, then
# restores them. It is a prerequisite so the audit cannot skip its own control.
.PHONY: test-mk-wired-regression
test-mk-wired: test-mk-wired-regression
test-mk-wired-regression:
	@python3 tests/tools/make/wired.py
