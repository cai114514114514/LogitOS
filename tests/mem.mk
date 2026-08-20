# --- the ring-3 heap: what it COSTS, not what it holds ----------------------
#
# In its own .mk rather than in the Makefile for the reason tests/jsperf.mk
# gives: several lines share this tree, and a whole-file Makefile overwrite from
# a concurrent line has silently deleted other people's targets more than once.
# A separate file cannot be clobbered that way. The only tokens this needs in
# the main Makefile are its `-include` and the two -D flags on the browser's
# malloc_big.o recipe.
#
#   make test-arena     the gate: the heap is reserved, not linked into .bss,
#                       and the commit bound still refuses. Runs BOTH negative
#                       controls, each required to FAIL.
#   make bench-arena    how much heap each page of the cssweb corpus needs,
#                       measured through mini-libc's own allocator.

# --- test-arena --------------------------------------------------------------
# The browser's arena was a 96 MiB .bss array, and elf_load (c/kernel/exec/elf.c)
# does pmm_alloc() + memset(0) for every page of p_memsz -- so it was resident
# from launch, whatever the page did. It is now SYS_MMAP'd, so frames appear on
# first touch. This asserts both halves of that change:
#   -DARENA_NO_MMAP    the static array back -> the .bss-size check fails
#   -DARENA_NO_BOUND   no commit bound       -> malloc hands out past the limit,
#                                               which on the machine is a fault
#                                               in a page the RAM cannot back
# The .bss figure is read off the compiled malloc.o with `size`, not asserted by
# the program about itself. See the header of tests/unit/arena_mem_test.c.
test-arena:
	@sh tests/unit/arena_run.sh $(BUILD)

