#!/usr/bin/env python3
"""HTML iframe sandboxing on the device: four sandboxed network frames.

Two local origins (two ports on the host, 10.0.2.2 from the guest). The
parent embeds four frames from the child origin:

  1  sandbox="allow-scripts allow-same-origin"   script runs; message round
     trip with the child's real origin; document.cookie, localStorage and a
     same-origin fetch all work.
  2  sandbox="allow-scripts"                     script runs; its origin is
     OPAQUE: parent sees event.origin "null"; a message addressed to the
     child URL's origin is never delivered, "*" is; localStorage,
     sessionStorage and document.cookie throw SecurityError; its fetches go
     out with `Origin: null` (read off the host server's own request log)
     and the same host's non-CORS resource is refused.
  3  sandbox=""                                  the document paints, but no
     script in it runs: no console line, no message, /c3.js never requested.
  4  sandbox="allow-scripts"                     alert/confirm/prompt/print
     show nothing (their text never reaches the console, this browser's only
     dialog surface), confirm false, prompt null, open() null, /popup never
     requested.

Every assertion is classified. POSITIVE ones must hold in both builds.
RESTRICTION ones must hold in the product and must FAIL in the control build
(--expect-flags-ignored: the frame loader ignores every flag). A few are
asserted but NOT DISCRIMINATING, and are labelled so rather than counted:
the parent can never reach a network frame's document (cross-runtime DOM
access is unimplemented, sandboxed or not), and no build ever opens a popup
window or requests one (auxiliary browsing contexts do not exist here).

Local fixture only: no public site, no challenge, no injected script.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_ui import Session  # noqa: E402


def pages(parent_origin, child_origin):
    P, C = parent_origin, child_origin
    common = ("function t(f){try{return String(f())}catch(e){return e.name}}"
              "var P=%s,s=document.getElementById('s'),r=[];" % json.dumps(P))
    return {
        '/parent': ('text/html', """<!doctype html><meta charset=utf-8><title>Sandboxed frames</title>
