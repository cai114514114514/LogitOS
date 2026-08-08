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
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c \
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
