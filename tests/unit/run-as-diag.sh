#!/usr/bin/env bash
# DG gate: DIAGNOSTICS PARITY BETWEEN THE TWO AETHERSCRIPT COMPILERS.
#
# tests/unit/run-as-crosscheck.sh proves the two compilers
#
#     the C compiler        c/apps/as/compiler.c + lexer.c
#     the self-hosted one   fsroot/as/lib/asc.as + aslex.as
#
# emit byte-identical bytecode for every .as file in the tree. Every file in
# that corpus COMPILES. So the crosscheck has never looked at the other half of
# a compiler: what it says when the program is wrong.
#
# That matters right now and not in the abstract. The plan of record replaces
# /bin/as's compiler with the self-hosted one. On the day that lands, a user who
# forgets a colon stops reading compiler.c's message and starts reading asc.as's
# -- and if that message is worse, the change is a downgrade that the crosscheck
# cannot see, because a broken program produces no bytecode to diff.
#
# So: a corpus of BROKEN AetherScript, one file per error class, both compilers
# run on all of it, and three things scored per side that a machine can check.
#
#   (a) DIAG  did it report an error at all -- non-zero exit, a message, and for
#             the self-hosted side a message it RAISED ON PURPOSE. That last
#             clause is the one with teeth and it is checkable: asc.as reports
#             through Parser.err / Lexer.err, so the innermost frame of the VM
#             traceback names `asc.err` or `aslex.err`. Any other innermost
#             frame means the compiler fell over instead of diagnosing -- which
#             is exactly what 200 nested parentheses used to do here, dying with
#             "call depth exceeded" from inside asc.grouping, no line, nothing
#             about the user's program.
#   (b) LINE  does the message name a line, and is it the line the mistake is
#             ON. The expected line is declared by hand in CLASSES.txt from
#             reading the source, NOT harvested from what the compilers print --
#             see the header of that file for why that distinction is the whole
#             value of the column.
#   (c) WANT  does the message name what was expected -- checked as a substring
#             declared per class, so "expected ':' after the condition" scores
#             and "syntax error" would not.
#
# WHAT FAILS AND WHAT ONLY REPORTS.
#   The gate FAILS on (a): a self-hosted compiler that accepts a broken program,
#   or dies without diagnosing one, is a bug and not a style preference. It also
#   fails if the self-hosted compiler REJECTS a program the C one accepts and
#   that is not a declared class -- that direction is how a self-hosting switch
#   breaks somebody's working script.
#   (b) and (c) are SCORED against a committed per-class baseline
#   (tests/unit/as-diag-baseline.txt). A row going backwards fails. A row going
#   forwards ALSO fails, with an instruction to update the baseline -- same rule
#   the crosscheck's allow-list uses, and for the same reason: an unrecorded
#   improvement is an improvement nobody can prove happened, and the gap is only
#   monotone if both directions are noticed.
#
# LIMITS, stated rather than implied:
#   * This measures the FIRST error only. Neither compiler recovers and
#     continues, so "reports 1 of 3 mistakes" is not a thing either side can be
#     scored on today.
#   * Rows marked EXPECT=accept are runtime classes. Both compilers hand their
#     bytecode to the same C VM, so those rows cannot discriminate between the
#     two compilers at all; they are here to prove the class is runtime-resolved
#     and that neither compiler rejects a legal program. Counted and labelled
#     separately, never folded into the parity score.
#   * A message can score 3/3 and still be unhelpful. (a)/(b)/(c) are the parts
#     a machine can check; they are a floor, not a definition of a good message.
#   * Both compilers run on the HOST here. Nothing about this gate is
#     device-specific -- it is the same `build/asc` binary providing both sides.
#
# usage: run-as-diag.sh <asc> [libdir] [baseline]
#   <asc>       host `as` binary (build/asc). `-c` is the C compiler; the same
#               binary is also the VM that interprets asc.as.
#   [libdir]    where to take the self-hosted compiler from. Defaults to
#               fsroot/as/lib; the negative control passes a perturbed copy.
#   [baseline]  defaults to tests/unit/as-diag-baseline.txt.
set -u

ASC="${1:?usage: run-as-diag.sh <asc> [libdir] [baseline]}"
LIBDIR="${2:-fsroot/as/lib}"
BASELINE="${3:-tests/unit/as-diag-baseline.txt}"
CORPUS_DIR="tests/unit/asdiag"
MANIFEST="$CORPUS_DIR/CLASSES.txt"
ROOT="$PWD"

