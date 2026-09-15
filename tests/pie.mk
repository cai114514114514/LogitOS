# Static PIE: independent compiler flags; never change application link bases.
# The negative control is a prerequisite, so it is observed even when only the
# positive target is invoked. The runner rejects a crash as a negative result.
.PHONY: test-pie test-pie-negctl pie-fixtures
test-pie: test-pie-negctl
	python3 tests/unit/pie_test.py --root . --build $(BUILD)/pie-check --positive-only
test-pie-negctl:
	python3 tests/unit/pie_test.py --root . --build $(BUILD)/pie-check --negative-only
pie-fixtures:
	python3 tests/unit/pie_test.py --root . --build $(BUILD)/pie --fixtures-only
