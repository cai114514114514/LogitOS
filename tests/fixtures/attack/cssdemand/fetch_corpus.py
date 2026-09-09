#!/usr/bin/env python3
"""fetch_corpus.py -- capture the HTML + every stylesheet of N mainstream sites,
with the browser's OWN User-Agent, into <out>/<name>/{index.html,sheet-N.css,
MANIFEST}. Same on-disk shape as tests/fixtures/cssweb so the tree's existing
LibCSS probes (css_sel_recovery, css_audit) read it unchanged.

WHY THE UA IS OURS AND NOT CHROME'S. tests/fixtures/cssweb/capture.py sent a
Chrome UA; this one sends exactly the header c/apps/browser/js_webapi.c:991
sends ("Mozilla/5.0 (LogitOS) Logit/1.0"), because the question is what OUR
browser is served, and a site that refuses that UA is a finding (REFUSED), not
something to work around. Refusals are recorded, never retried with another UA.

Output per site (MANIFEST): the URL, HTTP status, bytes, and for every sheet
its URL + bytes; MISS lines for sheets that did not fetch; IMPORT lines for
sheets pulled through a top-of-sheet @import (one level).
"""
import os, re, sys, subprocess, urllib.parse, hashlib, json
from concurrent.futures import ThreadPoolExecutor

UA = "Mozilla/5.0 (LogitOS) Logit/1.0"
SITES = [
 ("bing","https://www.bing.com/"), ("google","https://www.google.com/"),
 ("youtube","https://www.youtube.com/"), ("wikipedia","https://en.wikipedia.org/wiki/Main_Page"),
 ("amazon","https://www.amazon.com/"), ("reddit","https://www.reddit.com/"),
 ("x","https://x.com/"), ("facebook","https://www.facebook.com/"),
 ("instagram","https://www.instagram.com/"), ("netflix","https://www.netflix.com/"),
 ("microsoft","https://www.microsoft.com/"), ("apple","https://www.apple.com/"),
 ("github","https://github.com/"), ("stackoverflow","https://stackoverflow.com/"),
 ("nytimes","https://www.nytimes.com/"), ("cnn","https://www.cnn.com/"),
 ("bbc","https://www.bbc.com/"), ("yahoo","https://www.yahoo.com/"),
 ("zhihu","https://www.zhihu.com/"), ("bilibili","https://www.bilibili.com/"),
 ("taobao","https://www.taobao.com/"), ("jd","https://www.jd.com/"),
 ("weibo","https://weibo.com/"), ("baidu","https://www.baidu.com/"),
 ("qq","https://www.qq.com/"), ("163","https://www.163.com/"),
 ("sina","https://www.sina.com.cn/"), ("linkedin","https://www.linkedin.com/"),
 ("ebay","https://www.ebay.com/"), ("medium","https://medium.com/"),
 ("twitch","https://www.twitch.tv/"), ("openai","https://openai.com/"),
 ("mozilla","https://www.mozilla.org/en-US/"), ("cloudflare","https://www.cloudflare.com/"),
 ("wordpress","https://wordpress.com/"), ("ddg","https://duckduckgo.com/"),
 ("tailwind","https://tailwindcss.com/"), ("mdn","https://developer.mozilla.org/en-US/"),
 ("pydocs","https://docs.python.org/3/"), ("hn","https://news.ycombinator.com/"),
 ("webdev","https://web.dev/"), ("apnews","https://apnews.com/"),
 ("douyin","https://www.douyin.com/"), ("espn","https://www.espn.com/"),
 ("imdb","https://www.imdb.com/"), ("booking","https://www.booking.com/"),
 ("airbnb","https://www.airbnb.com/"), ("spotify","https://open.spotify.com/"),
 ("wired","https://www.wired.com/"), ("theguardian","https://www.theguardian.com/"),
 ("stripe","https://stripe.com/"), ("vercel","https://vercel.com/"),
]

