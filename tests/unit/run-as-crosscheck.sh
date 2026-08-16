#!/usr/bin/env bash
# DX gate: THE TWO AETHERSCRIPT COMPILERS AS EACH OTHER'S ORACLE.
#
# This tree's method everywhere else is a SECOND INDEPENDENT IMPLEMENTATION:
# mini-libc is diffed against glibc, the H.264 decoder against ffmpeg byte for
# byte, the crypto against openssl vectors, the 2D engine against an analytic
# reference. AetherScript is the one place that already HAS two full
# implementations of the same specification --
#
#     the C compiler        c/apps/as/compiler.c + lexer.c  (what /bin/as runs)
#     the self-hosted one   fsroot/as/lib/asc.as + aslex.as (written in AS)
#
# -- and until this script they were never compared. The neighbouring gates do
# not close it:
#   * run-selfhost-lex.sh compares TOKENS, not bytecode -- it stops at the lexer.
#   * run-selfhost-compile.sh compares RUNTIME OUTPUT of 9 programs. Two
#     compilers can emit different code that prints the same thing, and nine
#     programs is nine.
#   * run-selfhost-fixpoint.sh compiles exactly ONE input, asc.as. It proves the
#     self-hosted compiler is a fixed point of ITSELF; it says nothing about any
#     other program, and nothing about the C compiler on any program but that
#     one.
# So a divergence on any of the other fifty .as files in the tree is invisible
# today. This gate compiles EVERY .as in the tree with BOTH compilers and diffs
# the .la bytes.
#
# WHAT A FAILURE HERE MEANS. It does not say which side is wrong -- two
# implementations disagreeing name a defect without locating it. It says the
# specification has a hole at a nameable offset in a nameable file, which is the
# whole value: CLAUDE.md's SELF-HOSTING TAX paragraph records that an opcode
# drift in asc.as is a SILENT MISCOMPILE that test-as and test-as-gcstress stay
# fully green through. This gate is not silent about it.
#
# LIMITS, stated rather than implied:
#   * Byte-identical output is a STRONGER requirement than semantic agreement.
#     A legitimate optimisation added to one compiler and not the other fails
#     this gate. That is deliberate while both compilers are meant to be the
#     same compiler twice; the day they are meant to diverge, this gate has to
#     be rewritten around `-dis` equivalence, not relaxed with a tolerance.
#   * It compiles. It does not RUN what it compiled -- that is
#     run-selfhost-compile.sh's job, and the two are complementary.
#   * Only files reachable by the globs below are covered; see MIN_CORPUS.
#
# usage: run-as-crosscheck.sh <asc> [libdir]
#   <asc>    host `as` binary (build/asc) -- provides BOTH sides: `-c` is the C
#            compiler, and it is also the VM that interprets asc.as.
#   [libdir] where to take the self-hosted compiler's sources from. Defaults to
#            fsroot/as/lib. The negative control passes a perturbed scratch copy
#            here; nothing else should.
set -u

ASC="${1:?usage: run-as-crosscheck.sh <asc> [libdir]}"
LIBDIR="${2:-fsroot/as/lib}"
ROOT="$PWD"
ALLOW_FILE="tests/unit/as-crosscheck-allow.txt"

# A crosscheck that silently covered three files would be worse than none: it
# would report "0 divergences" in a green line and mean nothing. So the corpus
# size is itself asserted. Raise this when the corpus genuinely grows; it is a
# floor, not an expected value, and it is here to catch a glob that stopped
# matching (a directory move, a rename), not to police how many examples exist.
MIN_CORPUS=45

# How many diverging files get the full hex + disassembly treatment (see the
# sweep below). One wrong constant diverges the whole corpus at once.
DETAIL_MAX=3

