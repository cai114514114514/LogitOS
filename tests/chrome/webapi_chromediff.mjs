#!/usr/bin/env node
/* webapi_chromediff -- run the SAME captured bytes through real headless
 * Chrome and subtract its exceptions from ours.
 *
 *   node tests/chrome/webapi_chromediff.mjs tests/fixtures/webapi/*\/
 *   node tests/chrome/webapi_chromediff.mjs --probe build/probe.json <dirs>
 *
 * WHY THIS EXISTS -- it is the methodological hole, not a nicety.
 *
 * The probe counts uncaught exceptions per page. That number is not a work
 * list, because a large part of it is the PAGE's own bugs. bing's <inline 2>
 * assigns `_w.sj_pt` at byte 108900 and the only `var _w = window` is at
 * 146310, in a later script; Chrome throws there too. Until today there was no
 * way to tell that apart from a gap in this browser except by reading the
 * bundle, which does not scale past one.
 *
 * So: serve the committed fixture to Chrome, over the page's REAL URL, and
 * diff the exception sets. Every exception Chrome also throws is not ours.
 * What survives the subtraction is the work list.
 *
 * HOW THE URL IS KEPT IDENTICAL, which is the part that decides whether the
 * diff means anything. A page branches on location.hostname constantly, and
 * two engines on two different document URLs take two different code paths --
 * every such branch would show up as a disagreement. So Chrome is NOT pointed
 * at localhost. It is pointed at `https://www.kimi.com/`, with
 * --host-resolver-rules=MAP * 127.0.0.1:PORT, so every hostname in the fixture
 * resolves to the local server while the URL Chrome and the page see is the
 * real one, byte for byte the same string the probe passes js_page_set_location.
 * The certificate is self-signed and thrown away; --ignore-certificate-errors
 * is what makes that acceptable, and it is scoped to this one throwaway
 * profile.
 *
 * THE USER'S MACHINE IS NOT TOUCHED. A fresh --user-data-dir per run under the
 * OS temp directory, deleted afterwards: no history, no profile, no cache, no
 * extension, and --no-first-run so nothing is written outside it.
 *
 * WHAT IS DELIBERATELY NOT COMPARED.
 *   - Network failures. The host probe has no network at all (WEBAPI_HOST
 *     leaves struct webapi_net NULL) and Chrome gets a 404 from this server
 *     for anything not in the fixture. Both produce errors, with different
 *     wording, from the same cause: the corpus is a capture, not a site. They
 *     go in a `network` bucket on both sides and are excluded, stated here
 *     rather than filtered silently.
 *   - Sub-resources. Chrome loads stylesheets, images and fonts; the probe
 *     does not. Neither throws a JS exception for a missing one.
 */
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import https from "node:https";
import { execFileSync } from "node:child_process";

const CHROME = process.env.CHROME_PATH ||
    "C:/Program Files/Google/Chrome/Application/chrome.exe";

/* ---- normalisation: the only part of this tool with an opinion ----------
 *
 * The two engines describe the same failure in different words:
 *
 *   QuickJS  TypeError: cannot set property 'theme' of undefined
 *   V8       Uncaught TypeError: Cannot set properties of undefined (setting 'theme')
 *
 * A string diff calls that a disagreement and puts a page bug on our work
 * list. So an exception is reduced to (error type, the identifier it names),
 * which is the part both engines agree on and the part that says what is
 * missing. Where no identifier can be extracted the whole message is used,
 * lowercased with digits stripped -- those cases stay visible as "unmatched
 * wording" rather than silently counting as ours. */
function normalise(raw) {
    let m = String(raw || "").trim();
    m = m.replace(/^\[(?:error|warn)\]\s*/i, "");
    m = m.replace(/^Uncaught\s*\(in promise\)\s*/i, "");
    m = m.replace(/^Uncaught\s+/i, "");
    m = m.split("\n")[0].trim();
    const low = m.toLowerCase();

    const type = (m.match(/^([A-Z]\w*Error|DOMException)\b/) || [])[1] || "";
    const pats = [
        /'([^']+)' is not defined/i,
        /([\w$][\w$.]*) is not defined/i,
        /reading '([^']+)'/i,
        /read property '([^']+)'/i,
        /setting '([^']+)'/i,
        /set property '([^']+)'/i,
        /'([^']+)' is not a function/i,
        /([\w$][\w$.]*) is not a function/i,
        /'([^']+)' is not a constructor/i,
        /([\w$][\w$.]*) is not a constructor/i,
        /property '([^']+)'/i,
    ];
    for (const p of pats) {
        const g = low.match(p);
        if (g) return { key: `${type.toLowerCase()}|${g[1]}`, type, subject: g[1], msg: m };
    }
    return { key: `${type.toLowerCase()}|~${low.replace(/[0-9]+/g, "#")}`, type, subject: "", msg: m };
}

