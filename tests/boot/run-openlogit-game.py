#!/usr/bin/env python3
"""Guest SDK compile and normal-input gameplay, with real QEMU scanout checks.

Inputs are chosen from the game's ordinary diagnostic log. We never set game
state, invoke physics from the host, or count submitted frames as display FPS.
Every QEMU/socket belongs to this run; cleanup cannot stop another task's guest.
"""
import argparse, hashlib, json, re, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from qmp_ui import Session,configure
from qmp_window import win_by_title,cmd_key
from owned_process import stop_owned
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--usb-keyboard',action='store_true')
a=p.parse_args();b=a.build.resolve();out=b/('island-guest-usb' if a.usb_keyboard else 'island-guest');out.mkdir(exist_ok=True)
checks=[];inputs=[];q=None;ui=None

def record(complete=False):
    (out/'result.json').write_text(json.dumps({'passed':bool(checks) and all(c['passed'] for c in checks),
        'complete':complete,'checks':checks,'inputs':inputs},indent=2)+'\n')
def check(ok,name):
    checks.append({'name':name,'passed':bool(ok)});record();print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    if not ok:raise AssertionError(name)
def wait(fn,name,seconds=180):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if q.poll() is not None:raise RuntimeError('guest exited: '+name)
        result=fn()
        if result:return result
        time.sleep(.05)
    raise TimeoutError(name)
serial=out/'serial.log';serial.write_text('')
def text():return serial.read_text(errors='replace')
def send(s):q.stdin.write((s+'\n').encode());q.stdin.flush()
def key(name,down):
    r=ui.cmd({'execute':'input-send-event','arguments':{'events':[{'type':'key','data':{
        'down':down,'key':{'type':'qcode','data':name}}}]}})
    if 'error' in r:raise RuntimeError(r)
    inputs.append({'key':name,'down':down,'last_state':state()});record()
def tap(name):
    key(name,True);time.sleep(.08);key(name,False)
def state():
    rows=re.findall(r'ISLAND state x=([-\d.]+) y=([-\d.]+) z=([-\d.]+) grounded=(\d) score=(\d) deaths=(\d+)',text())
    if not rows:return None
    x,y,z,g,score,deaths=rows[-1]
    return dict(x=float(x),y=float(y),z=float(z),grounded=int(g),score=int(score),deaths=int(deaths))
def frame():
    rows=re.findall(r'ISLAND frame=(\d+)',text());return int(rows[-1]) if rows else 0
def shot(name):
    path=out/(name+'.ppm');r=ui.cmd({'execute':'screendump','arguments':{'filename':str(path)}})
    if 'error' in r:raise RuntimeError(r)
    image=Image.open(path).convert('RGB');image.save(out/(name+'.png'));return image

def scene(image):
    w=win_by_title(str(serial),'Sky Islands')
    if not w:return None
    # Scene excludes window chrome/menu clock/HUD, and the cursor is a plane.
    return image.crop((w['x']+10,w['y']+110,w['x']+w['w']-10,w['y']+w['h']-40))
def visible_scene():
    im=scene(shot('initial'))
    return im and sum(g>r+15 and g>bb+10 for r,g,bb in im.getdata())>1500
