"""Restore each historical timer defect in an isolated compilation unit."""
import pathlib
import sys

mode, source, target = sys.argv[1:]
text = pathlib.Path(source).read_text()
changes = {
    "this": ("JSValue receiver = is_raf ? JS_UNDEFINED : JS_GetGlobalObject(g_ctx);",
             "JSValue receiver = JS_UNDEFINED;"),
    "lifetime": ('js_prof_label(is_raf ? "<rAF callback>" : "<timer callback>");',
                 'js_prof_label(best->raf ? "<rAF callback>" : "<timer callback>");'),
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
pathlib.Path(target).write_text(text.replace(before, after))
