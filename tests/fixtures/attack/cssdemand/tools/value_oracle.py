#!/usr/bin/env python3
"""value_oracle.py -- LibCSS's verdict for every DISTINCT (property, value)
the corpus declares (census/values-<site>.jsonl), batched through dropdump
with a z-index sentinel after every probe so a SILENT drop (no report at all:
language.c's css__parse_important path) is detectable by alignment:
   #pN{prop:value}  #sN{z-index:N}
Between two z-index reports there is exactly one DECL for `prop` (its reason)
or none (SILENT). Batches whose sentinel sequence breaks are bisected.
Verdicts: ACCEPTED, UNKNOWN-PROP, BAD-VALUE, TRAILING, SILENT."""
import sys, os, json, subprocess, tempfile, collections
DD, cdir, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
NAMES = {0: "UNKNOWN-PROP", 1: "BAD-VALUE", 2: "TRAILING", 3: "ACCEPTED"}
def run(sheet):
    fd, path = tempfile.mkstemp(suffix=".css", dir=out); os.write(fd, sheet.encode("utf-8", "replace")); os.close(fd)
    o = subprocess.run([DD, path], capture_output=True, text=True, errors="replace").stdout
    os.unlink(path)
    return [l.split("\t") for l in o.splitlines() if not l.startswith("SRC")]
def probe(pairs):
    if not pairs: return []
    sheet = "".join("#p%d{%s:%s}\n#s%d{z-index:%d}\n" % (i, p, v, i, i) for i, (p, v) in enumerate(pairs))
    lines = run(sheet)
    res = []; i = 0; cur = None; ok = True
    for l in lines:
        if l[0] == "DECL" and l[1] == "z-index" and l[2] == "3":
            res.append(cur if cur is not None else "SILENT"); cur = None
        elif l[0] == "DECL":
            if cur is not None or l[1].lower() != pairs[len(res)][0].lower() if len(res) < len(pairs) else True:
                ok = False; break
            cur = NAMES.get(int(l[2]), "?")
        else:
            ok = False; break
    if ok and len(res) == len(pairs): return res
    if len(pairs) == 1: return ["AMBIGUOUS"]
    h = len(pairs) // 2
    return probe(pairs[:h]) + probe(pairs[h:])
cache = {}
for fn in sorted(os.listdir(cdir)):
    if not fn.startswith("values-") or not fn.endswith(".jsonl"): continue
    site = fn[7:-6]
    rows = [json.loads(l) for l in open(os.path.join(cdir, fn))]
    todo = [(p, v) for p, v, n in rows if (p, v) not in cache and "{" not in v and "}" not in v and ";" not in v and len(v) < 2000]
    B = 500
    for i in range(0, len(todo), B):
        chunk = todo[i:i+B]
        for pv, r in zip(chunk, probe(chunk)): cache[pv] = r
    per = collections.defaultdict(lambda: collections.Counter())   # prop -> verdict -> decl count
    silent_examples = collections.defaultdict(list)
    for p, v, n in rows:
        r = cache.get((p, v), "UNPROBED")
        per[p][r] += n
        if r == "SILENT" and len(silent_examples[p]) < 8: silent_examples[p].append(v[:80])
    json.dump({"site": site, "per_prop": {k: dict(v) for k, v in per.items()}, "silent_examples": silent_examples},
              open(os.path.join(out, "values-%s.json" % site), "w"), indent=0)
    tot = collections.Counter()
    for k, v in per.items(): tot.update(v)
    print("%-13s distinct=%-6d %s" % (site, len(rows), dict(tot)), flush=True)
json.dump({"%s\t%s" % k: v for k, v in cache.items()}, open(os.path.join(out, "value_cache.json"), "w"))