# --- bench-arena -------------------------------------------------------------
# The same pipeline css_bench times, measured for MEMORY instead, and linked
# against c/apps/libc/src/malloc.c under its real names so LibCSS's own
# malloc/realloc traffic is counted too -- css_bench links glibc and therefore
# cannot answer "how much memory does this page need". Reports live peak and
# resident high-water separately; see the header of tests/unit/arena_page_mem.c
# for why those are different questions.
#
# Each page runs in its OWN PROCESS. The render pipeline has no teardown, so a
# single-process run makes every page's figures include every page before it.
#   make bench-arena
#   make bench-arena ARENA_PAGES=tests/fixtures/cssweb/github
ARENA_PAGES ?= $(sort $(dir $(wildcard tests/fixtures/cssweb/*/index.html)))

$(BUILD)/arena_page_mem: tests/unit/arena_page_mem.c $(BUILD)/libcss_host.a \
                    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                    c/apps/browser/css_extra.c c/apps/browser/layout.c \
                    c/apps/browser/browser_paint.c c/apps/libc/src/malloc.c \
                    $(HTML_PARSER_SRC)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DARENA_SIZE=402653184u \
	    -o $@ tests/unit/arena_page_mem.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) c/apps/libc/src/malloc.c $(BUILD)/libcss_host.a

bench-arena: $(BUILD)/arena_page_mem
	@$(BUILD)/arena_page_mem $(ARENA_PAGES)

# --- bench-arena-js: can the heap hold a WHOLE web application? -------------
# js_bench compiles one bundle at a time and frees the runtime between fixtures,
# which answers "what does this file cost to compile". The question that decides
# whether kimi.com can run is CUMULATIVE -- a code-split app loads all its
# chunks into ONE runtime and holds them -- so this builds one runtime, compiles
# every chunk into it and frees nothing. Reports the engine's own live figure
# and, separately, the arena the process actually made resident.
#
# Default corpus is the Web API line's kimi.com capture (79 chunks, 9.0 MB),
# read-only. Override with ARENA_JS.
ARENA_JS ?= $(sort $(wildcard tests/fixtures/webapi/kimi/s*.js))

# A big reservation on purpose: this measures what the app NEEDS, so the ceiling
# must not be what it hits. It is address space, so it costs nothing untouched.
$(BUILD)/arena_js_mem: tests/unit/arena_js_mem.c $(QJS_SRC) c/apps/libc/src/malloc.c
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -DARENA_SIZE=1610612736u \
	    -o $@ tests/unit/arena_js_mem.c $(QJS_SRC) c/apps/libc/src/malloc.c -lm

bench-arena-js: $(BUILD)/arena_js_mem
	@$(BUILD)/arena_js_mem $(ARENA_JS)

.PHONY: test-arena bench-arena bench-arena-js

# --- the block layer's request engine: submit/poll, on the host --------------
#
# HOME NOTE: these two belong in a tests/blkasync.mk of their own, and they are
# here instead because adding a fragment costs one `-include` line in the
# Makefile and the Makefile is contended by several live lines. They sit beside
# the memory gates rather than anywhere else for a reason that is not only
# convenience: c/kernel/mm/swap.c is the CONSUMER this interface was built for,
# and `make test-swap` is the gate that measures whether it did anything.
#
# What it covers and why nothing else does: `make test-fs-host` compiles
# c/fs/*.c against tests/unit/fsstub/blkdev.h -- a five-function stub -- so the
# storage suite has never once compiled c/drivers/block/blkdev.c. Everything
# below blk_read() was device-only until this target existed.
BLKREQ_CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter \
                 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -DBLK_HOSTTEST -Ic/drivers/block -Ic/drivers/virtio -Ic/kernel/core

.PHONY: test-blk-async test-blk-async-negctl

# The control is a PREREQUISITE of the positive, not a sibling on the ci-host
# line below, and CLAUDE.md's audit section says exactly why: a *-negctl that is
# its own target and is named by nobody is excluded from the suite listing by
# the NOT_CI regex AND invoked from nowhere, so it runs never while looking
# exactly like a control that is covered. There were 55 in that state when it
# was last counted. Naming it beside test-blk-async on ci-host would satisfy the
# UNWIRED audit and still run it never, which is the worse of the two failures
# because it looks fixed.
test-blk-async: test-blk-async-negctl
test-blk-async:
	@mkdir -p $(BUILD)
	@$(CC) $(BLKREQ_CFLAGS) -o $(BUILD)/blkreq_test \
	    tests/unit/blkreq_test.c c/drivers/block/blkdev.c
	@$(BUILD)/blkreq_test

# THE NEGATIVE CONTROL, and it is aimed at the one property this change
# introduces rather than at the file as a whole. -DBLK_NO_INTERLOCK deletes the
# three lines in blk_submit() that drive an already-in-flight request to
# completion before starting another. EVERYTHING ELSE STILL WORKS: reads return
# the right bytes, chunking is right, bounds are right, the bounce is right --
# which is the point. The only thing that breaks is that two commands are on the
# medium at once, and the fake driver's `overlap` counter is the only witness
# there is. That is exactly the shape of the bug asynchrony introduces into a
# driver that used to be protected by never being preempted.
test-blk-async-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(BLKREQ_CFLAGS) -DBLK_NO_INTERLOCK -o $(BUILD)/blkreq_negctl \
	    tests/unit/blkreq_test.c c/drivers/block/blkdev.c
	@if $(BUILD)/blkreq_negctl > $(BUILD)/blkreq_negctl.log 2>&1; then \
	    echo "CONTROL FAILED: an engine with no in-flight interlock PASSED the suite"; \
	    cat $(BUILD)/blkreq_negctl.log; exit 1; \
	 else \
	    echo "negative control OK -- with the interlock removed, these fail:"; \
	    grep FAIL $(BUILD)/blkreq_negctl.log; \
	    tail -1 $(BUILD)/blkreq_negctl.log; \
	 fi

# Into the host suite, from the fragment that owns the target -- ci-host takes
# prerequisites from any fragment, so membership costs one line here and needs
# no edit to the contended Makefile.
#
# It is worth one line because of what the rest of the storage suite does NOT
# cover, measured rather than assumed: `make test-fs-host` compiles c/fs/*.c
# against tests/unit/fsstub/blkdev.h, a five-declaration stub, so test-fs-crash's
# 1,744 power-cut checks never compile c/drivers/block/blkdev.c at all. A
# reordering introduced by the submit/poll split would not redden one of them.
# This is the only host gate that runs the request engine.
ci-host: test-blk-async

# --- the out-of-memory killer: WHICH process dies ---------------------------
#
# HOME NOTE, the same one the block line's targets above give: these belong in a
# tests/oom.mk of their own and are here because adding a fragment costs one
# `-include` line in the Makefile and the Makefile is contended by several live
# lines. They sit beside the memory gates because that is what they are.
#
# c/kernel/mm/fault.c used to return 0 when memory was gone even after a forced
# reclaim, so the process that DIED was whoever touched memory next -- which on
# a machine one program has emptied is essentially never that program. The
# killer chooses instead. See c/kernel/mm/oom.h for the policy and why "kill the
# biggest" is wrong on this machine specifically.
#
#   test-oom        HOST, seconds, ASan+UBSan. The real oom.c over the real
#                   reverse map, 84 checks. INCLUDES ITS OWN NEGATIVE CONTROL:
#                   -DOOM_KILL_NEWEST is a plausible wrong policy (blame the
#                   process that just started) and is required to redden exactly
#                   12 assertions -- all of them about WHICH process, none about
#                   survival. The count is pinned; see tests/unit/oom_run.sh for
#                   why "it failed" is not good enough on its own.
#   test-oom-os     ON DEVICE, minutes. A hog takes most of a small machine and
#                   an innocent program then asks for a few MiB. The assertion
#                   is that the INNOCENT one completes -- "something died" is
#                   true of the bug as well as of the fix.
#
# Tunable: make test-oom-os OOM_RAM=256 OOM_HOG=192 OOM_SMALL=32
#
# THE THREE SIZES ARE A WINDOW, NOT A GUESS, and getting them wrong fails in a
# way that reads like a broken killer. Measured on this machine 2026-08-20: the
# desktop holds about 53 MiB at boot (192 MiB machine, 139 MiB free with the
# shell up). The requirement is
#
#     desktop + hog  <  RAM  <  desktop + hog + innocent
#
# -- the hog must fit so it becomes fully resident, and the innocent must NOT.
# At RAM=192/HOG=160 the FIRST attempt at this gate, the machine was so tight
# that /bin/as could not start: one run reported "[execve] /bin/as: read failed"
# and the next faulted inside as at cr2=0xffffffffffffffff before printing
# anything. Both are the loader running out, not the killer working -- and the
# same two programs run correctly backgrounded on a 512 MiB machine, which is
# what proved it. So the slack below is deliberate: ~60 MiB spare after the hog,
# which is enough for a 1 MiB program to load and set up mini-libc's arena, and
# an innocent big enough that it cannot fit in what is left.
OOM_RAM   ?= 320
OOM_HOG   ?= 200
OOM_SMALL ?= 96

.PHONY: test-oom test-oom-os
test-oom:
	@sh tests/unit/oom_run.sh $(BUILD)

test-oom-os: $(ISO) $(DISK)
	@bash tests/boot/run-oom-test.sh $(ISO) $(DISK) $(OOM_RAM) $(OOM_HOG) $(OOM_SMALL)