def fetch(url, timeout=40):
    r = subprocess.run(["curl", "-sSL", "--compressed", "-m", str(timeout), "-A", UA,
                        "-H", "Accept-Language: en-US,en;q=0.9,zh-CN;q=0.8",
                        "-H", "Accept: text/html,text/css,*/*;q=0.8",
                        "-o", "-", "-w", "\n\x01STATUS=%{http_code} FINAL=%{url_effective}",
                        url], capture_output=True)
    if r.returncode != 0:
        return None, "curl-exit-%d %s" % (r.returncode, r.stderr.decode("utf-8","replace").strip()[:120]), ""
    out = r.stdout
    i = out.rfind(b"\n\x01STATUS=")
    if i < 0:
        return out, "?", ""
    body, trailer = out[:i], out[i+2:].decode("utf-8","replace")
    m = re.match(r"STATUS=(\d+) FINAL=(.*)", trailer)
    return body, m.group(1), m.group(2)

REFUSE_PAT = re.compile(rb"just a moment|captcha|access denied|are you a robot|enable javascript and cookies|unusual traffic|request blocked|attention required|verify you are human", re.I)

def sheet_hrefs(text):
    hrefs = []
    for m in re.finditer(r'<link\b[^>]*>', text, re.I):
        tag = m.group(0)
        if not re.search(r'rel\s*=\s*["\']?[^"\'>]*stylesheet', tag, re.I):
            continue
        h = re.search(r'(?<![-\w])href\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))', tag, re.I)
        if not h: continue
        href = (h.group(1) or h.group(2) or h.group(3)).strip().replace("&amp;", "&")
        if href and href not in hrefs: hrefs.append(href)
    return hrefs

def imports(css):
    head = css[:4096].decode("utf-8", "replace")
    return [m.group(1) for m in re.finditer(r'@import\s+(?:url\(\s*)?["\']?([^"\')\s;]+)', head)]

def one(site):
    name, url = site
    d = os.path.join(OUT, name); os.makedirs(d, exist_ok=True)
    html, status, final = fetch(url)
    man = ["# %s\n# %s\n# STATUS %s FINAL %s\n" % (name, url, status, final)]
    rec = {"site": name, "url": url, "status": status, "final": final, "html_bytes": len(html) if html else 0,
           "sheets": 0, "css_bytes": 0, "inline_style_blocks": 0, "verdict": "ok"}
    if html is None or status not in ("200",) or len(html) < 512 or (len(html) < 20000 and REFUSE_PAT.search(html)):
        rec["verdict"] = "REFUSED status=%s bytes=%d%s" % (status, len(html) if html else 0,
              " pattern=" + REFUSE_PAT.search(html).group(0).decode() if html and REFUSE_PAT.search(html) else "")
        if html: open(os.path.join(d, "index.html"), "wb").write(html)
        man.append("REFUSED %s\n" % rec["verdict"])
        open(os.path.join(d, "MANIFEST"), "w").write("".join(man))
        return rec
    open(os.path.join(d, "index.html"), "wb").write(html)
    text = html.decode("utf-8", "replace")
    rec["inline_style_blocks"] = len(re.findall(r'<style\b', text, re.I))
    n = 0
    queue = [(urllib.parse.urljoin(final or url, h), "") for h in sheet_hrefs(text)[:40]]
    seen = set()
    while queue and n < 60:
        full, how = queue.pop(0)
        if full in seen: continue
        seen.add(full)
        css, st, fin = fetch(full, 30)
        if not css or st != "200" or len(css) < 32:
            man.append("MISS%s %s status=%s\n" % (how, full, st)); continue
        n += 1
        open(os.path.join(d, "sheet-%d.css" % n), "wb").write(css)
        man.append("sheet-%d.css %d %s%s\n" % (n, len(css), full, how))
        rec["css_bytes"] += len(css)
        for imp in imports(css):
            queue.append((urllib.parse.urljoin(full, imp), " IMPORT"))
    rec["sheets"] = n
    open(os.path.join(d, "MANIFEST"), "w").write("".join(man))
    return rec

if __name__ == "__main__":
    OUT = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    with ThreadPoolExecutor(max_workers=6) as ex:
        recs = list(ex.map(one, SITES))
    json.dump(recs, open(os.path.join(OUT, "fetch.json"), "w"), indent=1)
    for r in recs:
        print("%-14s %-8s html=%-8d sheets=%-3d css=%-8d inline<style>=%-3d %s" % (
            r["site"], r["status"], r["html_bytes"], r["sheets"], r["css_bytes"], r["inline_style_blocks"], r["verdict"]))
