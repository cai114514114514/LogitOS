# A fresh isolated BUILD must be created by the graph before any host gate
# recipe runs. The negative control removes just that rule from a private
# Makefile copy and requires the old, exact "No rule to make target" failure.
.PHONY: test-build-dir test-build-dir-negctl
test-build-dir: test-build-dir-negctl
	@python3 tests/tools/make/build_dir.py

test-build-dir-negctl:
	@python3 tests/tools/make/build_dir.py --control

ci-host: test-build-dir