/* Errors that are about this harness rather than about either engine. */
function isNetwork(msg) {
    const s = String(msg).toLowerCase();
    return s.includes("could not open a socket") ||
           s.includes("failed to fetch") ||
           s.includes("networkerror") ||
           s.includes("err_") ||
           s.includes("net::") ||
           s.includes("failed to load resource") ||
           s.includes("load failed");
}

/* ---- the fixture, served over its own hostnames ------------------------ */
function readSource(dir) {
    const p = path.join(dir, "SOURCE");
    if (!fs.existsSync(p)) return "https://fixture.invalid/";
    return fs.readFileSync(p, "utf8").split("\n")[0].trim() || "https://fixture.invalid/";
}

function canon(u) {
    try {
        const x = new URL(u);
        /* Default ports removed on both sides: the manifest says
         * https://host/p and the Host header says host, and a route table that
         * disagreed on ":443" would 404 the whole application. */
        if ((x.protocol === "https:" && x.port === "443") ||
            (x.protocol === "http:" && x.port === "80")) x.port = "";
        x.hash = "";
        return x.toString();
    } catch { return u; }
}

function routes(dir, pageUrl) {
    const table = new Map();
    const mf = path.join(dir, "manifest.txt");
    if (fs.existsSync(mf)) {
        for (const line of fs.readFileSync(mf, "utf8").split("\n")) {
            const t = line.indexOf("\t");
            if (t < 0) continue;
            const src = line.slice(0, t), file = line.slice(t + 1).trim();
            if (!file) continue;
            let abs;
            try { abs = canon(new URL(src, pageUrl).toString()); } catch { continue; }
            table.set(abs, path.join(dir, file));
        }
    }
    table.set(canon(pageUrl), path.join(dir, "index.html"));
    return table;
}

let g_cert = null;
function selfSignedCert() {
    if (g_cert) return g_cert;
    const d = fs.mkdtempSync(path.join(os.tmpdir(), "wadiff-cert-"));
    const key = path.join(d, "k.pem"), crt = path.join(d, "c.pem");
    /* openssl rather than a bundled key pair: a private key committed to a
     * repository is a private key on the internet, even a throwaway one. */
    execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", key, "-out", crt, "-days", "1", "-subj", "/CN=logitos-fixture"],
        { stdio: "ignore" });
    g_cert = { key: fs.readFileSync(key), cert: fs.readFileSync(crt), dir: d };
    return g_cert;
}

function serve(table) {
    const { key, cert } = selfSignedCert();
    const hits = [], misses = [];
    /* ALPN, explicitly. Chrome offers h2 first on TLS; a server that neither
     * speaks it nor names http/1.1 in ALPN gets the connection closed with no
     * bytes, which Chrome reports as ERR_EMPTY_RESPONSE and this harness would
     * have reported as "the page had no errors". */
    const server = https.createServer({ key, cert, ALPNProtocols: ["http/1.1"] }, (req, res) => {
        const url = canon(`https://${req.headers.host}${req.url}`);
        const file = table.get(url) || table.get(canon(url.split("?")[0]));
        if (!file || !fs.existsSync(file)) {
            misses.push(url);
            res.writeHead(404, { "content-type": "text/plain" });
            res.end("not in fixture");
            return;
        }
        hits.push(url);
        const body = fs.readFileSync(file);
        res.writeHead(200, {
            "content-type": file.endsWith(".html")
                ? "text/html; charset=utf-8" : "text/javascript; charset=utf-8",
            "content-length": body.length,
            /* The fixture is one origin's document served under many
             * hostnames, so a cross-origin module fetch has to be allowed or
             * the graph will not link in Chrome and the diff would compare a
             * page that ran against one that did not. */
            "access-control-allow-origin": "*",
        });
        res.end(body);
    });
    if (process.env.WADIFF_DEBUG) {
        server.on("tlsClientError", e => console.log("  [debug] tlsClientError:", e.message));
        server.on("clientError", e => console.log("  [debug] clientError:", e.message));
        server.on("connection", () => console.log("  [debug] tcp connection"));
    }
    return new Promise(resolve => {
        server.listen(0, "127.0.0.1", () => resolve({ server, port: server.address().port, hits, misses }));
    });
}

