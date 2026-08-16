#!/usr/bin/env bash
# SW-shipped-compiler gate: /bin/as CONTAINS NO C COMPILER.
#
# The milestone is "the compiler that ships stops being C". A test that only
# showed `as foo.as` still working would prove nothing -- it works either way.
# What has to be shown is a NEGATIVE: that compiler.c and lexer.c are not in the
# binary at all, so the only thing that can have turned the source into bytecode
# is /usr/as/lib/asc.la, the compiler written in AetherScript.
#
# WHY A SYMBOL CHECK IS THE HONEST FORM OF THAT, and where it stops:
#
#  - The obvious spelling, "assert as_compile is absent", is WRONG and would
#    have passed vacuously. as_compile and as_compile_module are called from
#    vm.c (as_interpret, as_import), so the link needs both names; c/apps/as/as.c
#    defines them as failing stubs. The symbols exist. What distinguishes a stub
#    from a compiler is SIZE, so that is checked instead (nm -S, below).
#
#  - The strong assertion is over compiler.c's and lexer.c's STATIC functions.
#    They are file-local, so each one that appears in the symbol table can only
#    have come from that translation unit. The list is derived from the sources
#    at run time rather than hand-copied, so a function renamed in compiler.c
#    does not silently drop out of the check -- and names that a REMAINING
#    translation unit also defines as static are subtracted, since those would
#    be false positives.
#
#  - A symbol table can be stripped. So the same claim is also made against the
#    binary's BYTES: compiler.c's diagnostic strings ("expected ')' after
#    arguments" and friends) live in .rodata and survive stripping. If someone
#    strips as.elf, the symbol half goes quiet; the string half does not.
#
#  - What this gate CANNOT show is that the AetherScript compiler is the one
#    actually doing the work at run time. Absence of C is not presence of asc.
#    That half is tests/boot/run-as-shipped-negctl.sh: delete
#    /usr/as/lib/asc.la on the running machine and the same `as hello.as` that
#    worked a second earlier must fail, by path. The two together are the proof.
#
# Positive controls run first: if `nm` produced nothing (wrong file, no symbol
# table, a toolchain that failed silently), every "absent" below would hold and
# the gate would pass while measuring nothing.
set -u

ELF="${1:?usage: run-as-shipped.sh <as.elf>}"
NM="${NM:-nm}"
fail=0
say() { echo "  $*"; }

[ -f "$ELF" ] || { echo "FAIL: $ELF does not exist"; exit 1; }

SYMS="$(mktemp)"; SIZES="$(mktemp)"
trap 'rm -f "$SYMS" "$SIZES"' EXIT
"$NM" "$ELF" > "$SIZES" 2>/dev/null || { echo "FAIL: $NM could not read $ELF"; exit 1; }
awk '{print $NF}' "$SIZES" | LC_ALL=C sort -u > "$SYMS"
nsym=$(wc -l < "$SYMS" | tr -d ' ')

# ---------------------------------------------------------- positive controls
for want in main as_run as_load as_module_slot; do
    grep -qx "$want" "$SYMS" || { say "FAIL(control): $want is NOT in $ELF -- the symbol table is not being read"; fail=1; }
done
if [ "$nsym" -lt 200 ]; then
    say "FAIL(control): only $nsym symbols in $ELF; this binary has ~1100. Stripped or wrong file."
    fail=1
fi

# ------------------------------------------------- 1. the C lexer is gone
# as_lex() is lexer.c's only external entry, and as.c's -lex mode was its only
# caller in this binary. Both are host-side now.
if grep -qx "as_lex" "$SYMS"; then
    say "FAIL: as_lex is in $ELF -- lexer.c is still linked into the shipped binary"
    fail=1
fi

