// Event-loop ordering, written in WPT's own format and run by the WPT runner.
//
// WHY THIS FILE IS OURS AND NOT UPSTREAM'S. The vendored corpus has no
// html/webappapis/timers and no html/webappapis/microtask-queuing -- the
// subsets tools/wpt_fetch.sh takes do not include them. So the question the
// events line and the WPT line both need answered ("is task/microtask ordering
// right, and is it what is killing async_test?") has no upstream data behind
// it, and answering it by reading js_page.c is an argument, not a measurement.
//
// These are the assertions the HTML Standard's "perform a microtask
// checkpoint" makes, restated: they are not new requirements, and every one of
// them is something a testharness.js async_test or promise_test depends on.
//
// Run:  build/wpt_test --root tests/wpt-local --subset platform -b /dev/null

// ---- 1. microtasks drain BETWEEN tasks, not after all of them --------------
// The one that matters most. If a run of due timers fires back to back and
// only then drains the promise job queue, then a .then() scheduled by the
// first timer runs after the second timer -- and every promise_test whose
// resolution is chained off a setTimeout reports its steps in the wrong order.
async_test(function (t) {
    var log = [];
    setTimeout(t.step_func(function () {
        log.push('task1');
        Promise.resolve().then(t.step_func(function () { log.push('micro1'); }));
    }), 0);
    setTimeout(t.step_func(function () {
        log.push('task2');
    }), 0);
    setTimeout(t.step_func_done(function () {
        assert_array_equals(log, ['task1', 'micro1', 'task2'],
            'a microtask queued by task1 must run before task2 starts');
    }), 30);
}, 'microtask checkpoint happens after each task, not after the batch');

// ---- 2. same-deadline timers fire in registration order --------------------
// The clock here advances in coarse steps, so "same deadline" is the common
// case rather than a rare tie, and a linear scan with no tiebreaker would make
// this a coin flip.
async_test(function (t) {
    var log = [];
    for (var i = 0; i < 5; i++) {
        (function (n) { setTimeout(t.step_func(function () { log.push(n); }), 0); })(i);
    }
    setTimeout(t.step_func_done(function () {
        assert_array_equals(log, [0, 1, 2, 3, 4]);
    }), 30);
}, 'timers with equal deadlines fire in the order they were created');

// ---- 3. a longer delay does not overtake a shorter one ---------------------
async_test(function (t) {
    var log = [];
    setTimeout(t.step_func(function () { log.push('late'); }), 20);
    setTimeout(t.step_func(function () { log.push('early'); }), 0);
    setTimeout(t.step_func_done(function () {
        assert_array_equals(log, ['early', 'late']);
    }), 60);
}, 'timers fire in deadline order');

// ---- 4. setTimeout passes its extra arguments through ----------------------
// Shipped code relies on this constantly and it is one line to get wrong.
async_test(function (t) {
    setTimeout(t.step_func_done(function (a, b, c) {
        assert_equals(a, 1);
        assert_equals(b, 'two');
        assert_equals(c, undefined, 'only the arguments actually passed');
    }), 0, 1, 'two');
}, 'setTimeout forwards extra arguments to the callback');

// ---- 5. clearTimeout from inside another timer ----------------------------
async_test(function (t) {
    var fired = false;
    var id = setTimeout(t.step_func(function () { fired = true; }), 20);
    setTimeout(t.step_func(function () { clearTimeout(id); }), 0);
    setTimeout(t.step_func_done(function () {
        assert_false(fired, 'a cancelled timer must not fire');
    }), 60);
}, 'clearTimeout cancels a timer that has not fired yet');

// ---- 6. setInterval repeats and clearInterval stops it ---------------------
async_test(function (t) {
    var n = 0;
    var id = setInterval(t.step_func(function () {
        n++;
        if (n === 3) clearInterval(id);
    }), 5);
    setTimeout(t.step_func_done(function () {
        assert_equals(n, 3, 'exactly three ticks, then stopped');
    }), 200);
}, 'setInterval repeats until clearInterval');