<style>body{margin:16px;background:#eef2f8;font:16px sans-serif}iframe{display:block;width:460px;height:48px;border:2px solid #315591;margin:6px 0}</style>
<h1>SBX-PARENT</h1>
<iframe id=f1 sandbox="allow-scripts allow-same-origin" src="%(C)s/case1"></iframe>
<iframe id=f2 sandbox="allow-scripts" src="%(C)s/case2"></iframe>
<iframe id=f3 sandbox="" src="%(C)s/case3"></iframe>
<iframe id=f4 sandbox="allow-scripts" src="%(C)s/case4"></iframe>
<script>
var CHILD=%(Cj)s;
var w=[1,2,3,4].map(function(i){return document.getElementById('f'+i).contentWindow});
var f2=document.getElementById('f2'),doc2,throws2='none';
try{doc2=f2.contentDocument===null?'null':'object'}catch(e){doc2='threw-'+e.name}
try{f2.contentWindow.document}catch(e){throws2=e.name}
console.log('SBX-P2-DOC contentDocument='+doc2+' windowDocument='+throws2);
addEventListener('message',function(e){
  var i=w.indexOf(e.source)+1,d=String(e.data);
  console.log('SBX-P'+i+' origin='+e.origin+' data='+d);
  if(i===1&&d.indexOf('c1|')===0)e.source.postMessage('c1-reply',CHILD);
  if(i===2&&d.indexOf('c2|')===0){e.source.postMessage('c2-leak',CHILD);e.source.postMessage('c2-reply','*');}
});
</script>""" % {'C': C, 'Cj': json.dumps(C)}),
        '/case1': ('text/html', "<!doctype html><body style='margin:4px;font:14px sans-serif'>"
                   "<span id=s>SBX-C1-PENDING</span><script src='/case1.js'></script>"),
        '/case1.js': ('text/javascript', common +
                      "r.push('cookie='+t(function(){document.cookie='sbx1=yes';return document.cookie.indexOf('sbx1=yes')>=0}));"
                      "r.push('storage='+t(function(){localStorage.setItem('sbx1','v1');return localStorage.getItem('sbx1')}));"
                      "r.push('origin='+t(function(){return self.origin}));"
                      "fetch('/data.txt?case=1').then(function(x){return x.text()}).then(function(v){r.push('fetch=ok:'+v)},"
                      "function(e){r.push('fetch=err:'+e.name)}).then(function(){parent.postMessage('c1|'+r.join(','),P)});"
                      "addEventListener('message',function(e){if(e.origin===P&&e.source===parent&&e.data==='c1-reply'){"
                      "s.textContent='SBX-C1-PASS';console.log('SBX-C1-ROUNDTRIP-PASS')}});"),
        '/case2': ('text/html', "<!doctype html><body style='margin:4px;font:14px sans-serif'>"
                   "<span id=s>SBX-C2-PENDING</span><script src='/case2.js'></script>"),
        '/case2.js': ('text/javascript', common +
                      "r.push('ls='+t(function(){return typeof localStorage.getItem}));"
                      "r.push('ss='+t(function(){return typeof sessionStorage.getItem}));"
                      "r.push('cookie='+t(function(){return typeof document.cookie}));"
                      "r.push('setcookie='+t(function(){document.cookie='sbx2=1';return 'set'}));"
                      "r.push('origin='+t(function(){return self.origin}));"
                      "function f(u){return fetch(u).then(function(x){return x.text()}).then(function(v){return 'ok:'+v},function(e){return 'err:'+e.name})}"
                      "f('/data.txt?case=2').then(function(v){r.push('same='+v);return f('/cors.txt?case=2')}).then(function(v){"
                      "r.push('cors='+v);parent.postMessage('c2|'+r.join(','),'*')});"
                      "addEventListener('message',function(e){if(e.data==='c2-leak'){console.log('SBX-C2-LEAKED');return}"
                      "if(e.data==='c2-reply'){s.textContent='SBX-C2-REPLIED';"
                      "console.log('SBX-C2-REPLY origin='+e.origin+' source='+(e.source===parent))}});"),
        '/case3': ('text/html', "<!doctype html><body style='margin:4px;font:14px sans-serif'>"
                   "<span>SBX-C3-DOCUMENT</span> <span id=s>SBX-C3-INERT</span>"
                   "<script>document.getElementById('s').textContent='SBX-C3-SCRIPT-DID-RUN';"
                   "console.log('SBX-C3-SCRIPT-RAN');parent.postMessage('c3','*')</script>"
                   "<script src='/c3.js'></script>"),
        '/c3.js': ('text/javascript', "console.log('SBX-C3-EXTERNAL-RAN')"),
        '/case4': ('text/html', "<!doctype html><body style='margin:4px;font:14px sans-serif'>"
                   "<span id=s>SBX-C4-PENDING</span><script src='/case4.js'></script>"),
        '/case4.js': ('text/javascript', common +
                      "r.push('alert='+t(function(){return alert('SBX-C4-ALERT-TEXT')}));"
                      "r.push('confirm='+t(function(){return confirm('SBX-C4-CONFIRM-TEXT')}));"
                      "r.push('prompt='+t(function(){return prompt('SBX-C4-PROMPT-TEXT','d')}));"
                      "r.push('print='+t(function(){return print('SBX-C4-PRINT-TEXT')}));"
                      "r.push('open='+t(function(){return open('/popup?case=4')}));"
                      "s.textContent='SBX-C4-DONE';console.log('SBX-C4-RESULT '+r.join(','));"),
        '/data.txt': ('text/plain', 'child-data'),
        '/cors.txt': ('text/plain', 'cors-data'),
        '/popup': ('text/html', '<!doctype html><p>SBX-POPUP'),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--disk', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--expect-flags-ignored', action='store_true',
                    help='negative control: every RESTRICTION assertion must fail, every POSITIVE one pass')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / 'serial.log'
    serial.write_text('')
    requests = []
    result = {'passed': False, 'control': args.expect_flags_ignored, 'requests': requests}
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            path, _, query = self.path.partition('?')
            with lock:
                requests.append({'port': self.server.server_port, 'path': path, 'query': query,
                                 'origin': self.headers.get('Origin'), 'cookie': self.headers.get('Cookie')})
            table = pages('http://10.0.2.2:%d' % parent_server.server_port,
                          'http://10.0.2.2:%d' % child_server.server_port)
            if path not in table or (path == '/parent') != (self.server is parent_server):
                self.send_error(404)
                return
            mime, text = table[path]
            body = text.encode()
            self.send_response(200)
            self.send_header('Content-Type', mime + ('; charset=utf-8' if mime == 'text/html' else ''))
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            if path == '/cors.txt':
                self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(body)

    parent_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    child_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    for server in (parent_server, child_server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    P = 'http://10.0.2.2:%d' % parent_server.server_port
    C = 'http://10.0.2.2:%d' % child_server.server_port
    result.update(parent_origin=P, child_origin=C)

    proc = None

    def log():
        return serial.read_text(errors='replace')

    def wait(pattern, seconds=240, since=0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            m = re.search(pattern, log()[since:], re.M)
            if m:
                return m
            if proc.poll() is not None:
                raise RuntimeError('owned guest exited')
            time.sleep(.25)
        raise RuntimeError('missing guest evidence: ' + pattern)

    tmp = tempfile.mkdtemp(prefix='sbx-qmp-', dir='/tmp')
    sock = str(Path(tmp) / 'qmp.sock')
    cmd = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
           '-cdrom', str(Path(args.iso).resolve()), '-drive',
           'file=' + str(Path(args.disk).resolve()) + ',format=raw,if=none,id=hd0',
           '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd',
           '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
           '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none',
           '-no-reboot', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
           '-serial', 'file:' + str(serial), '-qmp', 'unix:' + sock + ',server,nowait']
    (out / 'launch.json').write_text(json.dumps(cmd, indent=2))
    checks = []

    def check(kind, name, ok, evidence=''):
        checks.append({'kind': kind, 'name': name, 'ok': bool(ok), 'evidence': evidence})

    try:
        with (out / 'qemu.log').open('w') as qlog:
            proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT, start_new_session=True)
        wait('desktop live')
        # Finder's boot launch follows "desktop live"; let it finish so the
        # dock click below is attributed to the browser, not to Finder.
        wait(r'^\[wm\] launched Finder\r?$')
        ui = Session(sock, serial=str(serial))
        ui.launch_app('browser')
        ui.key_mods(('ctrl',), 't', settle=.3)
        ui.typ(P + '/parent')
        ui.key('ret')
        wait(r'SBX-C1-ROUNDTRIP-PASS')
        wait(r'SBX-P2 origin=')
        wait(r'SBX-C2-REPLY origin=')
        wait(r'SBX-C4-RESULT ')
        wait(r'^\[dl\] -?\d+,-?\d+ SBX-C3-DOCUMENT\r?$')
        # Stragglers: frame 3's script, a delivered leak, a late request.
        time.sleep(10)
        try:
            from PIL import Image
            ui.screendump(str(out / 'frames.ppm'))
            Image.open(out / 'frames.ppm').save(out / 'frames.png')
        except Exception as exc:  # the picture is evidence, not a verdict
            result['screendump_error'] = str(exc)
        text = log()
        with lock:
            reqs = list(requests)

        def line(pattern):
            m = re.search(pattern, text, re.M)
            return m.group(0).strip() if m else ''

        def req(path, case):
            return [r for r in reqs if r['path'] == path and r['query'] == 'case=%s' % case]

        # ---- frame 1: allow-scripts allow-same-origin ----
        p1 = line(r'SBX-P1 origin=[^\r\n]*')
        check('positive', 'f1 script runs and the parent sees the child origin', p1.startswith('SBX-P1 origin=%s data=c1|' % C), p1)
        check('positive', 'f1 cookie, storage and self.origin behave as the unsandboxed origin',
              'cookie=true,storage=v1,origin=%s,' % C in p1, p1)
        check('positive', 'f1 same-origin fetch succeeds', 'fetch=ok:child-data' in p1, p1)
        check('positive', 'f1 message round trip', 'SBX-C1-ROUNDTRIP-PASS' in text)
        # ---- frame 2: allow-scripts only: opaque origin ----
        p2 = line(r'SBX-P2 origin=[^\r\n]*')
        check('positive', 'f2 script runs and its message reaches the parent', 'data=c2|' in p2, p2)
        check('restriction', 'f2 message origin is "null"', p2.startswith('SBX-P2 origin=null data='), p2)
        check('restriction', 'f2 localStorage/sessionStorage/document.cookie get+set throw SecurityError',
              'ls=SecurityError,ss=SecurityError,cookie=SecurityError,setcookie=SecurityError,' in p2, p2)
        check('restriction', 'f2 self.origin is "null"', ',origin=null,' in p2, p2)
        check('restriction', 'f2 same-host fetch without CORS opt-in is refused (cross-origin to everything)',
              'same=err:TypeError' in p2, p2)
        check('positive', 'f2 fetch with an ACAO:* opt-in succeeds', 'cors=ok:cors-data' in p2, p2)
        # An ABSENT header is spelled out: printing it as JSON null would read
        # exactly like the "null" origin this check exists to find.
        origins = [r['origin'] if r['origin'] is not None else '(no Origin header)'
                   for r in req('/data.txt', 2) + req('/cors.txt', 2)]
        check('restriction', 'f2 requests carry Origin: null (server log)', len(origins) == 2 and all(o == 'null' for o in origins),
              json.dumps(origins))
        reply = line(r'SBX-C2-REPLY origin=[^\r\n]*')
        check('positive', 'f2 receives the parent\'s "*" message with the parent origin and source',
              reply == 'SBX-C2-REPLY origin=%s source=true' % P, reply)
        check('restriction', 'f2 never receives a message addressed to its URL origin', 'SBX-C2-LEAKED' not in text)
        doc2 = line(r'SBX-P2-DOC [^\r\n]*')
        check('nondiscriminating', 'parent cannot reach f2: contentDocument null, contentWindow.document SecurityError',
              doc2 == 'SBX-P2-DOC contentDocument=null windowDocument=SecurityError', doc2)
        # ---- frame 3: sandbox="" ----
        check('positive', 'f3 document is fetched and painted', bool(re.search(r'^\[dl\] -?\d+,-?\d+ SBX-C3-DOCUMENT\r?$', text, re.M)))
        check('restriction', 'f3 inline script never runs', 'SBX-C3-SCRIPT-RAN' not in text and 'SBX-C3-SCRIPT-DID-RUN' not in text)
        check('restriction', 'f3 external script is never requested', not [r for r in reqs if r['path'] == '/c3.js'])
        check('restriction', 'f3 never messages the parent', not re.search(r'SBX-P3 ', text))
        # ---- frame 4: allow-scripts, no allow-modals/allow-popups ----
        r4 = line(r'SBX-C4-RESULT [^\r\n]*')
        check('positive', 'f4 script runs', r4.startswith('SBX-C4-RESULT '), r4)
        check('restriction', 'f4 dialog text never reaches the console',
              not any(s in text for s in ('SBX-C4-ALERT-TEXT', 'SBX-C4-CONFIRM-TEXT', 'SBX-C4-PROMPT-TEXT', 'SBX-C4-PRINT-TEXT')))
        check('restriction', 'f4 blocked dialogs are reported to the developer console',
              "Ignored call to 'alert()'" in text and "Ignored call to 'print()'" in text)
        check('restriction', 'f4 alert/print return undefined, confirm false, prompt null, open null',
              r4 == 'SBX-C4-RESULT alert=undefined,confirm=false,prompt=null,print=undefined,open=null', r4)
        check('restriction', 'f4 blocked popup is reported to the developer console', 'Blocked opening a new window' in text)
        check('nondiscriminating', 'f4 no popup is ever requested', not [r for r in reqs if r['path'] == '/popup'])
        # ---- the parent document itself ----
        check('positive', 'parent document is retained and painted', bool(re.search(r'^\[dl\] -?\d+,-?\d+ SBX-PARENT\r?$', text, re.M)))

        result['checks'] = checks
        bad_pos = [c['name'] for c in checks if c['kind'] != 'restriction' and not c['ok']]
        restr = [c for c in checks if c['kind'] == 'restriction']
        if args.expect_flags_ignored:
            still = [c['name'] for c in restr if c['ok']]
            result['passed'] = not bad_pos and not still
            result['restrictions_failed'] = sum(not c['ok'] for c in restr)
            if still:
                result['error'] = 'control did not fail restriction checks: ' + '; '.join(still)
        else:
            bad = bad_pos + [c['name'] for c in restr if not c['ok']]
            result['passed'] = not bad
            if bad:
                result['error'] = 'failed: ' + '; '.join(bad)
        if bad_pos:
            result['error'] = (result.get('error', '') + ' positive failures: ' + '; '.join(bad_pos)).strip()
    except Exception as exc:
        result['error'] = str(exc)
        result['checks'] = checks
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        for server in (parent_server, child_server):
            server.shutdown()
        (out / 'results.json').write_text(json.dumps(result, indent=2))
    for c in checks:
        print('%-4s %-17s %s%s' % ('ok' if c['ok'] else 'FAIL', c['kind'], c['name'],
                                   ('  [' + c['evidence'] + ']') if (c['evidence'] and not c['ok']) else ''))
    print('frame-sandbox-guest%s: %s%s' % (' (control, flags ignored)' if args.expect_flags_ignored else '',
                                          'PASS' if result['passed'] else 'FAIL',
                                          '' if result['passed'] else ' -- ' + result.get('error', '')))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
