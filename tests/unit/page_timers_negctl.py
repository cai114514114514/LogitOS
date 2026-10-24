"""Restore each historical timer defect in an isolated compilation unit.

The old lifetime control changed only the profiler's label to read best->raf.
That read is valid now: stable running ownership retains best until its full
checkpoint finishes. Explicitly restore the old early disposal before reading
the label, or the control would pass without ever exercising a freed entry.
"""
import pathlib
import sys

mode, source, target = sys.argv[1:]
text = pathlib.Path(source).read_text()
changes = {
    "this": ("JSValue receiver = is_raf ? JS_UNDEFINED : JS_GetGlobalObject(g_ctx);",
             "JSValue receiver = JS_UNDEFINED;"),
    "lifetime": ('js_prof_label(is_raf ? "<rAF callback>" : "<timer callback>");',
                 'if (!is_raf && !best->timer.repeat) timer_free(g_ctx, best);\n'
                 '        js_prof_label(best->raf ? "<rAF callback>" : "<timer callback>");'),
    "starvation": (
        """    if (finish_previous && PAGE_WEBAPI_HAVE(js_webapi_fetch_checkpoint)) {
        int n = js_webapi_fetch_checkpoint(g_ctx);
        if (n > 0) { js_dom_run_jobs(g_ctx); ran += n; }
    }
""",
        ""),
}
before, after = changes[mode]
if text.count(before) != 1:
    raise SystemExit(f"timer negative control {mode}: mutation site drifted")
# The copied translation unit no longer resides beside js/runtime/page.c.
# Resolve its nested queue include through BTEST_INC's browser root; retaining
# ../../app/page.c here tested relocation failure instead of the injected bug.
text = text.replace('#include "../../app/page.c"', '#include "app/page.c"')
pathlib.Path(target).write_text(text.replace(before, after))
