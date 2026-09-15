# Form submission inside a native keydown handler

Tests only changed: tests/unit/navigation_base_test.c and tests/navigation_base.mk. Production was changed by the parent. The pre-fix path was preserved with BROWSER_FORM_SUBMIT_SYNC_LOAD and was observed failing before the positive run.

The existing real app_main fixture edits an uncommitted address, clicks the actual input's painted text to focus it, then delivers native EV_KEY Enter. The ordinary keydown listener prevents the Enter default, records BEFORE, calls form.submit() or requestSubmit(), then records AFTER. A host callback only preserves observation counters across document replacement; it cannot dispatch input, submit, or navigate. The real JS forms installer is linked and its two APIs are asserted before activation. This source was absent from the prior native-only navigation harness, so its link was expanded rather than mistaking a missing JS adapter for the product crash.

Negative evidence in both modes: BEFORE is recorded (requestSubmit additionally records a submit event), FORM-GET and the absolute destination load occur, AFTER is never recorded, and the process aborts with `list_empty(&rt->gc_obj_list)` in JS_FreeRuntime. The strict negative target now requires exit 134, that exact assertion signature, FORM-GET, BEFORE, and absence of AFTER. A generic exit 139 or unrelated crash is rejected. `make BUILD=build test-navigation-form-stack-negctl` passed this stricter control, with its output preserved in navigation-form-strict-negctl.log.

Current production: all five new modes pass in 44 bounded virtual polls each:
- key-submit: handler continues; zero submit events; exact destination request and actual destination paint.
- key-request-submit: handler continues; exactly one submit event; same navigation and paint checks.
- key-same-url: exactly two requests for the original full URL (initial load plus submission), with the same resource repainted. No alternate URL or synthetic response disguises a same-document shortcut.
- key-submit-then-location: only the later location destination is fetched.
- key-location-then-submit: only the later form destination is fetched.

The shared-producer cases each require exactly two total document requests, so fetching an intermediate losing destination cannot pass. Every case verifies the handler began and returned once, the committed URL, the painted destination, bounded observations and normal app_main shutdown.

`make BUILD=build test-navigation-base` also exited 0 for the expanded aggregate and all four original native paths. Its BROWSER_NAV_USES_ADDRESS_EDIT control remains strict: link, relative-form and no-action-form each produce exactly three designated failures; tab-link produces four, including saved tab URL corruption, while actual tab roundtrip execution is required. No existing negative gate was removed or weakened.

Artifacts: navigation-form-old-repro.log (initial old crash), navigation-form-integrated.log (new five-mode gate), navigation-base-form-expanded.log (complete aggregate), navigation-form-strict-negctl.log (strengthened signature check), and navigation-base/current-*.log / form-sync-old-*.log. This is real host app_main/JS/DOM/native-event pipeline evidence, not guest pixels or network acceptance. No disk image or production file was modified by this task.
