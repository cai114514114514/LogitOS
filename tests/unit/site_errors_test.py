"""The QQ specimen logged failed fetches and rejected promises yet was PAINTED.
This checks the instrument's log parser, not website compatibility.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "qmp"))
from qmp_site import parse_serial, resource_error_count, latest_painted_text, observed_input_box, native_input_progress, diagnostic_progress
from qmp_ui import browser_client_point

sample = """[browser] load: https://site.invalid/
[webapi] fetch: response exceeds limit
[webapi] prelude failed: InternalError: interrupted
[error] Uncaught (in promise) AxiosError: Network Error
    at request (bundle.js:12)
[error] handled failure shown by page
[webapi] fetch-detail url=https://site.invalid/data boundary=pre-header-hold
[browser] fetch stalled: no response after 12 s: https://site.invalid/a.png
[img] fetch failed (status 0): a.png: stalled: no response
[browser] cannot fetch script.js
[browser] load done: 18 requests, 3 connections dialled, 15 reused, 0 modules loaded (0 failed)
[js] module loaded 10589862 bytes: https://site.invalid/late.js
[module-perf] compile_ms=3 prefetch_ms=77 fetch_ms=111 link_ms=4 eval_jobs_ms=33 compile_bytes=900 compiles=2 fetches=1 prefetch_batches=1
"""
result = parse_serial(sample)
completion_sample = """[browser] inserted-script queue full -- dropping one
[browser] inserted script LOST: https://site.invalid/chunk.js: timeout
[img] REFUSED: decoded image budget
[load-complete] elapsed_ms=1234
[browser] load: about:images
[images] layout total=200 decoded=3 cache_negative=1 pending=0
[images] owed=0 load_event_pending=1
[images] end state
"""
completion = parse_serial(completion_sample)
bare_errors = parse_serial("TypeError: no setter for property 'responseText'\n    at XMLHttpRequest (<webapi>:1579)\n")
paint = """[dl] painted text: 1 run(s), 5 bytes
[dl] ---8<--- begin painted text
[dl] 0,0 Early
[dl] ---8<--- end painted text
[browser] load: about:text
[dl] painted text: 2 run(s), 9 bytes
[dl] ---8<--- begin painted text
[dl] 0,0 Late form
[dl] ---8<--- end painted text
[dl] painted text: 3 run(s), 30 bytes
[dl] ---8<--- begin painted text
"""
late = latest_painted_text(paint)
native_point = browser_client_point(
    '[wm] win 2 frame 100 80 800 600 content 400 280 pt Browser\n', 20, 30)
boxlog = ('[dl] ---8<--- begin boxes\n'
    '[dl] ctrl     195,163    766x42   <textarea> #entry .input-scroll\n'
    '[dl] ---8<--- end boxes (1 shown of 1)\n')
input_box = observed_input_box(boxlog, 'entry')
input_log = '[input-trace] key-default length=1\n[input-trace] frame-painted length=1\n[input-trace] key-default length=2\n'
unpainted = native_input_progress(input_log)
image_end = '[images] end state\r\n'
image_trigger = '[browser] load: about:images\r\n'
partial_images = diagnostic_progress(image_end + image_trigger + '[images] DOM img=4\n', 'images')
input_trigger = '[browser] load: about:input\r\n'
input_armed = '[input-trace] armed point=0,0 remaining=63 guest_ms=123\r\n'
input_checks = {}
for name, log, expected in [
    ('input diagnostic recognizes its complete armed marker', input_trigger + input_armed, (True, True)),
    ('input diagnostic arrival waits for arming', input_trigger, (True, False)),
    ('input diagnostic cannot borrow an older arm', input_armed + input_trigger, (True, False)),
    ('input diagnostic needs a complete line', input_trigger + input_armed.rstrip(), (True, False)),
    ('input diagnostic refuses an unrelated trace stage', input_trigger + '[input-trace] key-default length=1\n', (True, False)),
]:
    try:
        state = diagnostic_progress(log, 'input')
        input_checks[name] = (state['arrived'], state['completed']) == expected
    except KeyError:
        # Missing command registration used to abort the whole real-site run.
        # Report it as a named test failure, not a website input failure.
        input_checks[name] = False
if "--negctl" in sys.argv:
    # Recreate the historical loss at the parser's output boundary: only its
    # newly captured diagnostics disappear; ordinary load counters stay live.
    result["console_errors"] = []
    result["webapi_errors"] = []
    result["fetch_failed"] = []
    result["fetch_stalled"] = []
    result["module_fetch_observations"] = []
    result["module_phases_ms"] = []
    late = latest_painted_text(paint.split('[browser] load: about:text')[0])
    native_point = (20, 30)  # old raw-client-as-device-pixels assumption
    input_box = None  # historical parser missed printf's padded height field
    unpainted['paint_after_defaults_observed'] = True  # old key-default-only wait
    partial_images['completed'] = partial_images['arrived']  # old dispatch-only wait
    input_checks['input diagnostic recognizes its complete armed marker'] = False
    completion['loader_errors'] = []
    completion['load_complete_ms'] = []
    bare_errors['console_errors'] = []
checks = {
    "caught bare Error object remains a page diagnostic": len(bare_errors['console_errors']) == 1,
    "dropped scripts and refused images affect verdict": resource_error_count(completion) == 3,
    "actual load event retained separately": completion['load_complete_ms'] == [1234],
    "pending load remains visible despite earlier load event": completion['load_event_pending'] is True,
    "unobserved completion stays unknown": parse_serial('')['load_event_pending'] is None,
    "partial image inventory cannot claim completion": parse_serial(completion_sample.replace('[images] end state', ''))['image_state'] is None,
    "partial final census line cannot commit inventory": parse_serial(completion_sample.rstrip())['image_state'] is None,
    "later partial image inventory cannot borrow completed state": parse_serial(completion_sample+'[browser] load: about:images\n[images] layout total=5\n')['image_state'] is None,
    "console errors retained": len(result.get("console_errors", [])) == 2,
    "fetch failures retained": result.get("webapi_errors") == [
        "fetch: response exceeds limit", "prelude failed: InternalError: interrupted"],
    "normal load counters intact": result["load_done"] and result["requests"] == 18,
    "image and stall diagnostics affect verdict": resource_error_count(result) == 3,
    "clean page remains clean": not parse_serial("[log] ready\n").get("console_errors"),
    "clean page has no resource failure": resource_error_count(parse_serial("[log] ready\n")) == 0,
    "late module retained beyond initial load counters": result["modules"] == 0 and result["module_fetch_observations"] == [
        {"bytes": 10589862, "url": "https://site.invalid/late.js"}],
    "exclusive module phases retained rather than called script CPU": result["module_phases_ms"] == [
        {"compile_ms": 3, "prefetch_ms": 77, "fetch_ms": 111, "link_ms": 4, "eval_jobs_ms": 33,
         "compile_bytes": 900, "compiles": 2, "fetches": 1, "prefetch_batches": 1}],
    "late page paint retained after nonnavigation diagnostic": late == {
        "text_runs": 2, "text_bytes": 9, "text": "0,0 Late form"},
    "missing paint is not zero painted text": latest_painted_text('') == {"text_runs": None},
    "native point uses current WM scale and titlebar": native_point == (140, 300),
    "padded native control rectangle retained": input_box == (195, 163, 766, 42),
    "explicit class identifies a unique observed control": observed_input_box(boxlog, element_class='input-scroll') == (195, 163, 766, 42),
    "class prefix is not an exact input selector": observed_input_box(boxlog, element_class='input') is None,
    "ambiguous class selector is refused": observed_input_box(boxlog.replace('[dl] ---8<--- end boxes',
        '[dl] ctrl 10,10 20x20 <textarea> .input-scroll\n[dl] ---8<--- end boxes'), element_class='input-scroll') is None,
    "ambiguous control IDs refused": observed_input_box(boxlog.replace('[dl] ---8<--- end boxes',
        '[dl] ctrl 10,10 20x20 <input> #entry\n[dl] ---8<--- end boxes'), 'entry') is None,
    "native defaults do not prove subsequent paint": unpainted == {
        'native_defaults_observed': 2, 'paint_after_defaults_observed': False},
    "only paint after latest default completes observation": native_input_progress(input_log+'[input-trace] frame-painted length=2\n') == {
        'native_defaults_observed': 2, 'paint_after_defaults_observed': True},
    "missing input trace is not completed painting": native_input_progress('') == {
        'native_defaults_observed': 0, 'paint_after_defaults_observed': False},
    "diagnostic arrival cannot borrow an older completed image census": partial_images == {
        'arrived': True, 'completed': False},
    "complete image census is confirmed after dispatch": diagnostic_progress(image_trigger + image_end, 'images') == {
        'arrived': True, 'completed': True},
    "partial terminator is not a completed census": not diagnostic_progress(image_trigger + image_end.rstrip(), 'images')['completed'],
    "latest incomplete diagnostic cannot borrow previous output": not diagnostic_progress(image_trigger + image_end + image_trigger, 'images')['completed'],
    "automatic text dump is not an arrived diagnostic": diagnostic_progress(paint.split('[browser] load:')[0], 'text') == {
        'arrived': False, 'completed': False},
    "box and text terminators are not interchangeable": not diagnostic_progress('[browser] load: about:boxes\n[dl] ---8<--- end painted text\n', 'boxes')['completed'],
    "complete native box census retains padded counts": diagnostic_progress('[browser] load: about:boxes\n[dl] ---8<--- end boxes (9 shown of 120)\n', 'boxes')['completed'],
}
checks.update(input_checks)
for name, ok in checks.items():
    print(("PASS " if ok else "FAIL ") + name)
sys.exit(0 if all(checks.values()) else 1)
