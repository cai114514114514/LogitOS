#!/usr/bin/env python3
"""Chrome as the ORACLE for 'how many rules does this source contain, and how
many of them does a real browser accept': every source (external sheet / inline
<style> block) is placed verbatim into its own <style> of one local HTML file
per site; headless Chrome parses it and a script counts, per <style>,
accepted style rules (recursively through group rules) and then dumps the DOM.
No network: file://, and virtual time so the run ends. Chrome's count vs
tinycss2's count of syntactic rules is the apparatus check on the census;
Chrome's count vs LibCSS's accepted count is the engine's extra loss."""
import os, re, sys, json, subprocess, html
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
corpus, out = sys.argv[1], sys.argv[2]
STYLE_RE = re.compile(r"<style\b[^>]*>(.*?)</style", re.I | re.S)
JS = r"""
function walk(rules, acc){ for (const r of rules){ if (r.type===1) acc.style++; else if (r.type===7) acc.kf++; else acc.other++;
  if (r.cssRules) walk(r.cssRules, acc); } }
const res=[]; for (const s of document.styleSheets){ const acc={style:0,kf:0,other:0}; try{ walk(s.cssRules, acc);}catch(e){acc.err=String(e);} res.push(acc); }
document.title = JSON.stringify(res);
"""
results = {}
for site in sorted(os.listdir(corpus)):
    d = os.path.join(corpus, site)
    if not os.path.isdir(d): continue
    if "\nREFUSED" in open(os.path.join(d, "MANIFEST"), errors="replace").read(): continue
    parts = []; names = []
    for fn in sorted(os.listdir(d)):
        p = os.path.join(d, fn)
        if fn.endswith(".css"):
            parts.append(open(p, encoding="utf-8", errors="replace").read()); names.append((fn, 0))
        elif fn.endswith(".html"):
            h = open(p, encoding="utf-8", errors="replace").read()
            for i, m in enumerate(STYLE_RE.finditer(h)):
                parts.append(m.group(1)); names.append((fn, i))
    if not parts: results[site] = {"sources": [], "counts": []}; continue
    body = "".join("<style>%s</style>\n" % t.replace("</style", "<\\/style") for t in parts)
    page = "<!doctype html><html><head><meta charset=utf-8><title>x</title></head><body>%s<script>%s</script></body></html>" % (body, JS)
    hp = os.path.join(out, site + ".html"); open(hp, "w").write(page)
    r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--virtual-time-budget=8000",
                        "--disable-extensions", "--dump-dom", "file://" + os.path.abspath(hp)], capture_output=True, text=True, timeout=180)
    m = re.search(r"<title>(.*?)</title>", r.stdout, re.S)
    counts = json.loads(html.unescape(m.group(1))) if m and m.group(1).startswith("[") else None
    results[site] = {"sources": names, "counts": counts, "stderr": r.stderr[-300:] if counts is None else ""}
    print(site, "sources", len(parts), "chrome style rules", sum(c["style"] for c in counts) if counts else "FAILED", flush=True)
json.dump(results, open(os.path.join(out, "chrome_counts.json"), "w"), indent=0)
