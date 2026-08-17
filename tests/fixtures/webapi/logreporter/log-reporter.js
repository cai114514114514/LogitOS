/* bilibili's log reporter, reduced to the call path that threw. See SOURCE.
 *
 * Every function here is the shape of a REAL one, and the comment on each says
 * which page it came from. Nothing is here because a browser is supposed to
 * have it -- that is js_platform.c's rule and this file keeps it.
 *
 * The reduction is deliberately unguarded in the same places the original is:
 * document.domain is split with no typeof check, and document.cookie is split
 * with no length check. Those two omissions ARE the fixture. A defensive
 * rewrite would pass against a browser that returns undefined from both. */

var REPORT = {
    domain: null,       /* what getCurrentDomain computed */
    skipped: false,     /* the navigator.cookieEnabled branch was taken */
    names: [],          /* every cookie name document.cookie handed script */
    id: '',             /* the id the reporter ends up reporting under */
    wrote: false,       /* setCookie was called (as opposed to a cached id) */
    errors: []
};

/* bilibili, log-reporter.js, frame `getCurrentDomain` in the committed stack.
 * `document.domain` with no guard, then `.split('.')` -- an undefined here is
 * the whole 2026-08-16 exception. The registrable-suffix fold is why it is a
 * split and not a plain read: the cookie has to be set for bilibili.com so
 * every *.bilibili.com host shares it. */
function getCurrentDomain() {
    var parts = document.domain.split('.');
    if (parts.length <= 2) return document.domain;
    return parts.slice(-2).join('.');
}

/* bilibili, log-reporter.js, frame `setCookie`, one frame below the throw. */
function setCookie(name, value, days) {
    var s = name + '=' + value + '; path=/; domain=' + getCurrentDomain();
    if (days) {
        var d = new Date();
        d.setTime(d.getTime() + days * 86400000);
        s += '; expires=' + d.toUTCString();
    }
    document.cookie = s;
}

/* The READ half of the same file: `document.cookie` split into pairs. A
 * getter that answered undefined would throw here exactly as document.domain
 * did, one frame further on, and a getter that answered "" would silently
 * report every visitor as new -- which no exception counter can see. */
function readCookies() {
    var out = {}, all = document.cookie.split('; ');
    for (var i = 0; i < all.length; i++) {
        if (!all[i]) continue;
        var eq = all[i].indexOf('=');
        if (eq < 0) continue;
        out[all[i].slice(0, eq)] = all[i].slice(eq + 1);
    }
    return out;
}

/* The gate around the whole thing.
 * tests/fixtures/webapi/baidureal/s010.js:104 --
 *     function close(){ if (navigator.cookieEnabled) {
 *         document.cookie = "su=0; domain=www.baidu.com" } }
 * and bing/index.html:34 --
 *     navigator.cookieEnabled || r("COOKIEDISABLED")
 * Neither throws when it answers no. Both stop. */
function report() {
    try {
        REPORT.domain = getCurrentDomain();
        if (!navigator.cookieEnabled) { REPORT.skipped = true; return; }

        var jar = readCookies(), n;
        for (n in jar) REPORT.names.push(n);
        REPORT.names.sort();

        if (!jar.buvid3) {
            setCookie('buvid3', 'B0F1E2D3C4B5A697', 365);
            REPORT.wrote = true;
        }
        REPORT.id = readCookies().buvid3 || '';
    } catch (e) {
        REPORT.errors.push(String(e));
    }
}

report();
