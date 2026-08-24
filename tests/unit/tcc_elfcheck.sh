#!/usr/bin/env bash
# Is this ELF a program the LogitOS kernel will load? Judged with readelf,
# which shares no code with c/kernel/exec/elf.c -- so this is a second opinion
# on the file, not the loader agreeing with itself.
#
# Six checks, one line each, so a control can be required to fail EXACTLY the
# checks it was built to fail (tests/tcc.mk counts the FAIL lines):
#   type      ET_EXEC        (ET_DYN needs relocation; the loader applies none)
#   machine   x86-64
#   entry     in [0x40000000, 0x7C000000]   elf.h's elf_entry_in_user_region
#   loads     every PT_LOAD inside [0x40000000, 0x80000000)
#   interp    no PT_INTERP   (there is no dynamic loader to name)
#   dynamic   no PT_DYNAMIC
set -u
f="${1:?usage: tcc_elfcheck.sh <elf>}"
fail=0
ok()   { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

H=$(readelf -h "$f") || { echo "  FAIL: readelf cannot read $f"; exit 1; }
L=$(readelf -lW "$f")

grep -q 'Type:[[:space:]]*EXEC' <<<"$H" && ok "type: ET_EXEC" || bad "type: not ET_EXEC"
grep -q 'Machine:.*X86-64' <<<"$H" && ok "machine: x86-64" || bad "machine: not x86-64"

entry=$(sed -n 's/.*Entry point address:[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' <<<"$H")
if [ -n "$entry" ] && [ $((entry)) -ge $((0x40000000)) ] && [ $((entry)) -le $((0x7C000000)) ]; then
    ok "entry $entry is inside the private user region"
else
    bad "entry ${entry:-?} is outside the private user region [0x40000000, 0x7C000000]"
fi

# readelf -lW prints one LOAD per line: LOAD offset vaddr paddr filesz memsz flg align
# The rule is the kernel's (c/kernel/exec/elf.c, the PT_LOAD pass): a segment
# that ends at or below 0x40000000 is SKIPPED, not mapped -- lld emits a
# read-only headers segment at 0x200000 in every .aex this tree builds -- and
# one that straddles or sits above the region is refused. At least one LOAD
# must land inside it, or there is nothing to run (ELF_E_NOLOAD).
nload=0; badload=0; inload=0
while read -r _ _ vaddr _ _ memsz _; do
    nload=$((nload + 1))
    lo=$((vaddr)); hi=$((vaddr + memsz))
    if [ "$hi" -le $((0x40000000)) ]; then
        echo "      LOAD $vaddr+$memsz is below the region: skipped by the loader, not mapped"
    elif [ "$lo" -lt $((0x40000000)) ] || [ "$hi" -gt $((0x80000000)) ]; then
        badload=$((badload + 1))
        echo "      LOAD $vaddr+$memsz straddles or exceeds the region"
    else
        inload=$((inload + 1))
    fi
done < <(grep -E '^[[:space:]]*LOAD' <<<"$L")
if [ "$inload" -gt 0 ] && [ "$badload" -eq 0 ]; then
    ok "PT_LOAD range: $inload of $nload segments inside the private user region, none straddling it"
else
    bad "PT_LOAD range: $inload of $nload segments inside the region, $badload straddling it"
fi

grep -qE '^[[:space:]]*INTERP' <<<"$L" && bad "no PT_INTERP: the file names a dynamic loader" || ok "no PT_INTERP"
grep -qE '^[[:space:]]*DYNAMIC' <<<"$L" && bad "no PT_DYNAMIC: the file has a dynamic section" || ok "no PT_DYNAMIC"

exit $fail