# --------------------------- 2. compiler.c / lexer.c statics are gone
# Derived, not hand-copied. Subtract any name that a translation unit still in
# the link also defines as static (e.g. two files with their own `advance`).
#
# "still in the link" is c/apps/as minus the two, PLUS mini-libc: /bin/as links
# c/apps/libc/src too, and that is not hypothetical -- stdio.c has its own
# `static void emit(...)`, which the first run of this gate reported as
# compiler.c's `emit` and failed on. Subtracting only c/apps/as would have made
# this test cry wolf on a binary that was already correct.
gone_src="c/apps/as/compiler.c c/apps/as/lexer.c"
kept_src="$(ls c/apps/as/*.c c/apps/libc/src/*.c | grep -vE '/(compiler|lexer)\.c$' | tr '\n' ' ')"
statics_of() {
    # shellcheck disable=SC2086
    grep -hoE '^static [A-Za-z_][A-Za-z0-9_ *]*[ *]([a-z_][a-z0-9_]*)\(' $1 2>/dev/null \
        | sed -E 's/.*[ *]([a-z_][a-z0-9_]*)\($/\1/' | LC_ALL=C sort -u
}
GONE="$(mktemp)"; KEPT="$(mktemp)"
trap 'rm -f "$SYMS" "$SIZES" "$GONE" "$KEPT"' EXIT
statics_of "$gone_src" > "$GONE"
statics_of "$kept_src" > "$KEPT"
cand="$(LC_ALL=C comm -23 "$GONE" "$KEPT")"
ncand=$(printf '%s\n' "$cand" | grep -c . || true)
if [ "$ncand" -lt 40 ]; then
    say "FAIL(control): only $ncand compiler/lexer-only static names derived (expected ~80)."
    say "               The derivation stopped matching; it is not proving anything."
    fail=1
fi
hits=""
for s in $cand; do
    if grep -qx "$s" "$SYMS"; then hits="$hits $s"; fi
done
if [ -n "$hits" ]; then
    say "FAIL: compiler.c/lexer.c statics found in $ELF:$hits"
    fail=1
fi

# ------------------------------- 3. as_compile* are present but are STUBS
# nm -S prints the size in column 2 for sized symbols. The check is deliberately
# loose: it is asking "is this a function body or a message", not pinning a byte
# count.
#
# MEASURED, so nobody over-reads it: on the pre-change binary as_compile_module
# is 1697 bytes and the stub is 81 -- that half is decisive. as_compile is 65
# bytes even in the C build (it is a four-line wrapper around
# as_compile_module), so its 30-byte stub proves much less. Both are asserted
# because a future as_compile that grew a body should trip something, but the
# load-bearing one is as_compile_module.
for s in as_compile as_compile_module; do
    line="$("$NM" -S "$ELF" 2>/dev/null | awk -v s="$s" '$NF==s {print $2; exit}')"
    if [ -z "$line" ]; then
        say "FAIL: $s has no size in the symbol table -- cannot tell a stub from a compiler"
        fail=1
        continue
    fi
    sz=$((16#$line))
    if [ "$sz" -ge 256 ]; then
        say "FAIL: $s is $sz bytes -- that is a compiler, not the failing stub as.c defines"
        fail=1
    else
        say "ok: $s is $sz bytes (a stub)"
    fi
done

# ------------------------------------------ 4. the bytes, not just the table
# .rodata survives `strip`. These strings exist only in compiler.c.
CSTR="expected ')' after arguments
'break' outside a loop
a class cannot inherit from itself
empty expression in f-string"
found=""
while IFS= read -r s; do
    [ -n "$s" ] || continue
    if LC_ALL=C grep -aqF "$s" "$ELF"; then found="$found|$s"; fi
done <<EOF
$CSTR
EOF
if [ -n "$found" ]; then
    say "FAIL: compiler.c diagnostics are in the binary's bytes:$found"
    fail=1
fi
# Control for that check: a string that MUST be there (the path the shipped
# binary names when its compiler is missing). Without it, a grep that always
# returns "not found" would look like a pass.
if ! LC_ALL=C grep -aqF "/usr/as/lib/asc.la" "$ELF"; then
    say "FAIL(control): /usr/as/lib/asc.la is not in $ELF -- the byte search is not working,"
    say "               or this binary was not built with -DAS_SELFHOST_COMPILER"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: /bin/as still contains a C compiler (or the gate could not tell)"
    exit 1
fi
echo "as-shipped: $nsym symbols, $ncand compiler/lexer-only names checked, none present;"
echo "as-shipped: as_compile* are stubs; no compiler.c diagnostics in .rodata"
echo "PASS: the shipped /bin/as contains no C compiler"