/* ---- Chrome over CDP --------------------------------------------------- */
async function cdp(port, ms, pageUrl, log) {
    const deadline = Date.now() + 20000;
    let target = null;
    while (Date.now() < deadline) {
        try {
            const r = await fetch(`http://127.0.0.1:${port}/json/list`);
            const list = await r.json();
            target = list.find(t => t.type === "page");
            if (target) break;
        } catch { /* chrome not up yet */ }
        await new Promise(r => setTimeout(r, 200));
    }
    if (!target) throw new Error("chrome never exposed a page target");

    const ws = new WebSocket(target.webSocketDebuggerUrl);
    let id = 0;
    const pend = new Map();
    const send = (method, params) => new Promise((res, rej) => {
        const i = ++id;
        pend.set(i, { res, rej });
        ws.send(JSON.stringify({ id: i, method, params: params || {} }));
    });

    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    ws.onmessage = ev => {
        const m = JSON.parse(ev.data);
        if (m.id && pend.has(m.id)) { pend.get(m.id).res(m.result); pend.delete(m.id); return; }
        if (m.method === "Runtime.exceptionThrown") {
            const d = m.params.exceptionDetails || {};
            const desc = (d.exception && (d.exception.description || d.exception.value)) || d.text || "";
            log.push({ kind: "exception", where: d.url || "?", msg: String(desc) });
        } else if (m.method === "Runtime.consoleAPICalled") {
            if (m.params.type !== "error" && m.params.type !== "warning") return;
            const txt = (m.params.args || []).map(a =>
                a.description !== undefined ? a.description :
                a.value !== undefined ? String(a.value) : (a.className || a.type)).join(" ");
            log.push({ kind: m.params.type === "error" ? "console" : "warn",
                       where: (m.params.stackTrace?.callFrames?.[0]?.url) || "?", msg: txt });
        }
    };

    await send("Runtime.enable");
    await send("Page.enable");
    const nav = await send("Page.navigate", { url: pageUrl });
    /* A navigation that fails leaves an empty page and every counter at zero,
     * which looks exactly like a page with no errors. Say so instead. */
    if (nav && nav.errorText) log.push({ kind: "navfail", where: pageUrl, msg: nav.errorText });
    if (process.env.WADIFF_DEBUG) console.log("  [debug] navigate ->", JSON.stringify(nav));
    await new Promise(r => setTimeout(r, ms));
    try { ws.close(); } catch {}
}

async function runChrome(dir, waitMs) {
    const pageUrl = readSource(dir);
    const table = routes(dir, pageUrl);
    const { server, port, hits, misses } = await serve(table);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), "wadiff-profile-"));
    const dbg = 9333 + Math.floor(Math.random() * 500);
    const args = [
        "--headless=new", "--disable-gpu", "--no-first-run",
        "--no-default-browser-check", "--disable-extensions",
        "--disable-background-networking", "--disable-sync",
        "--no-service-autorun", "--password-store=basic",
        `--user-data-dir=${profile}`,
        `--remote-debugging-port=${dbg}`,
        `--host-resolver-rules=MAP * 127.0.0.1:${port}`,
        /* MEASURED, and it cost an hour: this machine has a system HTTP proxy.
         * Chrome used it, and --host-resolver-rules mapped the PROXY's
         * hostname to the fixture server too -- so Chrome sent
         * `CONNECT host:443` in plaintext at a TLS port, the handshake failed,
         * and Page.navigate came back ERR_EMPTY_RESPONSE. An empty page has no
         * exceptions, which is indistinguishable from a page that works. */
        "--no-proxy-server",
        "--ignore-certificate-errors",
        "about:blank",
    ];
    const child = spawn(CHROME, args, { stdio: "ignore" });
    const log = [];
    try {
        await cdp(dbg, waitMs, pageUrl, log);
    } finally {
        try { child.kill(); } catch {}
        server.close();
        await new Promise(r => setTimeout(r, 300));
        try { fs.rmSync(profile, { recursive: true, force: true }); } catch {}
    }
    return { log, hits: hits.length, misses };
}