// ---- 7. queueMicrotask runs before the next task, and in FIFO order --------
// queueMicrotask is implemented as Promise.resolve().then(fn). That is the
// right ORDER -- both land on the same job queue in call order -- and the
// assertion is here so that if anyone reimplements it on a task source, this
// says so immediately.
async_test(function (t) {
    var log = [];
    setTimeout(t.step_func(function () {
        queueMicrotask(t.step_func(function () { log.push('qm1'); }));
        Promise.resolve().then(t.step_func(function () { log.push('p1'); }));
        queueMicrotask(t.step_func(function () { log.push('qm2'); }));
        log.push('sync');
    }), 0);
    setTimeout(t.step_func_done(function () {
        assert_array_equals(log, ['sync', 'qm1', 'p1', 'qm2'],
            'microtasks run after the current task, in the order queued');
    }), 40);
}, 'queueMicrotask interleaves FIFO with promise jobs');

// ---- 8. a microtask queued from a microtask runs in the same checkpoint ----
// The checkpoint drains until the queue is EMPTY; it does not take a snapshot.
// An implementation that drains a snapshot passes every test above and fails
// every framework that chains promises.
async_test(function (t) {
    var log = [];
    setTimeout(t.step_func(function () {
        Promise.resolve().then(t.step_func(function () {
            log.push('a');
            Promise.resolve().then(t.step_func(function () { log.push('b'); }));
        }));
    }), 0);
    setTimeout(t.step_func(function () { log.push('nexttask'); }), 10);
    setTimeout(t.step_func_done(function () {
        assert_array_equals(log, ['a', 'b', 'nexttask'],
            'a microtask queued from a microtask runs before the next task');
    }), 50);
}, 'the microtask checkpoint drains to empty, not to a snapshot');

// ---- 9. requestAnimationFrame -------------------------------------------
// rAF has its own handle space, per spec: cancelAnimationFrame(id) and
// clearTimeout(id) are not interchangeable, and a page that animates while a
// timer is pending must not have one cancel the other.
async_test(function (t) {
    assert_equals(typeof requestAnimationFrame, 'function', 'requestAnimationFrame exists');
    requestAnimationFrame(t.step_func_done(function (ts) {
        assert_equals(typeof ts, 'number', 'the callback gets a DOMHighResTimeStamp');
        assert_true(ts >= 0);
    }));
}, 'requestAnimationFrame fires with a numeric timestamp');

async_test(function (t) {
    var fired = false;
    var id = requestAnimationFrame(t.step_func(function () { fired = true; }));
    cancelAnimationFrame(id);
    setTimeout(t.step_func_done(function () {
        assert_false(fired, 'a cancelled animation frame must not fire');
    }), 100);
}, 'cancelAnimationFrame cancels a pending frame');

// ---- 10. rAF callbacks registered together share one timestamp ------------
async_test(function (t) {
    var seen = [];
    requestAnimationFrame(t.step_func(function (ts) { seen.push(ts); }));
    requestAnimationFrame(t.step_func(function (ts) {
        seen.push(ts);
        t.step(function () {
            assert_equals(seen.length, 2);
            assert_equals(seen[0], seen[1],
                'callbacks in one frame get the same timestamp, or animations drift apart');
        });
        t.done();
    }));
}, 'requestAnimationFrame batches callbacks into one frame');

// ---- 11. requestIdleCallback ---------------------------------------------
async_test(function (t) {
    assert_equals(typeof requestIdleCallback, 'function', 'requestIdleCallback exists');
    requestIdleCallback(t.step_func_done(function (deadline) {
        assert_equals(typeof deadline, 'object');
        assert_equals(typeof deadline.timeRemaining, 'function',
            'the deadline argument must have timeRemaining()');
    }));
}, 'requestIdleCallback fires with a deadline object');
