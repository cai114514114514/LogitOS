#!/usr/bin/env python3
"""rule_oracle.py -- ask the tree's LibCSS, rule by rule, whether it accepts
each style rule's selector list: every rule in census/rules-<site>.jsonl is
re-synthesised at TOP LEVEL as `<prelude>{color:red}` (so the refusal is
REPORTED -- inside @media/@layer LibCSS refuses silently; measured), batched,
and aligned by count; a batch whose report count disagrees is bisected down
to single rules. Nested rules (CSS nesting) never reach LibCSS as rules and
are counted LOST outright. Output per site: <out>/oracle-<site>.json."""
import sys, os, json, subprocess, tempfile, collections, re
DD, cdir, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
def run(sheet):
    fd, path = tempfile.mkstemp(suffix=".css", dir=out); os.write(fd, sheet.encode("utf-8", "replace")); os.close(fd)
    o = subprocess.run([DD, path], capture_output=True, text=True, errors="replace").stdout
    os.unlink(path)
    return [l.split("\t") for l in o.splitlines() if not l.startswith("SRC")]
def verdicts(sels):
    """list of 'ok' | 'refused' for each prelude, aligned."""
    if not sels: return []
    sheet = "".join("%s{color:red}\n" % s for s in sels)
    lines = run(sheet)
    if len(lines) == len(sels) and all(l[0] in ("SELDROP", "DECL") for l in lines):
        return ["refused" if l[0] == "SELDROP" else "ok" for l in lines]
    if len(sels) == 1:
        # ambiguous single: more than one report (e.g. prelude containing '{'?) -> call it refused-ambiguous
        return ["ambiguous"]
    h = len(sels) // 2
    return verdicts(sels[:h]) + verdicts(sels[h:])
for fn in sorted(os.listdir(cdir)):
    if not fn.startswith("rules-") or not fn.endswith(".jsonl"): continue
    site = fn[6:-6]
    recs = [json.loads(l) for l in open(os.path.join(cdir, fn))]
    top = [r for r in recs if not r["nested"]]
    sels = [r["sel"].replace("\n", " ") for r in top]
    v = []
    B = 400
    for i in range(0, len(sels), B): v += verdicts(sels[i:i+B])
    assert len(v) == len(top)
    for r, x in zip(top, v): r["verdict"] = x
    for r in recs:
        if r["nested"]: r["verdict"] = "nested"
    tot_r = len(recs); tot_d = sum(r["nd"] for r in recs)
    by = collections.defaultdict(lambda: [0, 0])
    unattributed = []
    for r in recs:
        if r["verdict"] == "ok": continue
        if r["verdict"] == "nested": key = "CSS nesting (rule nested in a style rule)"
        elif r["verdict"] == "ambiguous": key = "AMBIGUOUS"
        else: key = r["reasons"][0] if r["reasons"] else "UNATTRIBUTED"
        by[key][0] += 1; by[key][1] += r["nd"]
        if key == "UNATTRIBUTED" and len(unattributed) < 30: unattributed.append(r["sel"][:150])
    # rules the model called refused but LibCSS accepted
    model_only = collections.Counter(r["reasons"][0] for r in recs if r["verdict"] == "ok" and r["reasons"])
    lost_r = sum(x[0] for x in by.values()); lost_d = sum(x[1] for x in by.values())
    res = {"site": site, "rules": tot_r, "decls": tot_d, "lost_rules": lost_r, "lost_decls": lost_d,
           "by_reason": {k: v for k, v in sorted(by.items(), key=lambda kv: -kv[1][1])},
           "model_only": dict(model_only), "unattributed_examples": unattributed}
    json.dump(res, open(os.path.join(out, "oracle-%s.json" % site), "w"), indent=1)
    json.dump(recs, open(os.path.join(out, "verdicts-%s.json" % site), "w"))
    print("%-13s rules=%-6d lost=%-5d (%.2f%%)  decls=%-7d lost=%-6d (%.2f%%)  unattributed=%d model-only=%d" % (
        site, tot_r, lost_r, 100.0 * lost_r / max(tot_r, 1), tot_d, lost_d, 100.0 * lost_d / max(tot_d, 1),
        by.get("UNATTRIBUTED", [0, 0])[0], sum(model_only.values())), flush=True)