[ -x "$ASC" ] || { echo "FAIL: $ASC is not executable"; exit 1; }
[ -f "$LIBDIR/asc.as" ] || { echo "FAIL: no asc.as in $LIBDIR"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The self-hosted side runs as a program, so it needs its own modules importable
# from the cwd it runs in: asc.as (the compiler), aslex.as (its lexer), and the
# driver that calls compile_file(). Same mechanism run-selfhost-compile.sh uses
# -- deliberately not a second one.
cp "$LIBDIR"/*.as "$TMP/" || exit 1
cp tests/unit/asc_driver.as "$TMP/" || exit 1

# ---------------------------------------------------------------- allow-list
# path -> reason. Read into two parallel arrays (bash 3.2 on the macOS host has
# no associative arrays).
allow_paths=(); allow_reasons=(); allow_hit=()
if [ -f "$ALLOW_FILE" ]; then
    while IFS= read -r line; do
        line="${line%$'\r'}"                       # tolerate a CRLF checkout
        case "$line" in ''|'#'*) continue ;; esac
        p="${line%%#*}"; p="$(echo "$p" | tr -d '[:space:]')"
        [ -n "$p" ] || continue
        r="${line#*#}"
        allow_paths+=("$p"); allow_reasons+=("$r"); allow_hit+=(0)
    done < "$ALLOW_FILE"
else
    echo "FAIL: allow-list $ALLOW_FILE is missing -- it is committed on purpose"
    exit 1
fi

allow_index() {                                    # echoes index or -1
    local i=0
    while [ $i -lt ${#allow_paths[@]} ]; do
        [ "${allow_paths[$i]}" = "$1" ] && { echo $i; return; }
        i=$((i+1))
    done
    echo -1
}

# ------------------------------------------------------------------- corpus
# Every .as in the tree: the stdlib, the examples, and the test corpus.
corpus="$TMP/corpus.txt"
{
    ls fsroot/as/lib/*.as 2>/dev/null
    ls fsroot/as/examples/*.as 2>/dev/null
    find tests -name '*.as' -type f 2>/dev/null
} | LC_ALL=C sort -u > "$corpus"
n_corpus=$(wc -l < "$corpus" | tr -d ' ')

echo "as-crosscheck: corpus $n_corpus files (floor $MIN_CORPUS), allow-list ${#allow_paths[@]} entries"
if [ "$n_corpus" -lt "$MIN_CORPUS" ]; then
    echo "FAIL: corpus shrank to $n_corpus files (< MIN_CORPUS=$MIN_CORPUS) -- a glob stopped matching;"
    echo "      fix the glob or lower the floor ON PURPOSE, but do not let a crosscheck cover nothing."
    exit 1
fi

# --------------------------------------------------------------- reporting
# A diff nobody can act on is a diff nobody acts on. On a mismatch print, in
# order: which side is which, the first differing offset, the raw bytes around
# it from BOTH sides, and the first differing disassembly lines -- `as -dis`
# turns the offset into an instruction, which is where the work starts.
report_diff() {
    local f="$1" c="$2" s="$3"
    local cs ss off
    cs=$(wc -c < "$c" | tr -d ' '); ss=$(wc -c < "$s" | tr -d ' ')
    echo "  C-compiler  (as -c)        : $cs bytes  [< in the disasm diff below]"
    echo "  self-hosted (asc.as)       : $ss bytes  [> in the disasm diff below]"
    off=$(cmp "$c" "$s" 2>/dev/null | sed -n 's/.*differ: byte \([0-9][0-9]*\).*/\1/p')
    if [ -z "$off" ]; then
        # No differing byte -> one file is a strict prefix of the other.
        local shorter=$cs; [ "$ss" -lt "$cs" ] && shorter=$ss
        off=$((shorter+1))
        echo "  first difference: at EOF -- one output is a prefix of the other, truncated at byte $off (1-based)"
    else
        echo "  first differing byte: offset $off (1-based)"
    fi
    local start=$((off-1-16)); [ $start -lt 0 ] && start=0
    echo "  bytes [$start .. $((start+47))]"
    echo "    C   : $(od -A d -t x1 -j $start -N 48 "$c" | head -3 | tr '\n' '/')"
    echo "    SELF: $(od -A d -t x1 -j $start -N 48 "$s" | head -3 | tr '\n' '/')"
    # Disassembly is the actionable view -- it turns the offset above into an
    # instruction. But a badly-diverged .la may not load at all, and in that
    # case the diff is 900 lines of "everything", which is worse than nothing:
    # say what happened and stop at the hex.
    local disok=1
    "$ASC" -dis "$c" > "$TMP/c.dis" 2>&1 || { disok=0; echo "    (C output does not disassemble: $(head -1 "$TMP/c.dis"))"; }
    "$ASC" -dis "$s" > "$TMP/s.dis" 2>&1 || { disok=0; echo "    (SELF output does not disassemble: $(head -1 "$TMP/s.dis") -- the byte above is not a decodable opcode/operand; read the hex, not a disasm diff)"; }
    if [ $disok -eq 1 ]; then
        echo "  first disassembly differences ( < = C compiler, > = self-hosted ):"
        diff "$TMP/c.dis" "$TMP/s.dis" | head -10 | sed 's/^/    /'
    fi
    echo "  reproduce: $ASC -c $f -o /tmp/c.la"
    echo "             D=\$(mktemp -d); cp $LIBDIR/*.as tests/unit/asc_driver.as \$D/"
    echo "             (cd \$D && $ROOT/$ASC asc_driver.as $ROOT/$f /tmp/s.la); cmp -l /tmp/c.la /tmp/s.la | head"
}