# A diagnostics gate that silently covered four classes would report a green
# line and mean nothing. Floor, not an expected value: raise it when the corpus
# grows, and only lower it on purpose.
MIN_CLASSES=40

[ -x "$ASC" ] || { echo "FAIL: $ASC is not executable"; exit 1; }
[ -f "$LIBDIR/asc.as" ] || { echo "FAIL: no asc.as in $LIBDIR"; exit 1; }
[ -f "$MANIFEST" ] || { echo "FAIL: no manifest at $MANIFEST"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src"

# The self-hosted compiler runs as a program and imports its own modules from
# the cwd. Same mechanism run-as-crosscheck.sh and run-selfhost-compile.sh use.
cp "$LIBDIR"/*.as "$TMP/" || exit 1
cp tests/unit/asc_driver.as "$TMP/" || exit 1

# ------------------------------------------------------------ generated cases
# Four classes are bulk by nature -- 65 indent levels, 200 nested parens, 20
# nested loops, 100 breaks, 300 locals. Committing a 300-line file of `v0 = 0`
# would be noise, so those are built here from a recipe. Everything else is a
# hand-written file in $CORPUS_DIR, minimal and readable.
#
# gen_expect_out: for the impl-limit rows the self-hosted compiler ACCEPTS what
# the C one rejects, and "accepts" is only good news if the emitted code is
# right -- so those two also declare the output their program must print, and
# the runner runs the .la and checks it.
# Sets the globals gen_path and gen_expect_out. NOT called through $(...):
# gen_expect_out has to survive back into the caller, and a command
# substitution is a subshell.
gen_case() {
    local name="$1"
    local out="$TMP/src/$name.as"          # separate statement: bash expands a
    gen_expect_out=""; gen_path="$out"     # whole `local` line before assigning
    case "$name" in
    too-many-indent-levels)
        # Line k opens nesting level k, so the level that overflows a 64-entry
        # indent stack is on line 65.
        : > "$out"
        local k i pad
        for k in $(seq 0 69); do
            pad=""; i=0; while [ $i -lt $k ]; do pad="$pad    "; i=$((i+1)); done
            printf '%sif 1:\n' "$pad" >> "$out"
        done
        pad=""; i=0; while [ $i -lt 70 ]; do pad="$pad    "; i=$((i+1)); done
        printf '%sx = 1\n' "$pad" >> "$out" ;;
    expression-nested-too-deep)
        { printf 'x = '
          local k
          for k in $(seq 1 200); do printf '('; done
          printf '1'
          for k in $(seq 1 200); do printf ')'; done
          printf '\n'; } > "$out" ;;
    loops-nested-too-deep)
        # `n = 0` is line 1, so the 17th loop -- the one past C's 16-entry fixed
        # loop stack -- is on line 18. Runnable: 19 x range(1) and one range(3).
        { printf 'n = 0\n'
          local k i pad
          for k in $(seq 0 19); do
              pad=""; i=0; while [ $i -lt $k ]; do pad="$pad    "; i=$((i+1)); done
              if [ "$k" -eq 19 ]; then printf '%sfor i%d in range(3):\n' "$pad" "$k"
              else printf '%sfor i%d in range(1):\n' "$pad" "$k"; fi
          done
          pad=""; i=0; while [ $i -lt 20 ]; do pad="$pad    "; i=$((i+1)); done
          printf '%sn = n + 1\n' "$pad"
          printf 'print(n)\n'; } > "$out"
        gen_expect_out="3" ;;
    too-many-breaks-in-loop)
        # break #k is on line 3 + 2k, so the 65th -- one past C's 64-entry fixed
        # break-patch list -- is on line 133. Runnable: the first break wins.
        { printf 'n = 0\nwhile 1:\n    n = n + 1\n'
          local k
          for k in $(seq 1 100); do printf '    if n == %d:\n        break\n' "$k"; done
          printf 'print(n)\n'; } > "$out"
        gen_expect_out="1" ;;
    crlf-line-endings)
        # A VALID program, saved with CRLF. This one is GENERATED rather than
        # committed for the reason it is testing: core.autocrlf is true in this
        # repository, so a committed CRLF file is not reliably a CRLF file after
        # a checkout -- and a committed LF one is not reliably LF either, which
        # is why the copy step below strips CR from every other class. See the
        # header of .gitattributes; this is the same class of mistake it already
        # documents for the WPT and html5lib corpora. Built here, the bytes are
        # the runner's and no git configuration can move them.
        printf 'x = 1\r\nif x == 1:\r\n    print(x)\r\n' > "$out" ;;
    too-many-locals)
        # Slot 0 is reserved for the function itself, so v255 -- on line 257 --
        # is the 257th local and the one that overflows a 256-slot table.
        { printf 'def f():\n'
          local k
          for k in $(seq 0 299); do printf '    v%d = %d\n' "$k" "$k"; done
          printf '    return v0\n'; } > "$out" ;;
    *)
        echo "FAIL: manifest names gen:$name but the runner has no recipe for it"; exit 1 ;;
    esac
}

# ------------------------------------------------------------------- baseline
# path -> "diag line want". bash 3.2 on the macOS host has no associative
# arrays, so two parallel arrays.
bl_names=(); bl_vals=(); bl_hit=()
if [ -f "$BASELINE" ]; then
    while IFS= read -r line; do
        line="${line%$'\r'}"
        case "$line" in ''|'#'*) continue ;; esac
        bl_names+=("${line%%|*}")
        bl_vals+=("${line#*|}")
        bl_hit+=(0)
    done < "$BASELINE"
else
    echo "NOTE: no baseline at $BASELINE -- scoring will run, and the gate will"
    echo "      refuse at the end and print the file to commit."
fi
bl_index() {
    local i=0
    while [ $i -lt ${#bl_names[@]} ]; do
        [ "${bl_names[$i]}" = "$1" ] && { echo $i; return; }
        i=$((i+1))
    done
    echo -1
}

# --------------------------------------------------------------- one class
# Fills the globals below. Kept as globals rather than a parsed echo because the
# messages contain every punctuation character bash cares about.
run_side_c() {          # $1 = source path -> c_rc, c_msg
    local out
    out="$("$ASC" -c "$1" -o "$TMP/c.la" 2>&1 < /dev/null)"; c_rc=$?
    c_msg="$(printf '%s' "$out" | head -1)"
    c_msg="${c_msg#as: }"
}
run_side_self() {       # $1 = source path -> s_rc, s_msg, s_frame
    local out
    rm -f "$TMP/s.la"
    out="$( cd "$TMP" && "$ROOT/$ASC" asc_driver.as "$1" "$TMP/s.la" 2>&1 < /dev/null )"; s_rc=$?
    s_msg="$(printf '%s' "$out" | head -1)"
    s_msg="${s_msg#as: }"
    # Innermost traceback frame, i.e. where the raise came from. Absent when the
    # compile succeeded or when the VM printed a bare message.
    s_frame="$(printf '%s' "$out" | sed -n 's/^  in \([A-Za-z_][A-Za-z_.0-9<>]*\).*/\1/p' | head -1)"
}

