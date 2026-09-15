#!/usr/bin/env python3
"""Fixed browser workloads; all phase durations come from guest [load-perf].
The HTTP server's deliberate delay is a stimulus, never a host performance
measurement. One VM, sequential pages, no synthetic JS injection into guest.
Outputs retain phase records and serial so a missing completion cannot pass.
"""
import argparse, hashlib, http.server, json, pathlib, re, subprocess, sys, tempfile, threading, time, zlib, struct
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[2]/'tests/qmp'))
from qmp_ui import Session, browser_client_point

def png():
 def c(t,b):return struct.pack('!I',len(b))+t+b+struct.pack('!I',zlib.crc32(t+b))
 return b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('!2I5B',32,32,8,2,0,0,0))+c(b'IDAT',zlib.compress((b'\0'+b'\x20\xb0\x60'*32)*32))+c(b'IEND',b'')
ROOT=pathlib.Path(__file__).resolve().parents[2]

LOAD_PERF_MARKER='[load-perf]'
LOAD_PERF_FIELDS=(
 'total_ms','teardown','document','first_style_layout_paint','styles_fetch',
 'full_style_layout_paint','images','runtime_init','scripts_fetch',
 'scripts_execute','lifecycle_settle',
)
LOAD_PERF_PHASE_FIELDS=LOAD_PERF_FIELDS[1:]
LOAD_PERF_TOKEN=re.compile(r'([a-z][a-z0-9_]*)=([0-9]+)')

class LoadPerfEvidenceError(RuntimeError):pass

def parse_load_perf_record(serial_tail):
 """Accept one complete, uncorrupted production load record.

 A serial producer can interrupt browser printf between bytes. Reassembling
 fields across that interruption would invent a record whose values were never
 emitted atomically, so reject the physical line and retain the serial as the
 evidence. The phase sum is an independent integrity check for digit damage.
 """
 count=serial_tail.count(LOAD_PERF_MARKER)
 if count!=1:
  raise LoadPerfEvidenceError(f'expected exactly one {LOAD_PERF_MARKER} marker, found {count}')
 lines=[line for line in serial_tail.splitlines(keepends=True) if LOAD_PERF_MARKER in line]
 if len(lines)!=1:
  raise LoadPerfEvidenceError(f'{LOAD_PERF_MARKER} marker spans an invalid physical record')
 physical=lines[0]
 if not physical.endswith(('\n','\r')):
  raise LoadPerfEvidenceError(f'incomplete {LOAD_PERF_MARKER} record (no line terminator)')
 line=physical.rstrip('\r\n')
 prefix=LOAD_PERF_MARKER+' '
 if not line.startswith(prefix):
  raise LoadPerfEvidenceError(f'malformed {LOAD_PERF_MARKER} physical line: {line[:240]!r}')
 tokens=line[len(prefix):].split(' ')
 pairs=[]
 for token in tokens:
  match=LOAD_PERF_TOKEN.fullmatch(token)
  if not match:
   raise LoadPerfEvidenceError(f'malformed {LOAD_PERF_MARKER} token {token[:120]!r}; possible serial interleaving')
  pairs.append((match.group(1),int(match.group(2))))
 keys=[key for key,_ in pairs]
 duplicates=sorted({key for key in keys if keys.count(key)>1})
 missing=sorted(set(LOAD_PERF_FIELDS)-set(keys))
 unexpected=sorted(set(keys)-set(LOAD_PERF_FIELDS))
 if duplicates or missing or unexpected:
  raise LoadPerfEvidenceError(
   f'invalid {LOAD_PERF_MARKER} fields: missing={missing} unexpected={unexpected} duplicates={duplicates}')
 if tuple(keys)!=LOAD_PERF_FIELDS:
  raise LoadPerfEvidenceError(f'invalid {LOAD_PERF_MARKER} field order: {keys}')
 values=dict(pairs)
 phase_sum=sum(values[key] for key in LOAD_PERF_PHASE_FIELDS)
 if values['total_ms']!=phase_sum:
  raise LoadPerfEvidenceError(
   f'invalid {LOAD_PERF_MARKER} total: total_ms={values["total_ms"]} phase_sum={phase_sum}')
 return values

def load_perf_parser_self_test():
 valid=('[load-perf] total_ms=760 teardown=120 document=0 '
        'first_style_layout_paint=40 styles_fetch=80 '
        'full_style_layout_paint=50 images=0 runtime_init=430 '
        'scripts_fetch=0 scripts_execute=10 lifecycle_settle=30\r\n')
 expected=dict(zip(LOAD_PERF_FIELDS,(760,120,0,40,80,50,0,430,0,10,30)))
 if parse_load_perf_record(valid)!=expected:
  raise RuntimeError('load-perf self-test failed to parse a valid record')
 noisy='[wm] perf t=1 composites=2\n'+valid+'[wm] locks calls=3\n'
 if parse_load_perf_record(noisy)!=expected:
  raise RuntimeError('load-perf self-test rejected separately framed serial noise')
 rejected={
  'interleaved':('[load-perf] total_ms=[wm] perf t=50816 composites=873\n '
                 'full_style_layout_paint=140 images=0 runtime_init=340 '
                 'scripts_fetch=0 scripts_execute=0 lifecycle_settle=70\n'),
  'missing':valid.replace(' scripts_fetch=0',''),
  'unexpected':valid.replace(' lifecycle_settle=30',' wm=7 lifecycle_settle=30'),
  'duplicate':valid.replace(' document=0',' document=0 document=0'),
  'out-of-order':valid.replace(' teardown=120 document=0',' document=0 teardown=120'),
  'bad-total':valid.replace('total_ms=760','total_ms=761'),
  'unterminated':valid.rstrip('\r\n'),
  'two-records':valid+valid,
 }
 for name,record in rejected.items():
  try:parse_load_perf_record(record)
  except LoadPerfEvidenceError:continue
  raise RuntimeError(f'load-perf self-test accepted invalid {name} record')
 print(f'browser_load load-perf parser self-test: PASS ({len(rejected)+2} checks)')

