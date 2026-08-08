// Does a fetch of a LOCAL resource complete, and does the answer depend on
// how big the resource is?
//
// This exists to turn a reading of the source into a measurement. Two facts
// about the runner and the fetch path suggest that a data-driven WPT file
// cannot load a large corpus JSON:
//
//   1. c/net/http/http1.c does exactly ONE 4096-byte read per pump ("One read
//      per pump, so a fast server cannot starve the caller's event loop").
//      A 228 KB file therefore needs ~56 turns of the embedder's loop.
//   2. tests/unit/wpt_test.c's drain loop advances a VIRTUAL clock: each turn
//      it asks js_page_next_due() and jumps g_now straight to that deadline.
//      A fetch is not a timer and has no deadline in that queue, so once the
//      only thing scheduled is testharness.js's own timeout, the clock leaps
//      to it and the timeout fires -- however many bytes are still in flight.
//
// If both are true, a small resource loads and a large one reports "Test timed
// out", with nothing else different between the two. That is the whole test.
// It is deliberately not a proof of the mechanism -- it is the observable the
// mechanism predicts, which is what a hand-off to another line needs.

promise_test(function () {
    return fetch('resources/small.json').then(function (r) {
        assert_true(r.ok, 'small.json responded ' + r.status);
        return r.json();
    }).then(function (j) {
        assert_equals(j.length, 50);
    });
}, 'fetch of a small local resource (540 bytes, one read) completes');

promise_test(function () {
    return fetch('resources/big.json').then(function (r) {
        assert_true(r.ok, 'big.json responded ' + r.status);
        return r.json();
    }).then(function (j) {
        assert_equals(j.length, 1200,
            'a 267 KB body needs ~66 reads; if this times out, the drain loop ' +
            'is racing the fetch, not the URL parser failing');
    });
}, 'fetch of a large local resource (267 KB, ~66 reads) completes');
