/* An ordinary database upgrade must expose a newly created store through the
 * upgrade transaction immediately. Reuse the real page/event/timer fixture:
 * a mock IDB implementation or a synchronous fake success event would miss
 * the path this gate is intended to exercise. All records here are invented. */
#define main original_idb_main
#include "webapi_idb_test.c"
#undef main

int main(void)
{
    struct node *root=dom_parse(PAGE,(int)strlen(PAGE));
    if(!root)return 2;
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/idb-upgrade.html");
    if(!js_page_open(root))return 2;
    ctx=js_page_ctx();
    run("var observed={}, reqs=[], completed=0;"
        "function request(r){reqs.push(r);return r;}"
        "function transaction(t){t.oncomplete=function(){completed++;};return t;}"
        "function missing(t,n){try{t.objectStore(n);return false;}catch(e){return e.name==='NotFoundError';}}"
        "var first=request(indexedDB.open('ordinary-upgrade',1));"
        "first.onupgradeneeded=function(){"
        " var t=transaction(first.transaction), d=first.result;"
        " observed.initialEmpty=t.objectStoreNames.length===0;"
        " var calls=0, method=t.objectStore;"
        " t.objectStore=function(){calls++;return method.apply(this,arguments);};"
        " var a=d.createObjectStore('zeta',{keyPath:'id'});"
        " observed.creationInternal=calls===0;delete t.objectStore;"
        " observed.afterCreate=t.objectStoreNames.contains('zeta');"
        " var looked;try{looked=t.objectStore('zeta');observed.lookup=true;}catch(e){observed.lookup=false;}"
        " observed.identity=looked===a;"
        " observed.owner=a.transaction===t;"
        " d.createObjectStore('alpha',{keyPath:'id'});"
        " observed.sorted=Array.from(t.objectStoreNames).join(',')==='alpha,zeta';"
        " observed.dbMatches=Array.from(d.objectStoreNames).join(',')===Array.from(t.objectStoreNames).join(',');"
        " if(looked)request(looked.put({id:7,value:42}));"
        " observed.unknownRejected=missing(t,'missing');"
        "};first.onsuccess=function(){window.db=first.result;observed.opened=true;};");
    pump_until_idle(80);
    ckjs("observed.initialEmpty","new upgrade starts with an empty store scope");
    ckjs("observed.creationInternal","creation does not invoke a public objectStore wrapper");
    ckjs("observed.afterCreate","created store immediately joins upgrade transaction scope");
    ckjs("observed.lookup","created store is found through the upgrade transaction");
    ckjs("observed.identity","createObjectStore and transaction lookup share one handle");
    ckjs("observed.owner","created handle belongs to the upgrade transaction");
    ckjs("observed.sorted","upgrade scope is sorted after two store creations");
    ckjs("observed.dbMatches","database and upgrade transaction expose the same current names");
    ckjs("observed.unknownRejected","an unknown upgrade store still throws NotFoundError");
    ckjs("observed.opened&&first.error===null&&completed===1","ordinary initial upgrade commits and open succeeds");

    run("var read=transaction(db.transaction('zeta'));"
        "var result=request(read.objectStore('zeta').get(7));"
        "var scoped=transaction(db.transaction('alpha'));"
        "observed.outsideRejected=missing(scoped,'zeta');"
        "observed.normalScope=Array.from(scoped.objectStoreNames).join(',')==='alpha';");
    pump_until_idle(80);
    ckjs("result.readyState==='done'&&result.result&&result.result.value===42","write through upgrade lookup survives into a later transaction");
    ckjs("observed.outsideRejected","existing store outside a normal transaction still throws NotFoundError");
    ckjs("observed.normalScope","normal transaction retains only its requested scope");

    run("db.close();var second=request(indexedDB.open('ordinary-upgrade',2));"
        "second.onupgradeneeded=function(){"
        " var t=transaction(second.transaction),d=second.result;"
        " observed.secondInitial=Array.from(t.objectStoreNames).join(',')==='alpha,zeta';"
        " var previous=t.objectStore('zeta');"
        " d.deleteObjectStore('zeta');"
        " observed.deletedScope=!t.objectStoreNames.contains('zeta');"
        " observed.deletedRejected=missing(t,'zeta');"
        " var replacement=d.createObjectStore('zeta',{keyPath:'id'});"
        " observed.replacement=!!replacement&&replacement!==previous;"
        " observed.recreatedIdentity=t.objectStore('zeta')===replacement;"
        " d.createObjectStore('beta',{keyPath:'id'});"
        " observed.newMember=t.objectStoreNames.contains('beta');"
        " observed.finalOrder=Array.from(t.objectStoreNames).join(',')==='alpha,beta,zeta';"
        " var beta;try{beta=t.objectStore('beta');observed.secondLookup=true;}catch(e){observed.secondLookup=false;}"
        " if(beta)request(beta.put({id:1,value:24}));"
        " request(replacement.put({id:1,value:42}));"
        "};second.onsuccess=function(){window.db=second.result;observed.secondOpened=true;};");
    pump_until_idle(80);
    ckjs("observed.secondInitial","later upgrade begins with all existing stores in scope");
    ckjs("observed.deletedScope","deleting a store removes it from upgrade scope immediately");
    ckjs("observed.deletedRejected","lookup of a deleted store still throws NotFoundError");
    ckjs("observed.replacement","delete and recreate produces a different store handle");
    ckjs("observed.recreatedIdentity","recreated store shares its handle with transaction lookup");
    ckjs("observed.newMember","additional store joins a nonempty upgrade scope");
    ckjs("observed.finalOrder","delete and recreate leave a sorted current scope");
    ckjs("observed.secondLookup","new store in a later upgrade is immediately addressable");
    ckjs("observed.secondOpened&&second.error===null&&db.version===2","later ordinary upgrade completes at the requested version");
    run("var final=transaction(db.transaction(['beta','zeta']));"
        "var betaResult=request(final.objectStore('beta').get(1));"
        "var replacementResult=request(final.objectStore('zeta').get(1));");
    pump_until_idle(80);
    ckjs("betaResult.result&&betaResult.result.value===24","later upgrade lookup writes are readable after commit");
    ckjs("replacementResult.result&&replacementResult.result.value===42","recreated store contains the new ordinary record");
    ckjs("reqs.every(function(r){return r.readyState==='done'&&r.error===null;})","all ordinary requests settle successfully");
    ckjs("completed===5","all five ordinary transactions complete");
    /* The public fixture's completion count is independent of the host pump;
     * both must agree before destroying the page. */
    ck(!js_page_pending(),"the page has no pending tasks after ordinary requests");
    js_page_close();dom_free(root);
    printf("idb-upgrade-scope: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
