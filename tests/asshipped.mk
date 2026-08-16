# SW-shipped-compiler -- /bin/as stops containing a C compiler.
#
# THE SHAPE, argued once here so it is not re-derived from the rules:
#
#   The compiler that ships is /usr/as/lib/asc.la, a FILE ON THE DISK, not
#   bytes inside the binary. It is loaded through the VM's ordinary module
#   loader (`from asc import ...`), exactly as fsroot/as/examples/selfhost.as
#   already loads it. Embedding it instead would have made /bin/as
#   self-contained -- and would have put a SECOND copy of a 38 KB compiler in
#   the image, one of which could go stale against the other, with nothing in
#   the build able to tell which one ran. The disk already carries asc.la
#   ($(AS_LA)); one copy, one answer.
#
#   The cost of that choice is a file that can go missing, and that cost is
#   paid explicitly: c/apps/as/as.c preflights both /usr/as/lib/asc.la and
#   /usr/as/lib/aslex.la and refuses BY PATH, and test-as-shipped-negctl deletes
#   asc.la on a running machine to watch it happen.
#
#   ONE thing is embedded: build/asboot.la, three statements compiled from
#   c/apps/as/asboot.as. It is the bridge -- as.c cannot call an AetherScript
#   function directly (as.h exports no such entry point), so the source goes in
#   through a module global and the bytecode comes back out through another.
#   It is embedded because it is the binary's own logic, not a component the
#   image ships: a version of it on disk could disagree with the C that reads
#   its globals, and nothing would catch that.
#
# The host build/asc is UNCHANGED and still contains compiler.c + lexer.c: it is
# the bootstrap that produces every .la at build time (asc.la included) and the
# oracle half of make test-as-crosscheck. as.c is the only file that differs
# between the two, via -DAS_SELFHOST_COMPILER.

# --- the embedded bridge ---------------------------------------------------
# Compiled by the HOST asc (the C one) -- this is the bootstrap edge, and it is
# the only place the shipped binary's contents depend on a C compiler. check-asops
# is an order-only prerequisite for the same reason the $(BUILD)/%.la rule has
# it: an opcode drift here is a silent miscompile.
$(BUILD)/asboot.la: c/apps/as/asboot.as $(ASC) | check-asops
	@mkdir -p $(BUILD)
	$(ASC) -c $< -o $@

$(BUILD)/asboot_la.inc: $(BUILD)/asboot.la tools/bin2inc.py
	python3 tools/bin2inc.py $< $@ as_boot_la

# as.c is the one translation unit that is compiled differently for /bin/as than
# for build/asc. An explicit rule beats the $(BUILD)/asobj/%.o pattern rule
# regardless of where it appears, so this can live in the fragment.
$(BUILD)/asobj/c/apps/as/as.o: c/apps/as/as.c $(AS_HDRS) $(BUILD)/asboot_la.inc
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DAS_SELFHOST_COMPILER -I$(BUILD) -c $< -o $@

# --- the gates -------------------------------------------------------------
# Host, seconds: the shipped .elf contains no compiler.c and no lexer.c.
test-as-shipped: $(BUILD)/as.elf
	@bash tests/unit/run-as-shipped.sh $(BUILD)/as.elf

# On the machine: the compiler that runs is the FILE, and removing it is a
# named refusal rather than a hang, a fault or a silent fallback. This is the
# negative control for the whole change -- if it passes with asc.la deleted,
# something in /bin/as is still compiling.
test-as-shipped-negctl: $(ISO) $(DISK)
	@bash tests/boot/run-as-shipped-negctl.sh $(ISO) $(DISK)

test-as-shipped-all: test-as-shipped test-as-shipped-negctl

# --- MEASURED COST, and one blocked gate ------------------------------------
# On the machine (QEMU/TCG, -smp 1, average of 3 after a discarded warm-up,
# fsroot/as/examples/ascbench.as, same ISO both sides):
#
#     whole `run()` of /bin/as        C compiler      asc.la
#     hello.as (6 lines)              70 / 70 ms      70 / 76 ms   best/avg
#     -c ash.as (204 lines)           70 / 73 ms     200 / 200 ms
#
# So a trivial script is unchanged at best-case and ~6 ms slower on average
# (process spawn dominates at ~70 ms), and the work the AetherScript shell adds
# to its own start is +130 ms. /bin/as itself is 44,360 bytes SMALLER
# (367,472 -> 323,112).
#
# test-as-os IS RED WITH THIS CHANGE ON THE 2026-08-16 TREE, and the cause is
# not this change: the kernel currently deadlocks (whole machine, mid-print, no
# panic) after roughly 30-38 fork/exec cycles of any memory-touching ring-3
# program under -smp 4. Measured with the UNMODIFIED C-compiler /bin/as on the
# same ISO: `hello.as` x40 stuck after 37, `selfhost.as` x30 stuck after 18, and
# -- the control that settles it -- `/bin/libctest` x25, which contains no
# AetherScript at all, stuck after ~33 forks. Every one of those is green at
# -smp 1, as is the full test-as-os example list with this change (16/16, 7 s).
# What this change does is raise the per-run cost enough to reach that
# threshold inside test-as-os's own 16 commands. The boot log of the same tree
# shows an in-flight wait-queue subsystem (WAITQ_SELFTEST_OK, "[waitq] ...
# bkl_hlt_wait spin re-tested ..."), which is where to look first.

.PHONY: test-as-shipped test-as-shipped-negctl test-as-shipped-all
