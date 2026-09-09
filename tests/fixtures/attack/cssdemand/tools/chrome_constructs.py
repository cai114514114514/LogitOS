#!/usr/bin/env python3
"""Which of the synthetic selector/at-rule constructs does CHROME accept? One
<style> per construct; cssRules.length (recursive) == 0 means Chrome refused it.
The oracle for 'a rule LibCSS refuses that a real browser would have applied'."""
import re, sys, json, subprocess, html, os
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
out = sys.argv[1]
cases = [l.rstrip("\n") for l in open(sys.argv[2]) if l.strip()]
JS = r"""
function cnt(rules){ let n=0; for (const r of rules){ if (r.type===1) n++; if (r.cssRules) n+=cnt(r.cssRules);} return n; }
const res=[]; for (const s of document.styleSheets){ try{ res.push(cnt(s.cssRules)); }catch(e){ res.push(-1);} }
document.title = JSON.stringify(res);
"""
body = "".join("<style>%s</style>\n" % (c if c.startswith("@") else c + "{color:red}") for c in cases)
page = "<!doctype html><html><head><meta charset=utf-8><title>x</title></head><body>%s<script>%s</script></body></html>" % (body, JS)
hp = os.path.join(out, "constructs.html"); open(hp, "w").write(page)
r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--virtual-time-budget=3000", "--dump-dom", "file://" + os.path.abspath(hp)], capture_output=True, text=True, timeout=120)
m = re.search(r"<title>(.*?)</title>", r.stdout, re.S)
counts = json.loads(html.unescape(m.group(1)))
for c, n in zip(cases, counts): print("%-60s chrome_rules=%d" % (c, n))
