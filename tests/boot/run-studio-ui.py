#!/usr/bin/env python3
"""Real Studio picker/create/collapse/edit workflow on a private LogitOS disk.

Uses QMP mouse and keyboard only for app operations. Guest frame durations come
from the actual renderer's opt-in clock trace; host delays only pace input.
The disk, screenshots, artifact hashes and serial trace are retained per run.
"""
import argparse,hashlib,importlib.util,json,re,shlex,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
runtime=module('studio_runtime',ROOT/'tests/boot/run-agent.py')
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--base',type=Path,default=Path('build'));p.add_argument('--out',type=Path,required=True);p.add_argument('--before',type=Path);p.add_argument('--rebuild',action='store_true');a=p.parse_args()
b=a.build.resolve();base=a.base.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
fixture='# 中文高亮\ndef square(x):\n    return x * x\n\nmessage = "你好，Code Studio"\nvalues = [1, 2, 3, 4]\nprint(message)\nprint(square(12))\n'
(out/'demo.as').write_text(fixture);(out/'nested.as').write_text('print(7)\n')
(out/'settings.conf').write_text('ui.dark = 0\napp.studio.trace_paint = 1\napp.studio.project = /docs\napp.studio.tab.0 = /docs/project/demo.as\napp.studio.tab.1 = /docs/demo.as\napp.studio.active = 1\n' if not a.before else 'ui.dark = 0\napp.studio.trace_paint = 1\napp.studio.project = /docs\napp.studio.tab.0 = /docs/demo.as\napp.studio.active = 0\n')
app=a.before.resolve() if a.before else b/'studio.aex'
files=[f'{base}/{n}.aex:/bin/{n}' for n in ('login','sh','cat','echo','pref')]
files += [f'{ROOT}/fsroot/fonts/{n}.ttf:/fonts/{n}.ttf' for n in ('ui','mono','ui-bold','mono-bold')]
files += [f'{ROOT}/third_party/fonts/DejaVuSans.ttf:/fonts/text.ttf',f'{app}:/studio.aex',f'{out}/demo.as:/docs/demo.as',f'{out}/nested.as:/docs/src/nested.as',f'{out}/settings.conf:/etc/settings.conf',f'{out}/nested.as:/state/fixture',f'{b}/as.aex:/bin/as',f'{b}/asc.la:/usr/as/lib/asc.la',f'{b}/aslex.la:/usr/as/lib/aslex.la']
subprocess.run(['python3','tools/mkfs.py',str(out/'disk.img'),*files],cwd=ROOT,check=True,stdout=(out/'mkfs.log').open('w'))
checks=[];g=None
report={'studio_sha256':hashlib.sha256(app.read_bytes()).hexdigest(),'kernel_sha256':hashlib.sha256((base/'logit.iso').read_bytes()).hexdigest(),'checks':checks}
def check(name,value):
 if not value:raise AssertionError(name)
 checks.append(name);print('PASS',name,flush=True)
def shot(name):
 g.screenshot();subprocess.run(['sips','-s','format','png',str(out/'desktop.ppm'),'--out',str(out/(name+'.png'))],check=True,capture_output=True)
def move(x,y):
 while max(abs(x-g.pointer[0]),abs(y-g.pointer[1]))>0:
  dx=max(-100,min(100,x-g.pointer[0]));dy=max(-100,min(100,y-g.pointer[1]))
  g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
  g.pointer[0]+=dx;g.pointer[1]+=dy;time.sleep(.05)
def click(x,y,right=False):
 move(X+x,Y+y)
 for down in (True,False):
  g.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'right' if right else 'left','down':down}}]});time.sleep(.15)
 time.sleep(.3)
def saved(path,expected):
 # Observe completion rather than treating a fixed host sleep as guest I/O.
 # fsync may still be running when the last key's QMP acknowledgement arrives.
 until=time.monotonic()+5
 while time.monotonic()<until:
  if read(path)==expected:return True
  time.sleep(.1)
 return False
def compare_repaint(name):
 from PIL import Image,ImageChops
 time.sleep(.2);shot(name+'-partial')
 # A theme notification forces a full Studio frame without moving the caret.
 g.command('/bin/pref write ui.dark 0');time.sleep(.2);shot(name+'-full')
 crop=(X+250,Y+76,X+900,Y+500)
 check(name,ImageChops.difference(Image.open(out/(name+'-partial.png')).crop(crop),Image.open(out/(name+'-full.png')).crop(crop)).getbbox() is None)
def read(path):
 fs=module('studio_disk',ROOT/'tools/license_audit.py')._LogitFS(out/'disk.img')
 try:
  ino=fs.lookup(path)
  return fs.read_file(ino).decode() if ino else None
 except (KeyError,TypeError,UnicodeDecodeError,ValueError):return '' if fs.lookup(path) else None
 finally:fs.f.close()