score_line() {          # $1 = message, $2 = expected line -> "yes"|"wrong:N"|"no"
    local got
    got="$(printf '%s' "$1" | sed -n 's/.*(line \([0-9][0-9]*\)).*/\1/p')"
    if [ -z "$got" ]; then echo "no"
    elif [ "$got" = "$2" ]; then echo "yes"
    else echo "wrong:$got"; fi
}
score_want() {          # $1 = message, $2 = wanted substring
    case "$1" in *"$2"*) echo "yes" ;; *) echo "no" ;; esac
}

# ------------------------------------------------------------------- the sweep
n_class=0; hard_fail=0; regressed=0; improved=0; missing_bl=0
c_diag=0; c_line=0; c_want=0
s_diag=0; s_line=0; s_want=0
n_scored=0; n_accept=0; n_impl=0; n_parity_same=0; n_parity_diff=0
newbl="$TMP/baseline.new"
: > "$newbl"

printf '%-32s %-10s %-4s %-5s %-4s %-5s %s\n' CLASS EXPECT C:a C:b/c S:a S:b/c PARITY
printf '%s\n' "--------------------------------------------------------------------------------"

while IFS='|' read -r src expect eline want desc; do
    case "$src" in ''|'#'*|'SRC') continue ;; esac
    src="${src%$'\r'}"; desc="${desc%$'\r'}"
    n_class=$((n_class+1))

    gen_expect_out=""
    if [ "${src#gen:}" != "$src" ]; then
        cls="${src#gen:}"
        gen_case "$cls" || exit 1
        path="$gen_path"
    else
        cls="${src%.as.bad}"
        [ -f "$CORPUS_DIR/$src" ] || { echo "FAIL: manifest names $src, which is not in $CORPUS_DIR"; exit 1; }
        # Copied to a .as name: the .as.bad extension exists only to keep these
        # out of run-as-crosscheck.sh's `find tests -name '*.as'`.
        #
        # CR is stripped on the way in. core.autocrlf is true here, so a
        # checkout can hand these files back with CRLF -- and a CR is a lex
        # error in this language, so EVERY class would then report "unexpected
        # character" instead of the error it was written for and the whole gate
        # would fail for a reason that has nothing to do with either compiler.
        # .gitattributes carries `tests/unit/asdiag/** -text` as the real fix;
        # this line means the gate is still right for a checkout that does not.
        # The one class that IS about CRLF is generated above, not read here.
        path="$TMP/src/$cls.as"
        tr -d '\r' < "$CORPUS_DIR/$src" > "$path"
    fi

    run_side_c   "$path"
    run_side_self "$path"

    # ---- (a) DIAG
    cA="no"; sA="no"
    [ $c_rc -ne 0 ] && [ -n "$c_msg" ] && cA="yes"
    if [ $s_rc -ne 0 ] && [ -n "$s_msg" ]; then
        # A deliberate diagnostic is raised by Parser.err / Lexer.err or their
        # err_at variants (the ones that report a caller-chosen line). Anything
        # else as the innermost frame -- asc.grouping, asc.i64 -- means the
        # compiler fell over. `''` is the no-traceback case: a bare message.
        case "$s_frame" in
            asc.err|asc.err_at|aslex.err|aslex.err_at|asc.compile_file|'') sA="yes" ;;
            *) sA="CRASH" ;;
        esac
    fi

    if [ "$expect" = "accept" ]; then
        n_accept=$((n_accept+1))
        cB="-"; cC="-"; sB="-"; sC="-"
        if [ $s_rc -ne 0 ]; then
            echo "HARD FAIL [$cls]: self-hosted compiler REJECTS a program the class declares legal"
            echo "    SELF: $s_msg"
            hard_fail=$((hard_fail+1))
        elif [ $c_rc -ne 0 ]; then
            echo "HARD FAIL [$cls]: C compiler rejects it but the self-hosted one accepts -- the manifest says accept"
            echo "    C: $c_msg"
            hard_fail=$((hard_fail+1))
        fi
        sA="n/a"; cA="n/a"
    else
        cB="$(score_line "$c_msg" "$eline")"; cC="$(score_want "$c_msg" "$want")"
        sB="$(score_line "$s_msg" "$eline")"; sC="$(score_want "$s_msg" "$want")"
        if [ "$expect" = "impl-limit" ]; then
            n_impl=$((n_impl+1))
            if [ "$sA" = "CRASH" ]; then
                echo "HARD FAIL [$cls]: self-hosted compiler CRASHED (innermost frame $s_frame) -- $s_msg"
                hard_fail=$((hard_fail+1))
            fi
            # "Accepts what C rejects" is only good news if the code is right.
            if [ $s_rc -eq 0 ] && [ -n "${gen_expect_out:-}" ]; then
                got="$("$ASC" -run "$TMP/s.la" 2>&1 | head -1)"
                if [ "$got" != "$gen_expect_out" ]; then
                    echo "HARD FAIL [$cls]: self-hosted compiler accepted past C's fixed limit and emitted WRONG code"
                    echo "    expected stdout '$gen_expect_out', got '$got'"
                    hard_fail=$((hard_fail+1))
                fi
            fi
        else
            n_scored=$((n_scored+1))
            [ "$cA" = "yes" ] && c_diag=$((c_diag+1))
            [ "$cB" = "yes" ] && c_line=$((c_line+1))
            [ "$cC" = "yes" ] && c_want=$((c_want+1))
            [ "$sA" = "yes" ] && s_diag=$((s_diag+1))
            [ "$sB" = "yes" ] && s_line=$((s_line+1))
            [ "$sC" = "yes" ] && s_want=$((s_want+1))
            if [ "$sA" != "yes" ]; then
                if [ $s_rc -eq 0 ]; then
                    echo "HARD FAIL [$cls]: self-hosted compiler ACCEPTED a broken program (exit 0)"
                elif [ "$sA" = "CRASH" ]; then
                    echo "HARD FAIL [$cls]: self-hosted compiler CRASHED instead of diagnosing"
                    echo "    innermost frame: $s_frame   message: $s_msg"
                    echo "    a message raised anywhere but Parser.err/Lexer.err is about the compiler, not the program"
                else
                    echo "HARD FAIL [$cls]: self-hosted compiler exited $s_rc with no message"
                fi
                hard_fail=$((hard_fail+1))
            fi
        fi
    fi

    if [ "$c_msg" = "$s_msg" ]; then par="same"; n_parity_same=$((n_parity_same+1))
    else par="DIFFERS"; n_parity_diff=$((n_parity_diff+1)); fi

    printf '%-32s %-10s %-4s %-5s %-4s %-5s %s\n' \
        "$cls" "$expect" "$cA" "$cB/$cC" "$sA" "$sB/$sC" "$par"
    if [ "$par" = "DIFFERS" ]; then
        printf '    C   : %s\n    SELF: %s\n' "${c_msg:-<none, exit $c_rc>}" "${s_msg:-<none, exit $s_rc>}"
    fi

    # ---- baseline row (self side only: it is the one that ships)
    row="$sA $sB $sC"
    printf '%s|%s\n' "$cls" "$row" >> "$newbl"
    idx=$(bl_index "$cls")
    if [ "$idx" -lt 0 ]; then
        echo "    NEW CLASS [$cls] -- not in $BASELINE; add the row printed at the end"
        missing_bl=$((missing_bl+1))
    else
        bl_hit[$idx]=1
        if [ "${bl_vals[$idx]}" != "$row" ]; then
            # Direction: count the yes-es. More is better, and "wrong:N" is not
            # a yes -- a confidently wrong line number is not progress.
            oldn=$(printf '%s' "${bl_vals[$idx]}" | tr ' ' '\n' | grep -c '^yes$')
            newn=$(printf '%s' "$row" | tr ' ' '\n' | grep -c '^yes$')
            if [ "$newn" -lt "$oldn" ]; then
                echo "    REGRESSION [$cls]: baseline '${bl_vals[$idx]}' -> now '$row'"
                regressed=$((regressed+1))
            else
                echo "    IMPROVED [$cls]: baseline '${bl_vals[$idx]}' -> now '$row' (update the baseline)"
                improved=$((improved+1))
            fi
        fi
    fi