record();configure(1280,800)
try:
    with tempfile.TemporaryDirectory(prefix='ol-island-') as tmp:
        sock=str(Path(tmp)/'qmp.sock')
        cmd=['qemu-system-x86_64','-cpu','max','-smp','4','-m','1G','-accel','tcg,thread=multi',
             '-cdrom',str(b/'logit.iso'),'-drive',f'file={b/"disk.img"},format=raw,if=none,id=d0',
             '-device','virtio-blk-pci,drive=d0','-snapshot','-boot','d','-vga','none',
             '-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-nic','none',
             '-serial','stdio','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
        if a.usb_keyboard:cmd+=['-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0']
        (out/'command.json').write_text(json.dumps(cmd,indent=2))
        with serial.open('wb') as log:
            q=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT)
            wait(lambda:'[wm] launched Finder' in text(),'desktop startup')
            if a.usb_keyboard:
                check('USB_HID_BIND' in text() and 'role=keyboard' in text(),
                      'USB controller enumerates and binds the keyboard before gameplay')
            send('/bin/mkdir /tmp')
            send('/bin/olscc /usr/share/openlogit/passthrough.ols /tmp/pixel.olsb')
            wait(lambda:'OLS-IR v1: 2 instructions, pixel stage' in text(),'guest shader compiler')
            check(True,'installed OLS compiler compiles a real text shader inside guest')
            send('/bin/lslcc /usr/share/openlogit/passthrough.lsl /tmp/fragment.olsb')
            wait(lambda:'LSL v1: 2 instructions, fragment stage' in text(),'guest LSL compiler')
            check(True,'installed LSL compiler compiles typed fragment source inside guest')
            send('/bin/tcc /usr/share/openlogit/island.c -I/usr/include/openlogit -lopenlogit -o /tmp/island')
            send('/bin/echo OPENLOGIT_COMPILE_FINISHED')
            wait(lambda:re.search(r'\nOPENLOGIT_COMPILE_FINISHED\r?\n',text()),'guest source compilation')
            send('/tmp/island --low --trace')
            wait(lambda:'ISLAND READY' in text() and frame()>0,'guest-built game')
            check('shaders=LSL' in text(),'game compiles and links its real LSL vertex and fragment shaders')
            check('[execve]' in text() and '/tmp/island loading' in text(),'guest compiles SDK example and executes resulting binary')
            ui=Session(sock,serial=str(serial))
            wait(visible_scene,'3D scene on actual scanout')
            check(True,'rendered platforms and character reach actual display')
            before=frame();tap('esc');wait(lambda:'ISLAND pause=1' in text() and frame()>before,'pause frame')
            time.sleep(1);a0=scene(shot('paused'));n=frame();time.sleep(1);a1=scene(shot('paused-idle'))
            check(frame()==n and a0.tobytes()==a1.tobytes(),'pause preserves displayed scene and stops rendering')
            tap('esc');wait(lambda:'ISLAND pause=0' in text() and frame()>n,'resume')
            check(True,'normal Escape input resumes rendering')
            # Continuous W uses held-key state, Space is a short press at each
            # ledge. This route also crosses the moving bridge naturally.
            key('w',True);jumped=[False,False];end=time.monotonic()+120
            while time.monotonic()<end and 'ISLAND VICTORY' not in text():
                s=state()
                if s:
                    for i,ledge in enumerate((-2.9,-10.9)):
                        if not jumped[i] and s['z']<=ledge and s['grounded']:
                            tap('spc');jumped[i]=True
                            shot('jump-'+str(i))
                time.sleep(.035)
            key('w',False)
            check('ISLAND VICTORY' in text() and state()['deaths']==0,'normal movement and two jumps complete one full game without reset')
            n=frame();time.sleep(1);shot('victory')
            check(all(jumped) and 'ISLAND collected=3' in text(),'all three collectibles reached through normal gameplay')
            tap('r');wait(lambda:'ISLAND restart' in text() and state()['score']==0,'restart');wait(lambda:frame()>n,'restarted frame')
            shot('restarted');check(state()['z']==1 and state()['deaths']==0,'R restarts the completed game')
            tap('p');wait(lambda:'ISLAND quality=640x360' in text(),'default quality mode');n=frame();wait(lambda:frame()>n,'640x360 frame');shot('quality-640')
            check(True,'640x360 mode renders after changing target resolution')
            cmd_key(ui,'m');wait(lambda:win_by_title(str(serial),'Sky Islands')['min']==1,'minimized game')
            time.sleep(.5);hidden_frames=frame();time.sleep(1)
            check(frame()==hidden_frames,'minimized continuously running game stops render submissions')
            for _ in range(4):
                cmd_key(ui,'tab');time.sleep(.3)
                if win_by_title(str(serial),'Sky Islands')['min']==0:break
            wait(lambda:win_by_title(str(serial),'Sky Islands')['min']==0 and frame()>hidden_frames,'restored game')
            shot('restored-window');check(True,'normal window switching restores the game and rendering resumes')
            matches=re.findall(r'sdk_draws=(\d+)',text())
            check(matches and max(map(int,matches))>0,'compositor reports runtime OpenLogit drawing calls')
            record(True)
            (out/'artifacts.json').write_text(json.dumps({name:hashlib.sha256((b/name).read_bytes()).hexdigest()
                for name in ('logit.iso','disk.img','sdk/libopenlogit.a')},indent=2))
except Exception as exc:
    checks.append({'name':str(exc),'passed':False});record()
    raise
finally:
    if ui:
        try:ui.f.close();ui.s.close()
        except OSError:pass
    stop_owned(q)
print(f'Island guest: {len(checks)} checks passed',flush=True)