try:
 g=runtime.Guest(base,out/'disk.img',out,'bios','512M');g.wait(b'LogitOS shell',180)
 g.command('/studio.aex &');g.wait(b'STUDIO_PAINT',60);time.sleep(2)
 # Window is the only GUI app, centered by the WM at 110,66, titlebar 30.
 # Read the settled actual frame if available rather than guess from a shot.
 pattern=rb'\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) pt zoom 0 min 0 Code Studio'
 matches=re.findall(pattern,bytes(g.log))
 if matches:
  X,fy,W,fh,cw,ch=map(int,matches[-1]);Y=fy+fh-ch;check('unscaled guest geometry',W==cw)
 else:
  matches=re.findall(rb'home (\d+) (\d+) (\d+) (\d+)',bytes(g.log));X,fy,W,fh=map(int,matches[-1]);Y=fy+30
 shot('opened')
 if not a.before:
  from PIL import Image
  im=Image.open(out/'opened.png').convert('RGB')
  pixels=list(im.crop((X+250,Y+76,X+900,Y+350)).getdata())
  for label,colour in [('purple keywords',(198,120,221)),('green strings',(152,195,121)),('amber numbers',(229,192,123)),('black editor',(0,0,0))]:
   check(label,pixels.count(colour)>5)
  check('missing demo removed from session',g.capture('/bin/pref read app.studio.tab.0').strip()=='/docs/demo.as' and g.capture('/bin/pref read app.studio.tab.1').strip()=='unset' and g.last_capture_exit==1)
 # Identical edit workload for the recorded baseline and new implementation.
 bench_start=len(g.log)
 click(390,100);g.key('ctrl','end');g.key('ret');g.type('hello');g.key('spc');g.type('world');g.key('spc');time.sleep(.4)
 shot('edited')
 if not a.before:
  # Repaint by hovering the toolbar without editing or moving the caret.
  # Compare real scanout pixels, not the renderer's own cache/signatures.
  move(X+90,Y+20);time.sleep(.25);shot('edited-full')
  from PIL import ImageChops
  crop=(X+250,Y+76,X+900,Y+500)
  check('partial matches full repaint',ImageChops.difference(Image.open(out/'edited.png').crop(crop),Image.open(out/'edited-full.png').crop(crop)).getbbox() is None)
 bench_log=bytes(g.log[bench_start:]);g.key('ctrl','z');g.key('ctrl','s');time.sleep(.8)
 if not a.before:
  check('edited actual source','hello world' in (read('/docs/demo.as') or ''))
  g.key('ctrl','home');g.key('shift','down');g.key('shift','down');g.key('shift','down');compare_repaint('multiline-selection-repaint')
  g.key('backspace');compare_repaint('deleted-lines-repaint')
  g.key('ctrl','z');compare_repaint('undo-lines-repaint')
  g.key('right')
  # Open native chooser, browse Root -> docs, then select current folder.
  click(36,20);shot('picker')
  # Dialog at x=290,y=70,title=40 for a 1120x700 window.
  click(416,136);click(380,110+80+28+13);shot('picker-root')
  g.key('ret');time.sleep(.4)
  check('folder selected by picker', '/docs' in g.capture('/bin/pref read app.studio.project'))
  # Explicit toolbar creation is relative to project root with no selection.
  click(140,89);g.type('assets');g.key('ret');time.sleep(.6);shot('new-folder')
  check('folder created by UI',read('/docs/assets') is not None)
  # Sorted folders: assets, src. Expand src then collapse without switching root.
  click(45,139);shot('expanded');click(45,139);shot('collapsed')
  from PIL import ImageChops
  crop=(X+4,Y+150,X+192,Y+195)
  check('collapse removes child rows',ImageChops.difference(Image.open(out/'expanded.png').crop(crop),Image.open(out/'collapsed.png').crop(crop)).getbbox() is not None)
  # Select assets; right-click exposes creation menu. Its first item creates file.
  click(45,118);click(45,118,True);shot('context-menu');click(50,122)
  g.type('main.as');g.key('ret');time.sleep(.6);shot('new-file')
  check('file created in selected folder',read('/docs/assets/main.as')=='')
  g.type('print');g.key('esc');g.key('shift','9');g.type('42');g.key('shift','0');g.key('ret');g.key('ctrl','s');time.sleep(.5)
  check('new file saved',saved('/docs/assets/main.as','print(42)\n'))
  g.key('shift','9');g.key('ctrl','s');time.sleep(.2)
  check('typed opener inserts pair',saved('/docs/assets/main.as','print(42)\n()'))
  g.key('backspace');g.key('ctrl','s');time.sleep(.2)
  check('backspace deletes pair',saved('/docs/assets/main.as','print(42)\n'))
  g.key('shift','apostrophe');g.type('hello');g.key('shift','apostrophe');g.key('ctrl','s');time.sleep(.2)
  check('quotes pair and overtype',saved('/docs/assets/main.as','print(42)\n"hello"'))
  g.key('ctrl','z');g.key('ctrl','z');g.key('ctrl','z');g.key('ctrl','z');g.key('ctrl','z');g.key('ctrl','z');g.key('ctrl','s');time.sleep(.2)
  check('paired typing undo',saved('/docs/assets/main.as','print(42)\n'))
  # A duplicate name is rejected without changing the existing file.
  click(50,89);g.type('main.as');g.key('ret');time.sleep(.3);shot('duplicate-refused');g.key('esc')
  check('duplicate preserved bytes',saved('/docs/assets/main.as','print(42)\n') and read('/docs/main.as') is None)
  g.command('/bin/pref write ui.dark 1');g.command('/bin/pref write ui.dark 0');time.sleep(.3);shot('black-theme')
  check('black after system theme switch',Image.open(out/'black-theme.png').convert('RGB').getpixel((X+800,Y+350))==(0,0,0))
  g.key('f5');time.sleep(2);shot('run-output')
  # Retain command output alongside the GUI screenshot to distinguish a VM
  # failure from a render failure when the output panel is examined.
  report['native_output']=g.capture('/bin/as /docs/assets/main.as')
  check('native program outputs 42',report['native_output'].strip()=='42' and g.last_capture_exit==0)
  # Create a disposable file through the GUI, checkpoint dirty text, cancel
  # deletion once, then confirm. Recreating its name must not revive the draft.
  click(50,89);g.type('delete-me.as');g.key('ret');time.sleep(.4)
  g.type('unsaved');time.sleep(1.2)
  click(45,142,True);click(50,182);shot('delete-dialog');g.key('esc')
  check('cancel deletion retains file',read('/docs/assets/delete-me.as')=='')
  click(45,142,True);click(50,182);click(742,366);time.sleep(.5)
  check('delete removes file',read('/docs/assets/delete-me.as') is None)
  check('delete removes saved tab',g.capture('/bin/pref read app.studio.tab.2').strip()=='unset' and g.last_capture_exit==1)
  click(45,142);click(50,89);g.type('delete-me.as');g.key('ret');time.sleep(.4);g.key('ctrl','s')
  check('deleted draft does not revive',saved('/docs/assets/delete-me.as',''))
  click(45,142,True);click(50,182);click(742,366);time.sleep(.5)
  check('recreated file deletes again',read('/docs/assets/delete-me.as') is None)
 if a.rebuild:
  # Create an actual unsaved draft through the GUI, then rebuild the same
  # private image with the production Makefile's preservation policy.
  g.key('ctrl','end');g.key('shift','3');g.type(' retained draft');time.sleep(2)
  check('source remains explicitly unsaved',read('/docs/assets/main.as')=='print(42)\n')
  prior_log=bytes(g.log);g.close();g=None
  recipe=re.sub(r"\\\r?\n[ \t]*"," ",(ROOT/'Makefile').read_text())
  lines=[line for line in recipe.splitlines() if line.lstrip().startswith('python3 tools/mkfs.py ') and '$(DISK)' in line]
  check('one production disk recipe',len(lines)==1);words=shlex.split(lines[0]);flags=[]
  for i,word in enumerate(words):
   if word in ('--preserve','--preserve-merge'):flags.extend([word,words[i+1]])
  # Seed /state only on the first boot. On rebuild it belongs to preserved
  # user state; packaging the seed again rightly trips the strict root guard.
  rebuild_files=[spec for spec in files if not spec.endswith(':/state/fixture')]
  subprocess.run(['python3','tools/mkfs.py',*flags,'--snapshot-helper',str(b/'lfs_snapshot'),str(out/'disk.img'),*rebuild_files],cwd=ROOT,check=True,stdout=(out/'rebuild.log').open('w'))
  check('saved file survives actual rebuild',read('/docs/assets/main.as')=='print(42)\n')
  check('user edit beats packaged demo', 'hello world' in read('/docs/demo.as'))
  reboot=out/'reboot';reboot.mkdir();g=runtime.Guest(base,out/'disk.img',reboot,'bios','512M');g.wait(b'LogitOS shell',180)
  g.command('/studio.aex &');g.wait(b'STUDIO_PAINT',60);time.sleep(2)
  g.key('ctrl','s');check('unsaved draft restored after rebuild and reboot',saved('/docs/assets/main.as','print(42)\n# retained draft'))
  check('deleted file stays absent after rebuild',read('/docs/assets/delete-me.as') is None)
  report['rebuild_verified']=True
 values=[int(n) for n in re.findall(rb'STUDIO_EDIT us=(\d+)',bytes(g.log))]
 full=[int(n) for n in re.findall(rb'STUDIO_PAINT us=(\d+)',bytes(g.log))]
 bench_edit=[int(n) for n in re.findall(rb'STUDIO_EDIT us=(\d+)',bench_log)]
 bench_full=[int(n) for n in re.findall(rb'STUDIO_PAINT us=(\d+)',bench_log)]
 report.update(edit_us=values,full_us=full,workload_edit_us=bench_edit,workload_full_us=bench_full,passed=True)
except BaseException as exc:
 report.update(passed=False,error=str(exc))
 if g:
  try:shot('failure')
  except Exception:pass
 raise
finally:
 (out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
 if g:g.close()