done < "$MANIFEST"

stale=0; i=0
while [ $i -lt ${#bl_names[@]} ]; do
    if [ "${bl_hit[$i]}" -eq 0 ]; then
        echo "STALE baseline row: ${bl_names[$i]} is no longer a class in $MANIFEST -- remove the line"
        stale=$((stale+1))
    fi
    i=$((i+1))
done

echo
echo "as-diag: $n_class classes ($n_scored scored, $n_impl impl-limit, $n_accept runtime/accept), floor $MIN_CLASSES"
echo "as-diag: message parity  $n_parity_same identical / $n_parity_diff differ"
printf 'as-diag: C compiler   (a) %d/%d   (b) names the right line %d/%d   (c) names what was expected %d/%d\n' \
    "$c_diag" "$n_scored" "$c_line" "$n_scored" "$c_want" "$n_scored"
printf 'as-diag: self-hosted  (a) %d/%d   (b) names the right line %d/%d   (c) names what was expected %d/%d\n' \
    "$s_diag" "$n_scored" "$s_line" "$n_scored" "$s_want" "$n_scored"

rc=0
if [ "$n_class" -lt "$MIN_CLASSES" ]; then
    echo "FAIL: corpus shrank to $n_class classes (< MIN_CLASSES=$MIN_CLASSES) -- a row was dropped;"
    echo "      lower the floor ON PURPOSE, but do not let a diagnostics gate cover nothing."
    rc=1
fi
[ "$hard_fail" -gt 0 ] && { echo "FAIL: $hard_fail hard failure(s) -- see HARD FAIL above"; rc=1; }
[ "$regressed" -gt 0 ] && { echo "FAIL: $regressed class(es) regressed against $BASELINE"; rc=1; }
[ "$improved" -gt 0 ] && { echo "FAIL: $improved class(es) IMPROVED -- record it, so the gap stays monotone"; rc=1; }
[ "$missing_bl" -gt 0 ] && { echo "FAIL: $missing_bl class(es) have no baseline row"; rc=1; }
[ "$stale" -gt 0 ] && { echo "FAIL: $stale stale baseline row(s)"; rc=1; }

if [ $rc -ne 0 ]; then
    echo
    echo "----- baseline as measured; if these numbers are the intended ones, this is the file -----"
    echo "# tests/unit/as-diag-baseline.txt -- see the header of tests/unit/run-as-diag.sh."
    echo "# class | (a) diagnosed | (b) names the declared line | (c) names what was expected"
    echo "# Self-hosted compiler only: it is the one that ships. Regenerate by running the gate."
    cat "$newbl"
    echo "-----------------------------------------------------------------------------------------"
    echo "as-diag: FAIL"
    exit 1
fi
echo "as-diag: PASS -- $n_scored scored classes all diagnosed, no baseline movement"
exit 0
