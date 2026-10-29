# Static PIE: independent compiler flags; never change application link bases.
# The negative control is a prerequisite, so it is observed even when only the
# positive target is invoked. The runner rejects a crash as a negative result.
.PHONY: pie-fixtures
pie-fixtures:
	python3 tests/unit/pie_test.py --root . --build $(BUILD)/pie --fixtures-only