def media_region_paint_proof(serial):
 # Only a COMPLETE final painted-text block counts. Looking anywhere in the
 # log could let an earlier correct frame hide a later wrong one; looking at
 # CSSOM rects instead tested a different, still-broken transform projection.
 assert serial.rfind('[dl] ---8<--- begin painted text') <= serial.rfind('[dl] ---8<--- end painted text'), 'incomplete final painted-text frame'
 blocks=re.findall(r'\[dl\] ---8<--- begin painted text\r?\n(.*?)\[dl\] ---8<--- end painted text\r?\n',serial,re.S)
 assert blocks,'no complete painted-text frame'
 points={}
 for label in ('MEDIA700','MEDIANESTED'):
  hits=re.findall(r'^\[dl\] (-?\d+),(-?\d+) '+label+r'\r?$',blocks[-1],re.M)
  assert len(hits)==1, 'missing or ambiguous painted label: '+label
  points[label]=list(map(int,hits[0]))
 assert points['MEDIA700'][0]==60 and points['MEDIANESTED'][0]==100, 'wrong media-gated painted coordinates: '+str(points)
 return points
# CSS attribution replays retained DOM/CSS specimens with script and image IO
# removed. This isolates style cost; it is explicitly NOT a live-site score.
def css_specimen(name):
 base=ROOT/'tests/fixtures/cssperf'
 data=(base/(name+'.html')).read_text()
 data=re.sub(r'<script\b[^>]*>.*?</script\s*>','',data,flags=re.I|re.S)
 data=re.sub(r'<link\b[^>]*>','',data,flags=re.I)
 data=re.sub(r'(<img\b[^>]*?)\s(?:src|srcset)=(?:"[^"]*"|\'[^\']*\')',r'\1',data,flags=re.I)
 files=sorted(base.glob('ds-*.css' if name=='deepseek' else 'wp-*.css'))
 links=''.join('<link rel="stylesheet" href="/corpus/'+f.name+'">' for f in files)
 probe="<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"
 return data.replace('</head>',links+'</head>')+probe

def script_specimen():
 # Fixed app-like operations, not a engine language benchmark or site score.
 nodes=''.join('<div class="item">row %d</div>'%i for i in range(600))
 return ('<!doctype html><title>DOM script workload</title><main id="rows">'+nodes+"""</main><script>
 var t=performance.now(),count=0;
 for(var i=0;i<10;i++)count+=document.querySelectorAll('.item').length;
 var q=performance.now();
 for(var i=0;i<60;i++){var f=document.createDocumentFragment(),e=document.createElement('span');e.id='dynamic'+i;e.textContent='new';f.appendChild(e);document.getElementById('rows').appendChild(f);}
 var end=performance.now();
 console.log('SCRIPT-PROBE query_ms='+(q-t)+' fragment_ms='+(end-q)+' total_ms='+(end-t)+' count='+count);
 if(count!==6000||!document.getElementById('dynamic59'))throw Error('script workload incomplete');
 console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});
 </script>""")

