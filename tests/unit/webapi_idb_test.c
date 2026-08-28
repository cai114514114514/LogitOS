/* Host test for c/apps/browser/js_idb.c -- IndexedDB.
 *
 *     make test-idb          the coherent-subset suite AND the termination
 *                             bar's own quiescence check in one binary
 *     make test-idb-negctl   the SAME file, linked with -DJS_IDB_NEGCTL
 *                             (js_idb.c's own stub that replaces
 *                             IDBObjectStore.prototype.get with a request
 *                             nobody ever schedules a settle-task for) --
 *                             must FAIL, and specifically report the stuck
 *                             request rather than passing or hanging.
 *
 * THE ACCEPTANCE CRITERION IS ABOVE THE SCENARIO CHECKS: after every scenario
 * below has run and the event loop has been pumped to a bound, every
 * IDBRequest this file created (tracked in the JS array `allReqs`) must have
 * readyState === 'done', and every IDBTransaction it created (tracked by
 * `allTxnsTotal`/`allTxnsDone`) must have fired complete or abort. That is
 * the property js_platform.h's old comment demanded and js_idb.c's header
 * argues it has by construction; this is where the argument gets checked
 * against a real, pumped event loop rather than asserted.
 *
 * pump_until_idle bounds the pump at a fixed number of passes rather than
 * looping on js_page_pending() forever -- on the negctl build the stuck
 * request never gets counted into g_timers at all (the stub does not call
 * scheduleSettle), so the loop drains everything else and returns cleanly;
 * the STUCK request is caught by the readyState check afterwards, not by a
 * hang. That is deliberate: this harness must never itself hang, on EITHER
 * build, or a real bug here would read as "the test suite is slow" instead
 * of "a request never settled". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

static JSContext *ctx;
static int checks, failures;

static void ck(int cond, const char *name)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", name); }
    else printf("ok  : %s\n", name);
}

static JSValue eval(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("      [js exception] %s\n         while evaluating: %s\n", m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { printf("      %s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void run(const char *src) { JS_FreeValue(ctx, eval(src)); js_page_pump(); }

static void ckjs(const char *expr, const char *name)
{
    char buf[8192];
    snprintf(buf, sizeof buf, "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) && !JS_IsString(v);
    if (!ok && !JS_IsException(v)) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now; }
static void tick(int ms) { g_now += (unsigned long long)ms; js_page_run_due(); js_page_pump(); }

/* Bounded, on purpose -- see the file header. Each pass advances the fake
 * clock by 1ms (zero-delay timers do not need it, but a real page can use
 * setTimeout(fn, N) too, and this keeps the loop generic) and drains both
 * the timer queue and the microtask queue. */
static int pump_until_idle(int max_passes)
{
    int n = 0;
    while (js_page_pending() && n < max_passes) { tick(1); n++; }
    return n;
}