# ---------------------------------------------------------------- the sweep
identical=0; diverged=0; excused=0; regressed=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    c="$TMP/c.la"; s="$TMP/s.la"
    rm -f "$c" "$s"

    "$ASC" -c "$f" -o "$c" > "$TMP/c.err" 2>&1; rcC=$?
    ( cd "$TMP" && "$ROOT/$ASC" asc_driver.as "$ROOT/$f" s.la > s.err 2>&1 ); rcS=$?

    # Classify. "Agree" means both succeeded and the bytes match; the two
    # compilers agreeing to REJECT a file is not counted as agreement, because
    # every file in this corpus is supposed to compile and a corpus that stopped
    # compiling is a finding, not a pass.
    verdict=""; detail=""
    if [ $rcC -ne 0 ] && [ $rcS -ne 0 ]; then
        verdict="BOTH-REJECT"
        detail="C: $(head -c 160 "$TMP/c.err" | tr -d '\000' | tr '\n' ' ') | SELF: $(head -c 160 "$TMP/s.err" | tr -d '\000' | tr '\n' ' ')"
    elif [ $rcC -ne 0 ]; then
        verdict="C-REJECTS-SELF-ACCEPTS"
        detail="C: $(head -c 200 "$TMP/c.err" | tr -d '\000' | tr '\n' ' ')"
    elif [ $rcS -ne 0 ]; then
        verdict="SELF-REJECTS-C-ACCEPTS"
        detail="SELF: $(head -c 200 "$TMP/s.err" | tr -d '\000' | tr '\n' ' ')"
    elif ! cmp -s "$c" "$s"; then
        verdict="BYTECODE-DIFFERS"
    fi

    idx=$(allow_index "$f")
    if [ -z "$verdict" ]; then
        if [ "$idx" -ge 0 ]; then
            allow_hit[$idx]=1
            echo "REGRESSED-TO-PASSING $f"
            echo "  this file is on the allow-list ('${allow_reasons[$idx]}') but now agrees byte for byte."
            echo "  A fix must be noticed: delete its line from $ALLOW_FILE."
            regressed=$((regressed+1))
        else
            identical=$((identical+1))
        fi
        continue
    fi

    if [ "$idx" -ge 0 ]; then
        allow_hit[$idx]=1
        echo "EXCUSED $verdict $f -- allow-listed:${allow_reasons[$idx]}"
        excused=$((excused+1))
        continue
    fi

    echo "DIVERGENCE ($verdict) $f"
    [ -n "$detail" ] && echo "  $detail"
    # A single wrong constant in one compiler diverges every file in the corpus.
    # Fifty-two full reports bury the first one, which is the one to work from,
    # so only DETAIL_MAX get the hex + disassembly; the rest are named and
    # counted. Nothing is dropped -- every diverging file still prints a line.
    if [ "$verdict" = "BYTECODE-DIFFERS" ]; then
        if [ $diverged -lt $DETAIL_MAX ]; then report_diff "$f" "$c" "$s"
        else echo "  (detail suppressed: already printed $DETAIL_MAX full reports -- fix those first)"; fi
    fi
    diverged=$((diverged+1))
done < "$corpus"

# An allow-list entry naming a file that is no longer in the corpus is dead
# weight that will outlive whoever can explain it.
stale=0
i=0
while [ $i -lt ${#allow_paths[@]} ]; do
    if [ "${allow_hit[$i]}" -eq 0 ]; then
        echo "STALE allow-list entry: ${allow_paths[$i]} is not in the corpus (moved or deleted?) -- remove the line"
        stale=$((stale+1))
    fi
    i=$((i+1))
done

echo "as-crosscheck: $identical identical, $diverged diverged, $excused excused (allow-listed), $regressed allow-listed-but-passing, $stale stale allow-list entries"
if [ $diverged -eq 0 ] && [ $regressed -eq 0 ] && [ $stale -eq 0 ]; then
    echo "as-crosscheck: PASS -- both compilers emit identical bytecode for $identical of $n_corpus files ($excused allow-listed)"
    exit 0
fi
echo "as-crosscheck: FAIL"
exit 1