class Handler(http.server.BaseHTTPRequestHandler):
 protocol_version='HTTP/1.1'
 def do_GET(self):
  path=self.path.split('?')[0]
  if path=='/inserted-script-held.js':
   # Deliberate server delay is test stimulus. Input latency comes from the
   # guest's performance.now() and real input event, never this host clock.
   time.sleep(8);data=b"window.slowRuns++;console.log('INSERTED-SLOW-SOURCE');";mime='text/javascript'
  elif path=='/inserted-script-chain.js':
   data=b"console.log('INSERTED-SLOW-CHAIN');";mime='text/javascript'
  elif path=='/inserted-script-initial':
   # No timer or native input can rescue a lost final load wake in this case.
   data=b"""<!doctype html><title>Initial dynamic script lifecycle</title><h1 id=result>Waiting for initial script chain</h1><script>window.slowRuns=0;var done=false;console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){var msg='INSERTED-INITIAL '+(done&&slowRuns===1?'PASS':'FAIL');document.getElementById('result').textContent=msg;console.log(msg);console.log('LOAD-PROBE-LOAD')});var s=document.createElement('script');s.src='/inserted-script-held.js';s.onload=function(){var t=document.createElement('script');t.src='/inserted-script-chain.js';t.onload=function(){done=true;console.log('INSERTED-INITIAL-DONE')};document.head.appendChild(t)};document.head.appendChild(s);</script>""";mime='text/html'
  elif path=='/inserted-script-slow':
   data=b"""<!doctype html><title>Input during dynamic script download</title><style>body{margin:30px;font:18px sans-serif}input{display:block;width:300px;height:40px;font:20px sans-serif}</style><h1>Input must work while the script downloads</h1><input id=entry><p id=status>Waiting</p><script>window.slowRuns=0;var q=document.getElementById('entry'),started=0;q.addEventListener('input',function(){var msg='INSERTED-SLOW-INPUT before='+(slowRuns===0)+' value='+q.value+' elapsed_ms='+(performance.now()-started);document.getElementById('status').textContent=msg;console.log(msg)});var r=q.getBoundingClientRect();console.log('INSERTED-SLOW-POINT '+JSON.stringify({x:r.left+r.width/2,y:r.top+r.height/2}));console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD');setTimeout(function(){started=performance.now();var s=document.createElement('script');s.src='/inserted-script-held.js';s.onload=function(){console.log('INSERTED-SLOW-LOAD runs='+slowRuns);var t=document.createElement('script');t.src='/inserted-script-chain.js';t.onload=function(){console.log('INSERTED-SLOW-DONE')};document.head.appendChild(t)};document.head.appendChild(s);console.log('INSERTED-SLOW-QUEUED')},200)});</script>""";mime='text/html'
  elif path=='/navigation-target.html' or (path=='/navigation-self' and '?q=Python' in self.path):
   data=b"<!doctype html><title>Destination</title>NATIVE NAVIGATION ARRIVED<script>console.log('NAVIGATION-BASE DESTINATION');console.log('LOAD-PROBE-SCRIPT');</script>";mime='text/html'
  elif path in ('/form-script-submit','/form-script-requestsubmit'):
   method='requestSubmit' if path.endswith('requestsubmit') else 'submit'
   data=('''<!doctype html><title>Form navigation after callback</title><style>body{margin:30px;font:18px sans-serif}input{width:300px;height:40px}</style><form id=f action="/navigation-target.html"><input id=q name=q value="Python"></form><p>Press Enter. The callback must finish before navigation.</p><script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});var q=document.getElementById('q'),f=document.getElementById('f'),r=q.getBoundingClientRect();console.log('FORM-SCRIPT-POINT '+JSON.stringify({x:r.left+r.width/2,y:r.top+r.height/2}));q.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();console.log('FORM-SCRIPT-BEFORE');f.'''+method+'''();console.log('FORM-SCRIPT-AFTER')}});</script>''').encode();mime='text/html'
  elif path in ('/navigation-link','/navigation-form','/navigation-self','/navigation-tablink'):
   if path in ('/navigation-link','/navigation-tablink'):control='<a id=go href="/navigation-target.html">Follow link</a>'
   else:control='<form'+(' action="/navigation-target.html"' if path=='/navigation-form' else '')+'><input name=q value=Python><button id=go>Submit query</button></form>'
   data=('''<!doctype html><title>Committed document URL</title><style>body{margin:30px}#go{display:block;width:180px;height:40px}</style><h1>Native navigation after editing the address</h1>'''+control+'''<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});var go=document.getElementById('go'),r=go.getBoundingClientRect();go.addEventListener('click',function(){console.log('NAVIGATION-NATIVE-ACTIVATION')});console.log('NAVIGATION-POINT '+JSON.stringify({x:r.left+(go.tagName==='A'?8:r.width/2),y:r.top+(go.tagName==='A'?8:r.height/2)}));</script>''').encode();mime='text/html'
  elif path=='/transparent-box-hit-target.html':
   data=(ROOT/'tests/fixtures/engine-expansion/transparent-box-hit-target.html').read_bytes()+b"<script>console.log('TRANSPARENT-BOX-TARGET')</script>";mime='text/html'
  elif path.startswith('/script-resource-') and path.endswith('.js'):
   f=ROOT/'tests/fixtures/engine-expansion'/path[1:]
   if not f.is_file():self.send_error(404);return
   data=f.read_bytes();mime='text/javascript'
  elif path in ('/dom-matrix','/anchor-components'):
   page=(ROOT/'tests/fixtures/browser'/(path[1:]+'.html')).read_text()
   page+="""<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"""
   if path=='/dom-matrix':page+="""<script>var rr=document.getElementById('resize').getBoundingClientRect();console.log('MATRIX-BUTTON '+JSON.stringify({x:rr.left+rr.width/2,y:rr.top+rr.height/2}));</script>"""
   data=page.encode();mime='text/html'
  elif path=='/script-resource-editing':
   page=(ROOT/'tests/fixtures/engine-expansion/script-resource-events.html').read_text().replace('},20);','},1500);').replace('},1200);','},5000);')
   page+="<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"
   data=page.encode();mime='text/html'
  elif path=='/late-callback-target.html':
   data=b"<!doctype html><title>Destination</title>NAVIGATION-RAN<script>console.log('LATE-DESTINATION')</script>";mime='text/html'
  elif re.fullmatch(r'/late-[012]',path):
   mode=path[-1]
   page=(ROOT/'docs/engine-plan/patches/late-callback.html').read_text()
   page=page.replace('var mode=match?+match[1]:0;', 'var mode='+mode+';')
   # Only observe the existing script. No timer/pump is added: any injected
   # wakeup here could hide the original indefinite-park defect.
   page=page.replace("window.inserted=42;", "console.log('LATE-INSERTED');window.inserted=42;")
   page+="<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"
   data=page.encode();mime='text/html'
  elif path in ('/popover','/text','/element-scroll','/waapi','/max-height','/frame-bootstrap','/message-port','/caret-geometry','/percentage-height','/opacity-group','/display-contents','/grid-font-units','/grid-percentage-item','/absolute-auto-height','/script-resource-events','/positioned-insets','/transparent-box-hit','/pointer-events','/pointer-all-modal','/input-files'):
   page=(ROOT/'tests/fixtures/engine-expansion'/(path[1:]+'.html')).read_text()
   page+="""<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"""
   if path=='/popover':
    page+="""<script>function reportState(){console.log('POPOVER-STATE open='+panel.matches(':popover-open'))}panel.addEventListener('toggle',reportState);[opener,outside].forEach(function(b){var r=b.getBoundingClientRect();console.log('POPOVER-BUTTON '+b.id+' x='+(r.left+r.width/2)+' y='+(r.top+r.height/2))});</script>"""
   if path=='/max-height':
    page+="""<script>var rr=s.getBoundingClientRect();console.log('MAX-HEIGHT-POINT x='+(rr.left+10)+' y='+(rr.top+10));s.addEventListener('scroll',function(){console.log('MAX-HEIGHT-SCROLL top='+s.scrollTop)});</script>"""
   if path=='/text':
    page+="""<script>console.log('TEXT-WIRING '+document.getElementById('status').textContent)</script>"""
   if path=='/opacity-group':
    page+="""<script>['show','hide'].forEach(function(id){var r=document.getElementById(id).getBoundingClientRect();console.log('OPACITY-BUTTON '+id+' '+JSON.stringify({x:r.left+r.width/2,y:r.top+r.height/2}))})</script>"""
   data=page.encode();mime='text/html'
  elif path in ('/range-cover.svg','/intrinsic-delayed.svg'):
   if path=='/intrinsic-delayed.svg':time.sleep(1) # held fixture response, not guest timing evidence
   data=b"<svg xmlns='http://www.w3.org/2000/svg' width='240' height='100'><rect width='240' height='100' fill='#279060'/><circle cx='120' cy='50' r='32' fill='#fff'/></svg>";mime='image/svg+xml'
  elif path in ('/image-intrinsic','/image-intrinsic-passive'):
   page=(ROOT/'tests/fixtures/engine-expansion/image-intrinsic.html').read_text()
   page+="<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"
   data=page.encode();mime='text/html'
  elif path in ('/selector-candidates','/logical-margin','/absolute-auto-width','/stylesheet-interface'):
   data=(ROOT/'tests/fixtures/engine-expansion'/(path[1:]+'.html')).read_bytes();mime='text/html'
  elif path in ('/css-media-regions','/inset-math','/media-range','/absolute-image'):
   page=(ROOT/'tests/fixtures/engine-expansion'/(path[1:]+'.html')).read_text()
   page+="<script>console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>"
   data=page.encode();mime='text/html'
  elif path=='/css-wiring':
   page=(ROOT/'tests/fixtures/engine-expansion/css-wiring.html').read_text()
   page+="""<script>console.log(rows.join(' | '));if(rows.join(' ').indexOf('FAIL:')>=0)throw Error('CSS wiring failed');console.log('LOAD-PROBE-SCRIPT');window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});mq.addEventListener('change',function(){console.log('CSS-WIRING-RESIZE '+mq.matches+' changes='+changes)});</script>"""
   data=page.encode();mime='text/html'
  elif path=='/script':data=script_specimen().encode();mime='text/html'
  elif path in ['/deepseek','/wikipedia']:
   data=css_specimen(path[1:]).encode();mime='text/html'
  elif path.startswith('/corpus/') and re.fullmatch(r'/corpus/(?:ds-[a-f0-9]+|wp-[12])\.css',path):
   data=(ROOT/'tests/fixtures/cssperf'/path.rsplit('/',1)[1]).read_bytes();mime='text/css'
  elif path=='/slow.png':time.sleep(3);data=png();mime='image/png'
  elif path=='/style.css':data=('body{font:18px sans-serif;margin:24px;background:#eef4fa}'+''.join('.c%d{padding:2px;min-width:%dpx}'%(i,i%10) for i in range(300))).encode();mime='text/css'
  else:
   slow=path=='/slow';heavy=path=='/style';data=('''<!doctype html><meta charset=utf-8><title>Load probe</title><link rel=stylesheet href="/style.css"><h1 id=result>WAITING FOR SCRIPT</h1><button id=action onclick="this.textContent='CLICK WORKED';console.log('LOAD-PROBE-CLICK')">Click while image loads</button>'''+('<img width=32 height=32 src="/slow.png">' if slow else '')+(''.join('<div class=c%d>Row %d text</div>'%(i%300,i) for i in range(600)) if heavy else '')+'''<script>document.getElementById('result').textContent='APP READY';console.log('LOAD-PROBE-SCRIPT');setTimeout(function(){console.log('LOAD-PROBE-TIMER')},100);document.addEventListener('keydown',function(){console.log('LOAD-PROBE-INPUT')});window.addEventListener('load',function(){console.log('LOAD-PROBE-LOAD')});</script>''').encode();mime='text/html'
  self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers()
  try:self.wfile.write(data)
  except (BrokenPipeError,ConnectionResetError):pass
 def log_message(self,*a):pass

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--disk',default='build/disk.img');ap.add_argument('--iso',default='build/logit.iso');ap.add_argument('--out');ap.add_argument('--rounds',type=int,default=2);ap.add_argument('--expect-responsive',action='store_true');ap.add_argument('--cases',default='lean,slow,style');ap.add_argument('--self-test-load-perf',action='store_true');args=ap.parse_args()
 if args.self_test_load_perf:load_perf_parser_self_test();return
 if not args.out:ap.error('--out is required unless --self-test-load-perf is used')
 out=pathlib.Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True);serial=out/'serial.txt';serial.write_text('');results_file=out/'results.json';results_file.write_text('[]\n')
 # Keeping QMP under the artifact directory worked for short ad-hoc names,
 # then prevented boot in the wired gate: macOS's sockaddr_un is only 104
 # bytes. Artifact paths must not affect whether the guest can start. A short,
 # private temporary socket directory is owned by this run and removed only
 # after its QEMU exits; screenshots/serial remain in the requested directory.
 qmp_tmp=tempfile.TemporaryDirectory(prefix='bl-qmp-',dir='/tmp');sock=str(pathlib.Path(qmp_tmp.name)/'qmp.sock')
 def artifact_ids():
  result={}
  for name in (args.iso,args.disk):
   p=pathlib.Path(name).resolve();before=p.stat();digest=hashlib.sha256()
   with p.open('rb') as f:
    for block in iter(lambda:f.read(1024*1024),b''):digest.update(block)
   after=p.stat()
   assert (before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns),'artifact changed while hashing: '+str(p)
   result[str(p)]={'sha256':digest.hexdigest(),'size':after.st_size,'mtime_ns':after.st_mtime_ns}
  return result
 # Other agents rebuild this shared disk. A completed marker from a VM whose
 # backing image changed cannot be assigned to the final source revision.
 # Hash inputs before boot and after shutdown, without copying another disk.
 artifacts={'before':artifact_ids()}
 (out/'artifacts.json').write_text(json.dumps(artifacts,indent=2))
 srv=http.server.ThreadingHTTPServer(('0.0.0.0',0),Handler);threading.Thread(target=srv.serve_forever,daemon=True).start()
 cmd=['qemu-system-x86_64','-cpu','max','-cdrom',args.iso,'-drive',f'file={args.disk},format=raw,if=none,id=hd0,file.locking=off','-device','virtio-blk-pci,drive=hd0','-boot','d','-snapshot','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-netdev','user,id=n0','-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+sock+',server,nowait']
 proc=subprocess.Popen(cmd,stdout=open(out/'qemu.log','w'),stderr=subprocess.STDOUT)
 def text():return serial.read_text(errors='replace')
 def wait(s,frm=0,seconds=90):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   tail=text()[frm:]
   # Serial printf arrives in several writes. A marker prefix can precede
   # its JSON/elapsed value; only expose the completed line to assertions.
   if s in tail and '\n' in tail[tail.index(s)+len(s):]:return tail
   if proc.poll() is not None:raise RuntimeError('QEMU exited')
   time.sleep(.15)
  raise RuntimeError('guest marker missing: '+s)
 def page_point(cx,cy):
  return browser_client_point(text(),cx,cy)
 def click_client(cx,cy):u.click_at(*page_point(cx,cy))
 rows=[]
 try:
  wait('desktop live');time.sleep(3);u=Session(sock,serial=str(serial));u.launch_app('browser');time.sleep(3)
  for round in range(args.rounds):
   for case in args.cases.split(','):
    mark=len(text());u.key_mods(('ctrl',),'t',settle=.25);u.typ(f'http://10.0.2.2:{srv.server_port}/{case}?round={round}');u.key('ret');tail=wait('[load-perf]',mark)
    # The marker wait includes its terminating newline. Accept only the exact
    # production schema; a different serial writer interrupting printf makes
    # this case invalid rather than a bag of plausible-looking key/value pairs.
    tail=text()[mark:];load_phases=parse_load_perf_record(tail)
    phases=dict(load_phases);phases.update(case=case,round=round,script='LOAD-PROBE-SCRIPT' in tail)
    if case=='slow' and args.expect_responsive:
     assert 'LOAD-PROBE-LOAD' not in tail, 'load fired before delayed image settled'
     u.key('tab');u.key('spc');wait('LOAD-PROBE-INPUT',mark)
     pending=wait('LOAD-PROBE-TIMER',mark)
     assert 'LOAD-PROBE-LOAD' not in pending, 'input/timer blocked until image completion'
     phases['input_timer_before_load']=True
    wait('LOAD-PROBE-LOAD',mark)
    # Confirm again after the initial load completes, before case-specific
    # actions that may intentionally navigate. A second marker in this window
    # is ambiguous and invalidates the case; later deliberate navigations have
    # their own behavioral assertions and do not alter this load measurement.
    if parse_load_perf_record(text()[mark:])!=load_phases:
     raise LoadPerfEvidenceError(f'{LOAD_PERF_MARKER} record changed while loading case {case!r} round {round}')
    u.screendump(str(out/f'{case}-{round}.ppm'))
    if case=='selector-candidates':
     proof=wait('SELECTOR-CANDIDATES PASS ',mark)
     result=re.search(r'SELECTOR-CANDIDATES PASS query_ms=([0-9.]+) count=(\d+)',proof)
     assert result and int(result[2])==150, 'missing/wrong compound query result'
     phases['candidate_query_ms']=float(result[1]);phases['candidate_count']=int(result[2])
    if case=='logical-margin':
     proof=wait('LOGICAL-MARGIN OPENER ',mark)
     point=json.loads(re.search(r'LOGICAL-MARGIN OPENER (\{[^\r\n]+\})',proof)[1])
     start=len(text());click_client(point['x'],point['y']);proof=wait('LOGICAL-MARGIN OPEN ',start)
     u.screendump(str(out/f'{case}-{round}-open.ppm'))
     record=re.search(r'LOGICAL-MARGIN OPEN (PASS|FAIL) (\{[^\r\n]+\})',proof)
     assert record and record[1]=='PASS', 'logical margin dialog is not centered: '+str(record[0] if record else proof)
     geometry=json.loads(record[2]);phases['native_dialog_geometry']=geometry
     start=len(text());click_client(geometry['close']['x'],geometry['close']['y'])
     wait('LOGICAL-MARGIN CLOSED focus=true',start);phases['native_dialog_close']=True
     u.screendump(str(out/f'{case}-{round}-closed.ppm'))
    if case=='absolute-auto-width':
     proof=wait('AUTO-WIDTH ',mark)
     u.screendump(str(out/f'{case}-{round}-open.ppm'))
     record=re.search(r'AUTO-WIDTH (PASS|FAIL) (\{[^\r\n]+\})',proof)
     assert record and record[1]=='PASS', 'positioned auto width geometry failed: '+str(record[0] if record else proof)
     geometry=json.loads(record[2]);phases['native_auto_width_geometry']=geometry
     start=len(text());p=geometry['underPoint'];click_client(p['x'],p['y'])
     wait('AUTO-WIDTH UNDER 1',start);phases['native_separate_control_click']=True
     start=len(text());p=geometry['closePoint'];click_client(p['x'],p['y'])
     wait('AUTO-WIDTH CLOSED count=1',start);phases['native_auto_width_close']=True
     u.screendump(str(out/f'{case}-{round}-closed.ppm'))
    if case=='stylesheet-interface':
     proof=wait('CSS-SHEET READY ',mark)
     record=re.search(r'CSS-SHEET READY (PASS|FAIL) ([^\r\n]+)',proof)
     assert record and record[1]=='PASS', 'stylesheet interface failed: '+str(record[0] if record else proof)
     geometry=json.loads(record[2]);phases['stylesheet_fallback']=geometry
     u.screendump(str(out/f'{case}-{round}-ready.ppm'))
     start=len(text());p=geometry['change'];click_client(p['x'],p['y'])
     proof=wait('CSS-SHEET CHANGED ',start)
     record=re.search(r'CSS-SHEET CHANGED (PASS|FAIL) (\{[^\r\n]+\})',proof)
     assert record and record[1]=='PASS', 'native stylesheet insertion did not change layout'
     phases['native_stylesheet_insert']=json.loads(record[2])
     u.screendump(str(out/f'{case}-{round}-changed.ppm'))
     start=len(text());p=geometry['restore'];click_client(p['x'],p['y'])
     proof=wait('CSS-SHEET RESTORED ',start)
     record=re.search(r'CSS-SHEET RESTORED (PASS|FAIL) (\{[^\r\n]+\})',proof)
     assert record and record[1]=='PASS', 'native stylesheet deletion did not restore layout'
     phases['native_stylesheet_delete']=json.loads(record[2])
     u.screendump(str(out/f'{case}-{round}-restored.ppm'))
    if case in ('popover','element-scroll','waapi','message-port'):
     marker={'popover':'POPOVER-WIRING','element-scroll':'ELEMENT-SCROLL-WIRING','waapi':'WAAPI-WIRING','message-port':'MESSAGE-PORT-WIRING'}[case]
     proof=wait(marker,mark);time.sleep(.2);proof=text()[mark:]
     record=re.search(re.escape(marker)+r'[^\r\n]*',proof)
     assert record and 'failures=0' in record[0], proof
     phases['wiring_result']=record[0]
     u.screendump(str(out/f'{case}-{round}.ppm'))
    if case=='css-media-regions':
     proof=wait('CSS-MEDIA-REGIONS RECT ',mark)
     wait('[dl] ---8<--- end painted text',mark+proof.index('CSS-MEDIA-REGIONS RECT '))
     phases['media_regions_painted']=media_region_paint_proof(text()[mark:])
     u.screendump(str(out/f'{case}-{round}.ppm'))
    if case=='inset-math':
     proof=wait('INSET-MATH-OPEN-POINT ',mark)
     p=json.loads(re.search(r'INSET-MATH-OPEN-POINT (\{[^\r\n]+\})',proof)[1])
     step=len(text());click_client(p['x'],p['y'])
     proof=wait('INSET-MATH-CLOSE-POINT ',step)
     u.screendump(str(out/f'{case}-{round}-open.ppm'))
     assert 'INSET-MATH-OPEN PASS ' in proof,proof
     p=json.loads(re.search(r'INSET-MATH-CLOSE-POINT (\{[^\r\n]+\})',proof)[1])
     step=len(text());click_client(p['x'],p['y']);wait('INSET-MATH-CLOSED',step)
     phases['native_calculated_dialog_open_close']=True
     u.screendump(str(out/f'{case}-{round}-closed.ppm'))
    if case=='image-intrinsic':
     proof=wait('IMAGE-INTRINSIC ',mark)
     assert 'IMAGE-INTRINSIC PASS ' in proof,proof
     phases['decoded_geometry_in_load_callback']=True
    if case=='image-intrinsic-passive':
     wait('IMAGE-INTRINSIC-PASSIVE LOADED',mark)
     step=len(text());u.key_mods(('ctrl',),'l');u.typ('about:boxes');u.key('ret')
     proof=wait('[dl] ---8<--- end boxes',step)
     # This page makes no CSSOM geometry reads or DOM writes after decoding.
     # about:boxes is a dump only: it cannot perform the missing reflow itself.
     def box(kind,ident):
      matches=re.findall(r'\[dl\] '+kind+r'\s+(-?\d+),\s*(-?\d+)\s+(\d+)x\s*(\d+)\s+<[^>]+> #'+re.escape(ident)+r'(?:\s|\r?$)',proof,re.M)
      assert len(matches)==1,('missing/ambiguous native box',ident,matches)
      return tuple(map(int,matches[0]))
     a=box('img','flow-image');t=box(r'\S+','following')
     assert a[2:]==(240,100) and t[1]==a[1]+100,('late dimensions did not reflow passive page',a,t)
     phases['decoded_geometry_without_dom_or_cssom']=True
     u.screendump(str(out/f'{case}-{round}.ppm'))
    if case=='absolute-image':
     proof=wait('ABSOLUTE-IMAGE ',mark)
     assert 'ABSOLUTE-IMAGE PASS ' in proof,proof
     phases['native_positioned_image_with_whitespace']=True
     u.screendump(str(out/f'{case}-{round}.ppm'))
    if case=='media-range':
     proof=wait('MEDIA-RANGE ',mark)
     assert 'MEDIA-RANGE PASS wide ' in proof,proof
     frames=re.findall(r'\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+)[^\n]*Browser',text())
     assert frames,'missing Browser window frame'
     step=len(text());x,y,w,h=map(int,frames[-1]);u.goto(x+w-2,y+h-2)
     u._input([{'type':'btn','data':{'button':'left','down':True}}]);u.goto(x+700,y+h-30)
     u._input([{'type':'btn','data':{'button':'left','down':False}}])
     proof=wait('MEDIA-RANGE PASS narrow ',step)
     phases['native_range_query_resize']=True
     u.screendump(str(out/f'{case}-{round}-narrow.ppm'))
    if case=='input-files':
     proof=wait('INPUT-FILES PASS checks=23 failures=0',mark)
     r=json.loads(re.search(r'INPUT-FILES-POINT (\{[^\r\n]+\})',proof)[1])
     start=len(text());click_client(r['x'],r['y']);wait('INPUT-FILES-CLICK PASS',start)
     phases['native_binding_click']=True
     u.screendump(str(out/'input-files-click.ppm'))
    if case=='element-scroll':
     match=re.search(r'ELEMENT-SCROLL-TARGET x=([\d.]+) y=([\d.]+)',wait('ELEMENT-SCROLL-TARGET',mark))
     assert match,'missing target geometry'
     cx,cy=map(float,match.groups());click_client(cx,cy);wait('ELEMENT-SCROLL-CLICK',mark)
     wheelmark=len(text());u.goto(*page_point(cx,cy))
     for down in (True,False):u._input([{'type':'btn','data':{'button':'wheel-down','down':down}}])
     wait('ELEMENT-SCROLL-EVENT 220',wheelmark);phases['native_click_and_wheel']=True
     u.screendump(str(out/f'{case}-{round}-wheel.ppm'))
    if case=='popover':
     def popclick(name):
      match=re.search(r'POPOVER-BUTTON '+name+r' x=([\d.]+) y=([\d.]+)',text()[mark:]);assert match
      click_client(*map(float,match.groups()))
     step=len(text());popclick('opener');wait('POPOVER-STATE open=true',step);u.screendump(str(out/f'{case}-{round}-open.ppm'))
     step=len(text());popclick('outside');wait('POPOVER-STATE open=false',step)
     step=len(text());popclick('opener');wait('POPOVER-STATE open=true',step)
     step=len(text());u.key('esc');wait('POPOVER-STATE open=false',step)
     phases['native_invoker_lightdismiss_escape']=True;u.screendump(str(out/f'{case}-{round}-closed.ppm'))
    if case=='waapi':
     match=re.search(r'WAAPI-CANCEL-BUTTON clientX=([\d.]+) clientY=([\d.]+)',text()[mark:]);assert match,'missing cancel geometry'
     step=len(text());click_client(*map(float,match.groups()));wait('WAAPI-CANCEL state=idle',step)
     phases['native_cancel_click']=True;u.screendump(str(out/f'{case}-{round}-cancel.ppm'))
    if case=='caret-geometry':
     wait('CARET-READY specimens=8',mark)
     boxes={r['id']:r for r in (json.loads(x) for x in re.findall(r'CARET-BOX (\{[^\r\n]+\})',text()[mark:]))}
     def state_after(start,id,cause):
      wait('"cause":"'+cause+'"',start)
      rows=[json.loads(x) for x in re.findall(r'CARET-STATE (\{[^\r\n]+\})',text()[start:])]
      return [r for r in rows if r['id']==id and r['cause']==cause][-1]
     edits=[]
     for id,length in [('ascii',1600),('cjk',600),('mixed',800),('password',1400)]:
      r=boxes['focus-'+id];click_client(r['x']+r['w']/2,r['y']+r['h']/2)
      u.key('end');u.key('left');start=len(text());u.key('z')
      value=state_after(start,id,'input');assert value['length']==length+1 and value['start']==length,value
      u.key('end');time.sleep(.2);u.screendump(str(out/f'{case}-{id}-end.ppm'))
      r=boxes[id];start=len(text());click_client(r['x']+r['w']-9,r['y']+r['h']/2)
      value=state_after(start,id,'after-native-click');edits.append(value)
      assert value['start']==length+1,('native end click drift',value)
     phases['native_caret_edits']=edits
    if case=='address-geometry':
     # Native chrome editing only; do not navigate to the long specimen or
     # turn a network failure into a text-geometry result.
     u.key_mods(('ctrl',),'l',settle=.25);u.typ('Wi'*100)
     u.key('end');u.screendump(str(out/'address-end.ppm'))
     u.key('home');u.screendump(str(out/'address-home.ppm'))
     u.key('end');u.key_mods(('shift',),'left');u.screendump(str(out/'address-selection.ppm'))
     phases['native_address_screenshots']=True
    if case=='percentage-height':wait('PERCENTAGE-HEIGHT PASS',mark)
    if case in ('form-script-submit','form-script-requestsubmit'):
     r=json.loads(re.search(r'FORM-SCRIPT-POINT (\{[^\r\n]+\})',text()[mark:])[1])
     start=len(text());click_client(r['x'],r['y']);u.key('ret')
     proof=wait('NAVIGATION-BASE DESTINATION',start,seconds=20)
     assert proof.index('FORM-SCRIPT-AFTER')<proof.index('NAVIGATION-BASE DESTINATION')
     phases['native_script_form_callback_unwinds_before_navigation']=True
     u.screendump(str(out/f'{case}-destination.ppm'))
    if case=='inserted-script-initial':
     proof=wait('INSERTED-INITIAL PASS',mark)
     assert proof.index('INSERTED-INITIAL-DONE')<proof.index('INSERTED-INITIAL PASS')
     # load's console marker precedes settlement and the compositor. The
     # generic first capture can still show Waiting; require the later paint.
     wait('[dl] ---8<--- end painted text',mark+proof.index('INSERTED-INITIAL PASS'))
     u.screendump(str(out/'inserted-script-initial-painted.ppm'),settle=.3)
     phases['initial_load_after_chain_without_native_wake']=True
    if case=='pointer-events':
     proof=wait('POINTER-EVENTS PASS',mark)
     match=re.search(r'POINTER-EVENTS PASS point=(\d+),(\d+) hit=under child=([\d.]+),([\d.]+),([\d.]+),([\d.]+) auto=override',proof);assert match,proof
     x,y,cx,cy,cw,ch=map(float,match.groups())
     start=len(text());click_client(x,y);u.key_mods(('ctrl',),'a');u.typ('Python')
     wait('POINTER-EVENTS INPUT Python',start)
     start=len(text());click_client(cx+cw/2,cy+ch/2)
     wait('POINTER-EVENTS AUTO CLICK',start);wait('POINTER-EVENTS ANCESTOR BUBBLE',start)
     phases['native_pointer_passthrough_override_bubble']=True
     u.screendump(str(out/'pointer-events-native.ppm'))
    if case=='pointer-all-modal':
     proof=wait('MODAL-ALL READY ',mark)
     points=json.loads(re.search(r'MODAL-ALL READY (\{[^\r\n]+\})',proof)[1])
     def modal_click(name,marker):
      start=len(text());p=points[name];click_client(p['x'],p['y'])
      return wait(marker,start,seconds=20)
     # The first click is outside the dialog: the backdrop must intercept it.
     modal_click('under','MODAL-ALL SHADE action=0 close=0 reopen=0 outside=1')
     modal_click('action','MODAL-ALL ACTION action=1 close=0 reopen=0 outside=1')
     wait('MODAL-ALL BUBBLE action=1 close=0 reopen=0 outside=1 bubble=1',mark)
     u.screendump(str(out/'pointer-all-first-click.ppm'))
     modal_click('close','MODAL-ALL CLOSED action=1 close=1 reopen=0 outside=1')
     u.screendump(str(out/'pointer-all-closed.ppm'))
     modal_click('under','MODAL-ALL REOPENED action=1 close=1 reopen=1 outside=1')
     modal_click('action','MODAL-ALL ACTION action=2 close=1 reopen=1 outside=1')
     wait('MODAL-ALL BUBBLE action=2 close=1 reopen=1 outside=1 bubble=3',mark)
     phases['native_all_modal_action_close_reopen']=True
     u.screendump(str(out/'pointer-all-reopened.ppm'))
    if case=='inserted-script-slow':
     wait('INSERTED-SLOW-QUEUED',mark)
     r=json.loads(re.search(r'INSERTED-SLOW-POINT (\{[^\r\n]+\})',text()[mark:])[1])
     start=len(text());click_client(r['x'],r['y']);u.typ('abc')
     proof=wait('INSERTED-SLOW-INPUT before=true value=abc',start,seconds=6)
     match=re.search(r'INSERTED-SLOW-INPUT before=true value=abc elapsed_ms=([0-9.]+)',proof);assert match,proof
     phases['native_input_before_dynamic_script']=True;phases['guest_input_since_script_start_ms']=float(match[1])
     u.screendump(str(out/'inserted-script-input-pending.ppm'))
     wait('INSERTED-SLOW-DONE',mark,seconds=30)
     assert text()[mark:].count('INSERTED-SLOW-LOAD runs=1')==1
    if case=='transparent-box-hit':
     for name in ('cover','cancel','plain'):
      pattern=re.escape(name)+(r' client center [\d.]+,[\d.]+ right blank ([\d.]+),([\d.]+)' if name=='plain' else r' client center ([\d.]+),([\d.]+)')
      match=re.search(pattern,text()[mark:]);assert match,(name,text()[mark:])
      start=len(text());click_client(*map(float,match.groups()))
      wait('TRANSPARENT-BOX-NATIVE-CLICK '+name+' target='+name,start,seconds=10)
      if name!='plain':assert 'TRANSPARENT-BOX-TARGET' not in text()[start:]
     wait('TRANSPARENT-BOX-TARGET',start,seconds=10)
     phases['native_transparent_box_default_action']=True
     u.screendump(str(out/'transparent-box-target.ppm'))
    if case=='positioned-insets':
     proof=wait('POSITIONED-INSETS PASS',mark)
     match=re.search(r'POSITIONED-INSETS PASS[^\r\n]*point=(\d+),(\d+)',proof);assert match,proof
     click_client(*map(int,match.groups()));u.key_mods(('ctrl',),'a');u.typ('Python');start=len(text());u.key('ret')
     wait('POSITIONED-INSETS SUBMIT value=Python',start)
     start=len(text());u.goto(*page_point(30,300))
     for down in (True,False):u._input([{'type':'btn','data':{'button':'wheel-down','down':down}}])
     proof=wait('POSITIONED-INSETS PASS',start)
     match=re.search(r'POSITIONED-INSETS PASS[^\r\n]*scroll=(\d+)',proof);assert match and int(match[1])>0,proof
     phases['native_fixed_input_and_page_scroll']=True
     u.screendump(str(out/'positioned-insets-scrolled.ppm'))
    if case in ('navigation-link','navigation-form','navigation-self','navigation-tablink'):
     r=json.loads(re.search(r'NAVIGATION-POINT (\{[^\r\n]+\})',text()[mark:])[1])
     u.key_mods(('ctrl',),'l',settle=.25);u.typ('unsubmitted-address-draft')
     if case=='navigation-tablink':
      # The suspended tab must retain its committed origin while chrome has
      # an unsubmitted edit. Replayed document bytes alone cannot prove that.
      restore=len(text());u.key_mods(('ctrl',),'t',settle=.25);u.key_mods(('ctrl',),'w',settle=.25)
      wait('LOAD-PROBE-LOAD',restore)
      r=json.loads(re.search(r'NAVIGATION-POINT (\{[^\r\n]+\})',text()[restore:])[1])
     start=len(text());click_client(r['x'],r['y'])
     wait('NAVIGATION-NATIVE-ACTIVATION',start,seconds=5)
     wait('NAVIGATION-BASE DESTINATION',start,seconds=10)
     phases['native_navigation_after_address_edit']=True
     u.screendump(str(out/f'{case}-destination.ppm'))
    if case.startswith('late-'):
     u.goto(*page_point(120,160));start=len(text())
     u._input([{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'shift'}}}])
     for down in (True,False):u._input([{'type':'btn','data':{'button':'wheel-down','down':down}}])
     u._input([{'type':'key','data':{'down':False,'key':{'type':'qcode','data':'shift'}}}])
     wait('LATE-INSERTED' if case=='late-1' else 'LATE-DESTINATION',start)
     phases['late_callback_without_extra_input']=True
     u.screendump(str(out/f'{case}-after.ppm'))
    if case=='opacity-group':
     wait('OPACITY-GROUP opacity=0',mark)
     for id,op in [('show',1),('hide',0)]:
      r=json.loads(re.search('OPACITY-BUTTON '+id+r' (\{[^\r\n]+\})',text()[mark:])[1])
      start=len(text());click_client(r['x'],r['y']);wait('OPACITY-GROUP opacity='+str(op),start)
      u.screendump(str(out/f'{case}-{id}.ppm'))
     phases['native_show_hide']=True
    if case in ('display-contents','grid-font-units','grid-percentage-item','absolute-auto-height'):
     label=case.upper();proof=wait(label+' ',mark)
     assert label+' PASS' in proof, re.findall(label+r'[^\r\n]*',proof)
    if case=='dom-matrix':
     proof=wait('DOM-MATRIX ',mark);assert 'DOM-MATRIX PASS initial' in proof,proof
     r=json.loads(re.search(r'MATRIX-BUTTON (\{[^\r\n]+\})',text()[mark:])[1])
     start=len(text());click_client(r['x'],r['y']);proof=wait('DOM-MATRIX ',start)
     assert 'DOM-MATRIX PASS resize' in proof,proof
     phases['native_matrix_resize']=True;u.screendump(str(out/'dom-matrix-resize.ppm'))
    if case=='anchor-components':
     proof=wait('ANCHOR-COMPONENTS ',mark);assert 'ANCHOR-COMPONENTS PASS' in proof,proof
    if case in ('script-resource-events','script-resource-editing'):
     if case=='script-resource-editing':
      u.key_mods(('ctrl',),'l');u.typ('unsubmitted draft')
     proof=wait('SCRIPT-RESOURCE-READY',mark)
     state=json.loads(re.findall(r'SCRIPT-RESOURCE (\{[^\r\n]+\})',proof)[-1])
     assert sorted(state['events'])==sorted(['first:load','chain:load','empty:load','throws:load','missing:error']),state
     assert sorted(state['executed'])==['chain','first','throws'],state
     phases['script_resource_events']=state;u.screendump(str(out/(case+'-complete.ppm')))
    if case=='text':
     wait('TEXT-WIRING PASS:',mark)
    if case=='max-height':
     wait('MAX-HEIGHT PASS:',mark)
     # Native wheel on the first bounded panel, using actual reported box.
     match=re.search(r'MAX-HEIGHT-POINT x=([\d.]+) y=([\d.]+)',text()[mark:]);assert match
     u.goto(*page_point(*map(float,match.groups())))
     for down in (True,False):u._input([{'type':'btn','data':{'button':'wheel-down','down':down}}])
     wait('MAX-HEIGHT-SCROLL top=40',mark);u.screendump(str(out/f'{case}-{round}-wheel.ppm'))
     phases['native_wheel']=True
    if case=='frame-bootstrap':
     proof=wait('FRAME-WIDGET-SANDBOX refused=4',mark)
     assert 'FRAME-WIDGET-PARENT stable=true' in proof
     assert proof.count('[frame] FRAME-WIDGET-RAN count=1')==2,proof
     assert '[frame] FRAME-SANDBOX-BYPASS-RAN' not in proof
     phases['parser_dynamic_scripts']=2;phases['sandbox_forms_refused']=4
    if case=='css-wiring':
     # Use the actual WM frame from guest output; guessing the titlebar or
     # bottom-right corner would test a drag on the wallpaper instead.
     frames=re.findall(r'\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+)[^\n]*Browser',text())
     assert frames, 'missing Browser window frame'
     x,y,w,h=map(int,frames[-1]);u.goto(x+w-2,y+h-2)
     u._input([{'type':'btn','data':{'button':'left','down':True}}])
     u.goto(x+700,y+h-30)
     u._input([{'type':'btn','data':{'button':'left','down':False}}])
     wait('CSS-WIRING-RESIZE false',mark)
     u.screendump(str(out/f'{case}-{round}-resized.ppm'))
    # Only completed behavior and a validated initial phase record enter JSON.
    phases['case_complete']=True
    rows.append(phases);results_file.write_text(json.dumps(rows,indent=2));print(json.dumps(phases),flush=True)
    # Each specimen gets a fresh tab, but completed tabs must be closed. The
    # original driver kept eleven of them beside the startup tab, hit TAB_MAX
    # (12), then typed the next URL into page focus and timed out looking for
    # a navigation it never requested. Keep the startup tab as the survivor.
    u.key_mods(('ctrl',),'w',settle=.25)
  results_file.write_text(json.dumps(rows,indent=2));assert len(rows)==args.rounds*len(args.cases.split(',')) and all(r['script'] and r['case_complete'] for r in rows)
 finally:
  proc.terminate();proc.wait();srv.shutdown();qmp_tmp.cleanup()
  artifacts['after']=artifact_ids()
  artifacts['unchanged']=artifacts['before']==artifacts['after']
  (out/'artifacts.json').write_text(json.dumps(artifacts,indent=2))
  if not artifacts['unchanged'] and sys.exc_info()[0] is None:
   raise RuntimeError('guest input artifacts changed during run; see artifacts.json')
if __name__=='__main__':main()
