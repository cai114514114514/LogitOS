# WHAT AN OPEN FILE DESCRIPTION COSTS -- c/kernel/exec/file.c's F_VFS backend.
#
# Its own fragment, included from tests/exec.mk rather than from the top-level
# Makefile, for the reason tests/poll.mk, tests/procfs.mk, tests/coredump.mk and
# tests/fsgeom.mk all give: the Makefile is contended and `-include` nests.
#
# THE CLAIM, and it is one sentence: a READ-ONLY F_VFS descriptor holds no bytes,
# so an fd costs the same whatever the file's size. Until this change it held the
# whole file in one kmalloc, which made "the largest file this machine can open"
# a question about kheap's grow() -- it DOUBLES an arena and then asks
# pmm_alloc_contig() for one contiguous run -- rather than a question about the
# filesystem. Opening a 128 MiB file grew the kernel heap by 256 MiB; a 256 MiB
# one was refused outright.
#
#   test-fdstream         ON THE MACHINE: an fd costs 0 B at 9,636 B and at
#                         2,222,276 B, the bytes still read back byte-identical
#                         against a whole-file vfs_read, and a RENDERED node
#                         (/dev/vfsmounts) still keeps its buffer -- vfs_pread
#                         refuses a synthetic node at any non-zero offset, so
#                         streaming one would return -1 on its second read.
#   test-fdstream-negctl  the same, against a kernel built -DFILE_SLURP, where
#                         the fd must cost EXACTLY the file. Runs WITH its
#                         positive, as a PREREQUISITE and never as a recipe
#                         line -- see the note above test-exec, and
#                         tests/audit-stranded.baseline for what naming it on a
#                         `ci-boot:` line instead would buy: it would satisfy
#                         the UNWIRED audit and still run the control never.
#   test-fdstream-big     THE CEILING, on an image carrying a file larger than
#                         the kernel heap can hold. Not wired into any suite and
#                         not a prerequisite of anything: the image is not built
#                         by `make` (nothing in this tree ships a 355 MiB file),
#                         so the target SKIPs when it is absent, and a gate that
#                         skips is not a gate. It is here so the run is
#                         reproducible by name rather than remembered.
#
# WHY NOT A HOST TEST. file.c's read path is three lines over vfs_pread; what is
# actually being measured is the KERNEL HEAP, and a host harness would have to
# supply its own kmalloc and would then be measuring that. The instrument is
# /dev/fsbench's `openfd` (c/fs/fsbench.c), which this line of work neither
# wrote nor owns.

.PHONY: test-fdstream test-fdstream-negctl test-fdstream-big

FDSTREAM_MEM  ?= 512
# The image the ceiling gate wants: anything with a file bigger than the heap.
# Overridable, because the file that motivated this is a language model nobody
# is going to commit.
FDSTREAM_BIGIMG  ?= $(BUILD)/qwendisk.img
FDSTREAM_BIGFILE ?= /qwen3.lm

test-fdstream: test-fdstream-negctl
test-fdstream: $(ISO) $(DISK)
	@bash tests/boot/run-fdstream.sh $(ISO) $(DISK) $(FDSTREAM_MEM)

test-fdstream-negctl: $(ISO) $(DISK)
	@bash tests/boot/run-fdstream-negctl.sh $(DISK) $(FDSTREAM_MEM)

test-fdstream-big: $(ISO)
	@bash tests/boot/run-fdstream-big.sh $(FDSTREAM_BIGIMG) $(FDSTREAM_BIGFILE) $(FDSTREAM_MEM)

# Two QEMU boots for the gate and two more for its control, so this is a boot
# suite, not a host one.
ci-boot: test-fdstream
