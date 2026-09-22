# These fixtures run tiny independent make graphs in temporary directories;
# neither the OS build tree nor a guest is involved.
.PHONY: test-build-jobs test-build-jobs-negctl

test-build-jobs-negctl:
	python3 tests/tools/make/jobs.py --negative

test-build-jobs: test-build-jobs-negctl
	python3 tests/tools/make/jobs.py

ci-host: test-build-jobs