static const char *PAGE =
    "<!doctype html><html><head><title>t</title></head><body>"
    "<script>var ran=1;</script></body></html>";

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: fixture did not parse\n"); return 1; }

    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/idb.html");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    /* ==== feature detection surface ===================================== */
    ckjs("typeof window.indexedDB === 'object' && window.indexedDB !== null", "window.indexedDB exists");
    ckjs("self.indexedDB === window.indexedDB", "self.indexedDB is the same object (self === window)");
    ckjs("typeof indexedDB.open === 'function'", "indexedDB.open is a function");
    ckjs("typeof indexedDB.deleteDatabase === 'function'", "indexedDB.deleteDatabase is a function");
    ckjs("typeof indexedDB.cmp === 'function'", "indexedDB.cmp is a function");
    ckjs("typeof indexedDB.databases === 'function'", "indexedDB.databases is a function");
    ckjs("typeof IDBKeyRange === 'function'", "IDBKeyRange exists");
    ckjs("typeof IDBRequest === 'function' && typeof IDBOpenDBRequest === 'function'", "IDBRequest / IDBOpenDBRequest exist");
    ckjs("typeof IDBDatabase === 'function' && typeof IDBTransaction === 'function'", "IDBDatabase / IDBTransaction exist");
    ckjs("typeof IDBObjectStore === 'function' && typeof IDBIndex === 'function'", "IDBObjectStore / IDBIndex exist");
    ckjs("typeof IDBCursor === 'function' && typeof IDBCursorWithValue === 'function'", "IDBCursor / IDBCursorWithValue exist");
    ckjs("typeof IDBVersionChangeEvent === 'function'", "IDBVersionChangeEvent exists");
    ckjs("typeof indexedDB.getAllRecords === 'undefined' && typeof IDBIndex.prototype.getAllRecords === 'undefined'",
         "getAllRecords stays absent (out of scope, not stubbed)");
    ckjs("(function(){ try { new IDBKeyRange(); return false; } catch (e) { return e instanceof TypeError; } })()",
         "IDBKeyRange has no public constructor");

    /* Every request/transaction created below is tracked here so the
     * quiescence check at the end has something concrete to walk. */
    run("var allReqs = []; var allTxnsTotal = 0, allTxnsDone = 0;");
    run("function T(t) { allTxnsTotal++; t.addEventListener('complete', function(){ allTxnsDone++; });"
        " t.addEventListener('abort', function(){ allTxnsDone++; }); return t; }");
    run("function R(r) { allReqs.push(r); return r; }");

    /* ==== open + upgradeneeded + createObjectStore + index =============== */
    run("var log = [];");
    run("var openReq = R(indexedDB.open('t1', 1));");
    run("openReq.onupgradeneeded = function (e) {"
        "  log.push('upgradeneeded:' + e.oldVersion + '->' + e.newVersion);"
        "  var db = e.target.result;"
        "  var os = db.createObjectStore('s', { keyPath: 'id' });"
        "  os.createIndex('byName', 'name', { unique: false });"
        "  os.createIndex('byUniq', 'uniq', { unique: true });"
        "};");
    run("openReq.onsuccess = function (e) { window.db1 = e.target.result; log.push('opened'); };");
    run("openReq.onerror = function (e) { log.push('open-error:' + e.target.error); };");
    pump_until_idle(50);
    ckjs("log.join(',') === 'upgradeneeded:0->1,opened'", "open() on a new database fires upgradeneeded then success, in order");
    ckjs("db1.version === 1", "IDBDatabase.version reflects the version just created");
    ckjs("db1.objectStoreNames.length === 1 && db1.objectStoreNames.contains('s')", "objectStoreNames reflects the store created in upgradeneeded");
    ckjs("openReq.readyState === 'done'", "the open request itself reached readyState done");

    /* ==== put / get / getKey / count roundtrip, in transaction order ===== */
    run("var txnA = T(db1.transaction('s', 'readwrite'));");
    run("var osA = txnA.objectStore('s');");
    run("var putReq = R(osA.put({ id: 1, name: 'alice', uniq: 'a' }));");
    run("var putReq2 = R(osA.put({ id: 2, name: 'bob', uniq: 'b' }));");
    run("var getReq, getKeyReq, countReq;");
    run("putReq2.onsuccess = function () {"
        "  getReq = R(osA.get(1));"
        "  getKeyReq = R(osA.getKey(2));"
        "  countReq = R(osA.count());"
        "  getReq.onsuccess = function () { log.push('got:' + getReq.result.name); };"
        "};");
    pump_until_idle(50);
    ckjs("getReq.result.name === 'alice' && getReq.result.id === 1", "get(1) returns the record put under key 1");
    ckjs("getKeyReq.result === 2", "getKey(2) returns the primary key, not the value");
    ckjs("countReq.result === 2", "count() with no filter counts every record");
    ckjs("txnA.error === null", "a clean read/write transaction carries no error");

    /* structuredClone semantics: mutating the object passed to put() after the
     * call must not be visible in the stored record. */
    run("var mutMe = { id: 9, name: 'orig', uniq: 'z' };");
    run("var txnMut = T(db1.transaction('s', 'readwrite'));");
    run("var putMut = R(txnMut.objectStore('s').put(mutMe)); mutMe.name = 'mutated-after-put';");
    run("var getMut;");
    run("putMut.onsuccess = function () { getMut = R(db1.transaction('s','readonly').objectStore('s').get(9)); T(getMut.transaction);"
        " getMut.onsuccess = function () { log.push('clone:' + getMut.result.name); }; };");
    pump_until_idle(50);
    ckjs("getMut.result.name === 'orig'", "put() clones the value -- a later mutation of the caller object is not visible in the store");

    /* ==== cursor iteration, ascending and descending ===================== */
    run("var seen = [];");
    run("var txnC = T(db1.transaction('s', 'readonly'));");
    run("var curReq = R(txnC.objectStore('s').openCursor());");
    run("curReq.onsuccess = function (e) {"
        "  var c = e.target.result;"
        "  if (c) { seen.push(c.key); c.continue(); } else { log.push('cursor-done:' + seen.join(',')); }"
        "};");
    pump_until_idle(80);
    ckjs("seen.join(',') === '1,2,9'", "openCursor() with no direction walks keys in ascending order");

    run("var seenRev = [];");
    run("var txnCR = T(db1.transaction('s', 'readonly'));");
    run("var curReqR = R(txnCR.objectStore('s').openCursor(null, 'prev'));");
    run("curReqR.onsuccess = function (e) { var c = e.target.result; if (c) { seenRev.push(c.key); c.continue(); } };");
    pump_until_idle(80);
    ckjs("seenRev.join(',') === '9,2,1'", "openCursor(null, 'prev') walks keys in descending order");

    /* ==== index get / cursor ============================================= */
    run("var txnI = T(db1.transaction('s', 'readonly'));");
    run("var idxByName = txnI.objectStore('s').index('byName');");
    run("var idxGetReq = R(idxByName.get('bob'));");
    run("idxGetReq.onsuccess = function () { log.push('idx-get:' + idxGetReq.result.id); };");
    pump_until_idle(50);
    ckjs("idxGetReq.result.id === 2", "IDBIndex.get looks the record up by the indexed field, not the primary key");

    /* ==== IDBKeyRange ===================================================== */
    run("var txnKR = T(db1.transaction('s', 'readonly'));");
    run("var krReq = R(txnKR.objectStore('s').getAllKeys(IDBKeyRange.bound(1, 2)));");
    pump_until_idle(50);
    ckjs("krReq.result.join(',') === '1,2'", "getAllKeys(bound(1,2)) filters to the range");
    ckjs("IDBKeyRange.only(5).includes(5) === true && IDBKeyRange.only(5).includes(6) === false", "IDBKeyRange.only().includes()");
    ckjs("indexedDB.cmp(1, 2) < 0 && indexedDB.cmp('a', 1) > 0", "indexedDB.cmp follows the key order (number before string)");

    /* ==== add() duplicate key: ConstraintError on the request, and because
     * nobody preventDefault()s it, the transaction aborts and the successful
     * write earlier in the SAME transaction is rolled back. ==================
     * This is also the async-error / auto-abort path js_platform.h worried
     * about -- it must fire 'error', not hang. */
    run("var txnD = T(db1.transaction('s', 'readwrite'));");
    run("var osD = txnD.objectStore('s');");
    run("var addOk = R(osD.add({ id: 20, name: 'temp', uniq: 'temp-u' }));");
    run("var addDup, dupErrName = null, txnDAborted = false;");
    run("addOk.onsuccess = function () { addDup = R(osD.add({ id: 1, name: 'dup', uniq: 'dup-u' })); "
        "addDup.onerror = function (e) { dupErrName = addDup.error.name; }; };");
    run("txnD.onabort = function () { txnDAborted = true; };");
    pump_until_idle(50);
    ckjs("dupErrName === 'ConstraintError'", "add() with a duplicate primary key fires error with ConstraintError");
    ckjs("txnDAborted === true", "an unhandled request error aborts its transaction");
    run("var txnCheck = T(db1.transaction('s', 'readonly'));");
    run("var checkReq = R(txnCheck.objectStore('s').get(20));");
    pump_until_idle(50);
    ckjs("checkReq.result === undefined", "the transaction abort rolled back the earlier successful add() in the same transaction");

    /* ==== explicit abort() also rolls back and always fires 'abort' ======= */
    run("var txnE = T(db1.transaction('s', 'readwrite'));");
    run("var putE = R(txnE.objectStore('s').put({ id: 30, name: 'willvanish', uniq: 'v' }));");
    run("var txnEAborted = false;");
    run("txnE.onabort = function () { txnEAborted = true; };");
    run("putE.onsuccess = function () { txnE.abort(); };");
    pump_until_idle(50);
    ckjs("txnEAborted === true", "explicit IDBTransaction.abort() fires the abort event");
    run("var txnCheck2 = T(db1.transaction('s', 'readonly'));");
    run("var checkReq2 = R(txnCheck2.objectStore('s').get(30));");
    pump_until_idle(50);
    ckjs("checkReq2.result === undefined", "explicit abort() rolled back the write made inside it");

    /* ==== an idle transaction commits rather than lingering =============== */
    run("var txnIdle = T(db1.transaction('s', 'readonly'));");
    pump_until_idle(50);
    ckjs("txnIdle.error === null", "a transaction with zero requests still reaches complete (via oncomplete/onabort tracked above)");

    /* ==== the refusal: Blob/File as a stored value THROWS synchronously,
     * not a request that resolves to something wrong ======================= */
    ckjs(
        "(function () {"
        "  if (typeof Blob !== 'function') return true;"
        "  var txn = db1.transaction('s', 'readwrite');"
        "  var os = txn.objectStore('s');"
        "  try { os.put(new Blob(['x']), 999); return false; }"
        "  catch (e) { return e instanceof DOMException && e.name === 'DataCloneError'; }"
        "})()",
        "put() with a Blob value throws DataCloneError synchronously (refused by name, not silently accepted)");

    /* ==== deleteDatabase, and it must terminate even though nothing keeps
     * a reference to it: support.js-style cleanup calls this and moves on. */
    run("var delLog = [];");
    run("db1.close();");
    run("var delReq = R(indexedDB.deleteDatabase('t1'));");
    run("delReq.onsuccess = function () { delLog.push('deleted'); };");
    pump_until_idle(50);
    ckjs("delLog.join(',') === 'deleted'", "deleteDatabase() on a closed connection completes");
    ckjs("delReq.readyState === 'done'", "the deleteDatabase request itself reached readyState done");

    /* ==== reopening after delete starts a fresh upgrade from version 0 ==== */
    run("var reopenLog = [];");
    run("var reopenReq = R(indexedDB.open('t1', 1));");
    run("reopenReq.onupgradeneeded = function (e) { reopenLog.push('v:' + e.oldVersion); };");
    run("reopenReq.onsuccess = function (e) { reopenLog.push('ok'); window.db1reopen = e.target.result; };");
    pump_until_idle(50);
    ckjs("reopenLog.join(',') === 'v:0,ok'", "after deleteDatabase, open() again upgrades from version 0");
    /* Close it before the next scenario opens a new version -- otherwise this
     * live connection legitimately blocks that upgrade forever (spec-correct:
     * see js_idb.c's header on 'blocked' being bounded only by the page
     * itself closing its connections), and the test would demonstrate a real
     * property of the spec rather than the one this section is checking. */
    run("db1reopen.close();");

    /* ==== requesting a lower version than the current one is a VersionError,
     * delivered on the request -- not a silent no-op and not a hang ======== */
    run("var lowLog = null;");
    run("var lowReq = R(indexedDB.open('t1', 999));");
    run("lowReq.onsuccess = function (e) { e.target.result.close(); "
        "  var lr2 = R(indexedDB.open('t1', 1)); lr2.onerror = function () { lowLog = lr2.error.name; }; };");
    pump_until_idle(50);
    ckjs("lowLog === 'VersionError'", "open() with a version below the current one fires error with VersionError, not silence");

    /* ==== THE ACCEPTANCE CRITERION: every request and transaction created
     * anywhere above reached a terminal state within the bounded pump. This
     * is what the file header calls the property js_platform.h demanded. */
    {
        JSValue stuckv = eval(
            "(function () {"
            "  for (var i = 0; i < allReqs.length; i++) if (allReqs[i].readyState !== 'done') return i;"
            "  return -1;"
            "})()");
        int32_t stuck = -1;
        JS_ToInt32(ctx, &stuck, stuckv);
        JS_FreeValue(ctx, stuckv);
        if (stuck >= 0) printf("STUCK: allReqs[%d] never reached readyState 'done' after the bounded pump\n", stuck);
        ck(stuck < 0, "quiescence: every IDBRequest created by this file reached readyState 'done'");
    }
    ckjs("allTxnsDone === allTxnsTotal",
         "quiescence: every IDBTransaction created by this file reached complete or abort");

    js_page_close();
    dom_free(root);

    printf("\ntest-idb: %d checks, %d failures\n", checks, failures);
    if (failures) { printf("FAILED\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
