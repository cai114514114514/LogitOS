# Async queue regression audit

No production defect was found in these four gates. Only tests/unit/late_callbacks_test.c changed. Existing mk prerequisite wiring and production sources remain unchanged.

The old late-callback observer accepted only wait_idle(0). The new pending-script queue correctly requests a bounded pump wake while work is queued; the sabotaged consumer therefore reaches wait_idle(10), never the observer's old zero-only branch. After 500 polls the test called app_exit directly, which longjmped out of app_main without pending_scripts_reset. Its subsequent js_page_close encountered a pinned script-node JSValue left in the async queue and aborted. This is a harness ownership violation, not the production close path; browser.c releases the queue before closing the runtime on EV_CLOSE.

The adapted test observes the first actual scheduler wait regardless of delay. Its fixture only inserts immediately available inline text, without an external transport/timer/animation dependency, so it remains correct to require script execution/navigation before that wait. It does not demand a pending external script finish before sleeping. After recording its assertions it posts a real EV_CLOSE, exercising the production queue cleanup path. The old failure now reports an ordinary exit 1 instead of aborting or timing out. Its bounded-loop fallback also posts EV_CLOSE rather than escaping through app_exit.

Observed negative evidence, BROWSER_EARLY_CALLBACK_CONSUMERS:
- mode0: direct late navigation missing at wait_ms=0, destination_requests=0.
- mode1: inserted inline code missing at wait_ms=10, destination_requests=0.
- mode2: navigation from inserted inline code missing at wait_ms=10, destination_requests=0.
- all modes stop after four polls, report exactly the designated late-consumer failure, intercept the real wait, and complete native cleanup without abort.

Positive modes 0/1/2/3 all pass, at their first wait_ms=0. Navigation modes record one destination request; inline mode observes window.inserted===42. Mode3 retains the load-time scroll/navigation case. Full output: late-callback-adapted.log and late-callback/{positive,negative}-*.log.

The final two browser-loading FAIL lines in async-browser-regressions.log are deliberately printed by the negative recipe's grep commands. The BROWSER_BLOCKING_IMAGES binary must fail those two checks for the target to succeed. The current positive binary independently passes: 36 virtual polls and zero blocking image drains. No browser_loading source or recipe changes were needed.

Fresh separate make targets all exited 0:
- make BUILD=build test-late-callbacks
- make BUILD=build test-browser-loading
- make BUILD=build test-script-resource-events
- make BUILD=build test-navigation-base

The latter three logs are test-browser-loading-async-recheck.log, test-script-resource-events-async-recheck.log and test-navigation-base-async-recheck.log in this directory. Script-resource records seven requests and the expected resource-event chain; all four navigation-base native paths pass while its old-address-buffer control remains red. These are host pipeline regressions, not guest performance claims. No disk rebuild or VM was run.
