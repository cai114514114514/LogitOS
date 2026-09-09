#!/usr/bin/env bash
# Build /bin/jsmicro for the MACHINE, from $(ENGINE_OBJ) -- the literal object
# set build/browser.elf links (Makefile:1006).  Same objects, same JS_CF, same
# mini-libc arena, same -msse2, same -DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG.
#
# Why a script and not a tests/*.mk fragment: five other lines are live in this
# tree right now and the $(DISK) recipe is in the shared root Makefile.  Adding
# a pack token there is the one edit that a concurrent whole-file overwrite
# silently loses -- the failure the Makefile's own pre-wiring block exists to
# prevent.  This script instead re-runs the disk image's own mkfs command with
# two extra pairs appended, so no tracked file changes at all.
#
# The object list below is $(ENGINE_OBJ) spelled out.  A hand-copied source
# list is rule 2 in CLAUDE.md and it is exactly how test-canvas went dark, so
# this does not copy it -- it derives it from the same wildcards the Makefile
# uses and then REFUSES if any object is absent, which is the check a copied
# list cannot make.
set -euo pipefail
cd "$(dirname "$0")/../.."
B="${BUILD:-build}"

make "$B/jsbench.aex" BUILD="$B" -j8 >/dev/null          # builds $(ENGINE_OBJ)
make "$B/jsobj/tests/jsmicro/micro_run.o" BUILD="$B" >/dev/null

ENG=()
for f in third_party/quickjs/{quickjs,cutils,libregexp,libunicode,libbf}.c \
         third_party/libm/*.c c/apps/libc/src/*.c; do
    [ "$f" = "c/apps/libc/src/malloc.c" ] && continue
    ENG+=("$B/jsobj/${f%.c}.o")
done
for f in c/apps/libc/src/*.asm; do ENG+=("$B/jsobj/${f%.asm}.o"); done
for o in "${ENG[@]}"; do [ -f "$o" ] || { echo "ENGINE_OBJ member absent: $o" >&2; exit 1; }; done
echo "engine objects: ${#ENG[@]}"

nasm -f elf64 c/apps/crt0_cli.asm -o "$B/jsmicro.crt0c.o"
ld.lld -nostdlib -e _start -Ttext=0x50000000 -o "$B/jsmicro.elf" --start-group \
    "$B/jsmicro.crt0c.o" "$B/jsobj/tests/jsmicro/micro_run.o" "${ENG[@]}" \
    "$B/jsbenchobj/malloc_big.o" --end-group
python3 tools/mkaex.py "$B/jsmicro.elf" "$B/jsmicro.aex" jsmicro - '?' 150 150 150

# --- the disk image, with the cases on it -----------------------------------
# make -n gives the literal mkfs invocation; the cases are appended to it.
# The continuations are joined FIRST (CLAUDE.md rule 2) before anything is read
# out of it.
make -n build/disk.img 2>/dev/null | python3 -c '
import re, sys
t = re.sub(r"\\\r?\n[ \t]*", " ", sys.stdin.read())
for line in t.split("\n"):
    if "tools/mkfs.py" in line:
        sys.stdout.write(line.strip()); break
else:
    sys.exit("no mkfs.py invocation in make -n output")
' > "$B/mkfs_base.txt"
[ -s "$B/mkfs_base.txt" ] || { echo "could not read the disk recipe" >&2; exit 1; }

CASES=()
for f in tests/jsmicro/cases/*.js tests/jsmicro/chars/*.js; do CASES+=("$f:/jsmicro/$(basename "$f")"); done

# Drop pairs whose host file does not exist and SAY SO.  Five lines share this
# tree; another one has already added a pack entry for a binary it has not
# built yet, and a silent drop here would be a disk image quietly missing
# somebody else's program.  Dropping loudly is the difference between a private
# copy of the image and a wrong one.
BASE=()
for tok in $(sed "s|tools/mkfs.py build/disk.img|tools/mkfs.py $B/disk.img|" "$B/mkfs_base.txt"); do
    host="${tok%%:*}"
    if [ "${#BASE[@]}" -gt 2 ] && [ ! -e "$host" ]; then
        echo "  dropping (host file absent, not mine to build): $tok"
        continue
    fi
    BASE+=("$tok")
done
"${BASE[@]}" "$B/jsmicro.aex:/bin/jsmicro" "${CASES[@]}" >/dev/null
ls -la "$B/jsmicro.aex" "$B/disk.img"