/* ---- the probe's side -------------------------------------------------- */
function readProbe(file) {
    const out = new Map();
    if (!file || !fs.existsSync(file)) return out;
    for (const line of fs.readFileSync(file, "utf8").split("\n")) {
        if (!line.startsWith("#JSON\t")) continue;
        const [, site, kind, where, ...rest] = line.split("\t");
        const msg = rest.join("\t");
        if (!out.has(site)) out.set(site, []);
        out.get(site).push({ kind, where, msg });
    }
    return out;
}

function bucket(entries) {
    /* One entry per distinct normalised key. A page that throws the same
     * ReferenceError forty times from forty component mounts is ONE missing
     * API, and counting it forty times would rank it above forty real ones. */
    const m = new Map();
    for (const e of entries) {
        if (e.kind === "fetch" || e.kind === "netstub") continue;
        if (isNetwork(e.msg)) continue;
        const n = normalise(e.msg);
        if (!n.type && !n.subject) continue;      /* not an error object */
        if (!m.has(n.key)) m.set(n.key, { ...n, n: 0, kinds: new Set() });
        const b = m.get(n.key);
        b.n++;
        b.kinds.add(e.kind);
    }
    return m;
}

async function main() {
    const a = process.argv.slice(2);
    let probeFile = null, waitMs = 9000;
    const dirs = [];
    for (let i = 0; i < a.length; i++) {
        if (a[i] === "--probe") probeFile = a[++i];
        else if (a[i] === "--wait") waitMs = parseInt(a[++i], 10);
        else dirs.push(a[i].replace(/[\\/]+$/, ""));
    }
    if (!dirs.length) {
        console.log("usage: webapi_chromediff.mjs [--probe probe.json] [--wait ms] <fixture-dir>...");
        process.exit(2);
    }
    if (!fs.existsSync(CHROME)) {
        console.log(`chrome not found at ${CHROME}; set CHROME_PATH`);
        process.exit(2);
    }

    const probe = readProbe(probeFile);
    const ours = new Map();          // key -> {pages:Set, sample}
    console.log("== Chrome differential: the same committed bytes, both engines ==\n");

    for (const dir of dirs) {
        const site = path.basename(dir);
        let res;
        try { res = await runChrome(dir, waitMs); }
        catch (e) { console.log(`  ${site.padEnd(11)} CHROME FAILED: ${e.message}`); continue; }

        const cb = bucket(res.log);
        const pb = bucket(probe.get(site) || []);
        const onlyOurs = [...pb.keys()].filter(k => !cb.has(k));
        const shared = [...pb.keys()].filter(k => cb.has(k));

        console.log(`  ${site.padEnd(11)} chrome served ${res.hits} files, ` +
                    `${res.misses.length} not in fixture`);
        console.log(`  ${site.padEnd(11)} chrome ${cb.size} distinct, ` +
                    `ours ${pb.size} distinct, shared ${shared.length}, OURS ONLY ${onlyOurs.length}`);
        for (const k of shared)
            console.log(`     [both]  ${pb.get(k).msg.slice(0, 100)}`);
        for (const k of onlyOurs) {
            const e = pb.get(k);
            console.log(`     [OURS]  ${e.msg.slice(0, 110)}`);
            if (!ours.has(k)) ours.set(k, { pages: new Set(), sample: e.msg, kinds: new Set() });
            ours.get(k).pages.add(site);
            for (const kk of e.kinds) ours.get(k).kinds.add(kk);
        }
        /* Chrome-only entries are printed too. They are not a work list -- they
         * are usually a page path Chrome reached and we did not -- but a
         * category we never see at all is worth knowing about. */
        for (const k of [...cb.keys()].filter(k => !pb.has(k)))
            console.log(`     [chrome-only] ${cb.get(k).msg.slice(0, 100)}`);
        console.log("");
    }

    const ranked = [...ours.entries()].sort((x, y) =>
        y[1].pages.size - x[1].pages.size || x[0].localeCompare(y[0]));
    console.log("== OURS: exceptions Chrome does NOT throw on the same bytes ==");
    console.log("PAGES  KIND      MESSAGE");
    for (const [, v] of ranked)
        console.log(`${String(v.pages.size).padStart(5)}  ${[...v.kinds].join(",").padEnd(9)} ` +
                    `${v.sample.slice(0, 110)}   [${[...v.pages].join(",")}]`);
    if (!ranked.length) console.log("  (none)");
}

main().catch(e => { console.error(e); process.exit(1); });
