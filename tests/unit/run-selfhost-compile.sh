#!/usr/bin/env bash
# M21-P3 S2/S3 gate: programs compiled by the SELF-HOSTED compiler (lib/asc.as)
# must produce byte-identical runtime output to direct interpretation.
set -u
ASC="${1:?usage: run-selfhost-compile.sh <asc>}"
ROOT="$PWD"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp fsroot/as/lib/*.as tests/unit/asc_driver.as "$TMP/"

# extra M23-feature snippet (beyond the real example programs)
cat > "$TMP/m23.as" <<'SNIP'
a, b = 1, 2
a, b = b, a
n = 7
print(f"swap={a}{b} tern={'even' if n % 2 == 0 else 'odd'}")
print(",".join([str(x * x) for x in range(5) if x != 2]))
print("  Mixed Up  ".strip().lower().replace(" ", "_"))
d = {"k": [1, 2, 3]}
print(d["k"][1] if "k" in d else -1, len(d))
try:
    raise "boom"
except e:
    print("caught:", e)
class A:
    def init(self):
        self.v = 10
    def add(self, k):
        self.v += k
        return self.v
class B(A):
    def add(self, k):
        return super.add(k * 2)
print(B().add(5))
SNIP

# Examples whose DIRECT interpretation is expected to end in an error, and
# therefore cannot be compared against a self-hosted compile of the same source.
# Space-separated names, no extension.
#
# WHY THIS LIST HAS TO EXIST. The loop below skips any example whose direct run
# fails, and until now it skipped SILENTLY: it printed a line and did not count
# it, so the gate's own criterion (fail == 0 && pass >= 8) still held. That is
# fine while every example is supposed to succeed and a skip means "something
# broke, look at it" -- and it becomes a hole the moment an example is supposed
# to error, which M28 introduces: a script proving it CANNOT read /etc without
# CAP_FS exits non-zero by design. Silently dropped, it would read as a pass
# while testing nothing.
#
# So an unexpected skip is now a FAILURE, and a skip has to be declared here by
# name. Declaring one is a decision someone makes on purpose; forgetting to is
# no longer free.
EXPECT_ERROR=""

pass=0; fail=0; skipped=0
for name in hello fib dict closure classes exc use_mod strings m23; do
    f="$TMP/$name.as"
    [ -f "$f" ] || cp "$ROOT/fsroot/as/examples/$name.as" "$f"
    ( cd "$TMP" && "$ROOT/$ASC" "$name.as" > direct.out 2>&1 )
    rcA=$?
    ( cd "$TMP" && "$ROOT/$ASC" asc_driver.as "$name.as" "$name.la" > compile.err 2>&1 \
        && "$ROOT/$ASC" -run "$name.la" > selfla.out 2>&1 )
    rcB=$?
    if [ $rcA -ne 0 ]; then
        case " $EXPECT_ERROR " in
            *" $name "*)
                echo "SKIP (errors by design, declared in EXPECT_ERROR) $name"
                skipped=$((skipped+1)); continue ;;
            *)
                echo "FAIL (direct run failed, and $name is not in EXPECT_ERROR) $name: $(tail -2 "$TMP/direct.out" 2>/dev/null | tr '\n' ' ')"
                fail=$((fail+1)); continue ;;
        esac
    fi
    if [ $rcB -ne 0 ]; then echo "FAIL (selfhost) $name: $(tail -2 "$TMP/compile.err" "$TMP/selfla.out" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1)); continue; fi
    if cmp -s "$TMP/direct.out" "$TMP/selfla.out"; then pass=$((pass+1));
    else echo "FAIL (output mismatch) $name"; diff "$TMP/direct.out" "$TMP/selfla.out" | head -6; fail=$((fail+1)); fi
done
echo "selfhost-compile: $pass identical, $fail failed, $skipped declared-error"
[ "$fail" -eq 0 ] && [ "$pass" -ge 8 ]
