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

pass=0; fail=0
for name in hello fib dict closure classes exc use_mod strings m23; do
    f="$TMP/$name.as"
    [ -f "$f" ] || cp "$ROOT/fsroot/as/examples/$name.as" "$f"
    ( cd "$TMP" && "$ROOT/$ASC" "$name.as" > direct.out 2>&1 )
    rcA=$?
    ( cd "$TMP" && "$ROOT/$ASC" asc_driver.as "$name.as" "$name.la" > compile.err 2>&1 \
        && "$ROOT/$ASC" -run "$name.la" > selfla.out 2>&1 )
    rcB=$?
    if [ $rcA -ne 0 ]; then echo "SKIP (direct fails) $name"; continue; fi
    if [ $rcB -ne 0 ]; then echo "FAIL (selfhost) $name: $(tail -2 "$TMP/compile.err" "$TMP/selfla.out" 2>/dev/null | tr '\n' ' ')"; fail=$((fail+1)); continue; fi
    if cmp -s "$TMP/direct.out" "$TMP/selfla.out"; then pass=$((pass+1));
    else echo "FAIL (output mismatch) $name"; diff "$TMP/direct.out" "$TMP/selfla.out" | head -6; fail=$((fail+1)); fi
done
echo "selfhost-compile: $pass identical, $fail failed"
[ "$fail" -eq 0 ] && [ "$pass" -ge 8 ]
