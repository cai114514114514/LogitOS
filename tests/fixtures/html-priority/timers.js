/* Classic-script fixture shared by Window and WorkerGlobalScope.
 * Load with <script src=...> or importScripts(...), then await
 * testHTMLTimers((condition, label) => record(condition, label)). No autorun.
 * Timings use the guest's performance clock only to assert HTML's 4 ms minimum;
 * these are scheduling semantics, not throughput or host wall-time measurements. */
globalThis.testHTMLTimers = async function testHTMLTimers(check) {
    let checks = 0;
    function verify(condition, label) {
        checks++;
        check(Boolean(condition), label);
    }

    function capture(call) {
        try { call(); return undefined; } catch (error) { return error; }
    }

    /* A broken timer must produce a failing observation, not leave the whole
     * guest gate awaiting a Promise forever. The guard is a guest timer too;
     * the outer harness still owns the final limit if no tasks run at all. */
    function observe(register) {
        return new Promise(resolve => {
            let finished = false;
            let cleanup = () => {};
            const guard = setTimeout(() => finish({ timedOut: true }), 500);
            function finish(value) {
                if (finished) return;
                finished = true;
                clearTimeout(guard);
                cleanup();
                resolve(value);
            }
            cleanup = register(finish) || (() => {});
        });
    }

    const conversionError = { marker: "timeout conversion" };
    for (const name of ["setTimeout", "setInterval"]) {
        verify(capture(() => globalThis[name](() => {}, Symbol("delay"))) instanceof TypeError,
               name + " rejects a Symbol timeout");
        verify(capture(() => globalThis[name](() => {}, 1n)) instanceof TypeError,
               name + " rejects a BigInt timeout");
        verify(capture(() => globalThis[name](() => {}, {
            valueOf() { throw conversionError; }
        })) === conversionError, name + " propagates the exact timeout conversion exception");
        verify(capture(() => globalThis[name]()) instanceof TypeError,
               name + " requires a handler");
        verify(capture(() => globalThis[name]({
            toString() { throw conversionError; }
        }, 0)) === conversionError, name + " preserves handler conversion exceptions");
        verify(capture(() => globalThis[name]("void 0", 0)) instanceof TypeError,
               name + " explicitly refuses the unsupported TrustedScript/CSP string path");
    }

    for (const name of ["clearTimeout", "clearInterval"]) {
        verify(capture(() => globalThis[name](Symbol("id"))) instanceof TypeError,
               name + " rejects a Symbol ID");
        verify(capture(() => globalThis[name](1n)) instanceof TypeError,
               name + " rejects a BigInt ID");
        verify(capture(() => globalThis[name]({
            valueOf() { throw conversionError; }
        })) === conversionError, name + " propagates the exact ID conversion exception");
        verify(capture(() => globalThis[name](-2147483648.5)) === undefined,
               name + " converts a fractional negative long boundary without overflow");
    }

    for (const delay of [4294967296, 4294967295, 2147483648, -2147483648.5, 1.9, -1, NaN, Infinity]) {
        const outcome = await observe(done => {
            const fast = setTimeout(() => done("converted"), delay);
            const slow = setTimeout(() => done("guard"), 100);
            return () => { clearTimeout(fast); clearTimeout(slow); };
        });
        verify(outcome === "converted", "Web IDL long conversion for delay " + String(delay));
    }

    const argument = { marker: "owned argument" };
    const callback = await observe(done => {
        const id = setTimeout(function () {
            "use strict";
            done({ receiver: this, args: Array.from(arguments) });
        }, 0, argument, 41, "tail");
        return () => clearTimeout(id);
    });
    verify(callback.receiver === globalThis, "strict timer callback receives this global");
    verify(callback.args && callback.args.length === 3 && callback.args[0] === argument &&
           callback.args[1] === 41 && callback.args[2] === "tail",
           "timer preserves extra argument count, order and object identity");

    let cancelledCalls = 0;
    const timeout = setTimeout(() => cancelledCalls++, 0);
    const interval = setInterval(() => cancelledCalls++, 0);
    clearInterval(timeout);
    clearTimeout(interval);
    await observe(done => { setTimeout(() => done(true), 0); });
    verify(cancelledCalls === 0, "clearTimeout and clearInterval share their ID map");

    const cancelledBeforeDispatch = await observe(done => {
        let victim;
        const first = setTimeout(() => clearTimeout(victim), 0);
        victim = setTimeout(() => cancelledCalls++, 0);
        const last = setTimeout(() => done(cancelledCalls), 0);
        return () => { clearTimeout(first); clearTimeout(victim); clearTimeout(last); };
    });
    verify(cancelledBeforeDispatch === 0, "an earlier due callback cancels a later frozen-batch timer");

    const repeat = await observe(done => {
        const times = [];
        let validArgs = true;
        let validReceiver = true;
        const id = setInterval(function (value, tail) {
            "use strict";
            validArgs = validArgs && arguments.length === 2 && value === argument && tail === "tail";
            validReceiver = validReceiver && this === globalThis;
            times.push(performance.now());
            if (times.length === 8) done({ times, validArgs, validReceiver });
        }, 0, argument, "tail");
        return () => clearInterval(id);
    });
    verify(repeat.times && repeat.times.length === 8, "setInterval with timeout zero keeps repeating");
    verify(repeat.validArgs && repeat.validReceiver, "each repeat retains arguments and its global receiver");
    verify(repeat.times && repeat.times[6] - repeat.times[5] >= 4 &&
           repeat.times[7] - repeat.times[6] >= 4,
           "interval re-initialization increments nesting and applies the 4 ms minimum");

    const selfCancel = await observe(done => {
        let calls = 0;
        let replacement;
        const id = setInterval(() => {
            calls++;
            clearInterval(id);
            replacement = setTimeout(() => done(calls), 20);
        }, 0);
        return () => { clearInterval(id); clearTimeout(replacement); };
    });
    verify(selfCancel === 1, "self-cancelled interval does not reappear after creating a replacement timer");

    const nested = await observe(done => {
        const waits = [];
        let id;
        function schedule() {
            const start = performance.now();
            id = setTimeout(() => {
                waits.push(performance.now() - start);
                if (waits.length === 8) done(waits);
                else schedule();
            }, 0);
        }
        schedule();
        return () => clearTimeout(id);
    });
    verify(Array.isArray(nested) && nested.length === 8 && nested[6] >= 4 && nested[7] >= 4,
           "nested timeouts apply the minimum when the running task's nesting exceeds five");

    async function checkpointProbe(dispatch) {
        return observe(done => {
            let depth = 0;
            let id;
            let nestedId;
            let microId;
            const order = [];
            let start;
            let microStart;
            function arrived(label) {
                order.push(label);
                if (order.length === 2) done({ order, start, microStart });
            }
            function descend() {
                id = setTimeout(() => {
                    if (++depth < 6) { descend(); return; }
                    start = performance.now();
                    nestedId = setTimeout(() => arrived("nested"), 0);
                    dispatch(() => {
                        microStart = performance.now();
                        microId = setTimeout(() => arrived("microtask"), 0);
                    });
                }, 0);
            }
            descend();
            return () => { clearTimeout(id); clearTimeout(nestedId); clearTimeout(microId); };
        });
    }

    async function verifyCheckpoint(dispatch, label) {
        /* The guest clock has 10 ms ticks. If creation crosses a tick, the
         * nested timer may already have expired before the microtask creates
         * its timer; that order proves nothing. Retry a bounded three times,
         * and name an apparatus failure if no same-tick probe was admitted. */
        for (let attempt = 0; attempt < 3; attempt++) {
            const result = await checkpointProbe(dispatch);
            if (result.timedOut) { verify(false, label + " completed"); return; }
            if (result.microStart - result.start >= 4) continue;
            verify(result.order[0] === "microtask", label + " resets timer nesting for its microtask");
            return;
        }
        verify(false, label + ": guest clock advanced during all three same-tick probes");
    }

    await verifyCheckpoint(run => Promise.resolve().then(run), "timer callback checkpoint");
    if (typeof document !== "undefined") {
        const button = document.createElement("button");
        let run;
        button.addEventListener("click", () => Promise.resolve().then(run));
        /* Use the native EventTarget path available to the core runtime too;
         * activation/default-action coverage belongs to HTMLElement.click's
         * own gate, while this scenario needs only the nested checkpoint. */
        await verifyCheckpoint(callback => { run = callback; button.dispatchEvent(new Event("click")); },
                               "DOM checkpoint entered inside a timer");
    }

    return { checks, nestedWaits: nested, intervalTimes: repeat.times || [] };
};
