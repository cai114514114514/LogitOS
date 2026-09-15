#!/usr/bin/env python3
"""Compile and exercise native effects through guest input and real scanout.

Serial frame counters only synchronize the actions; PNG comparisons establish
that intermediate/final images reached the display. No FPS claim is inferred.
"""
import argparse,json,re,subprocess,sys,tempfile,time,hashlib
from pathlib import Path
from PIL import Image,ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from qmp_ui import Session,configure
from qmp_window import win_by_title
from owned_process import stop_owned
p=argparse.ArgumentParser();p.add_argument('--build',required=True,type=Path);a=p.parse_args()
b=a.build.resolve();out=b/'effects-guest';out.mkdir(exist_ok=True)
serial=out/'serial.log';serial.write_text('');checks=[];q=None;ui=None
def text():return serial.read_text(errors='replace')
def record(complete=False):
    (out/'result.json').write_text(json.dumps({'passed':bool(checks) and all(c['passed'] for c in checks),
        'complete':complete,'checks':checks},indent=2)+'\n')
def check(value,label):
    checks.append({'name':label,'passed':bool(value)});record();print(('PASS ' if value else 'FAIL ')+label,flush=True)
    if not value:raise AssertionError(label)
def wait(fn,label,seconds=120):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if q.poll() is not None:raise RuntimeError('guest exited: '+label)
        value=fn()
        if value:return value
        time.sleep(.02)
    raise TimeoutError(label)
def send(line):q.stdin.write((line+'\n').encode());q.stdin.flush()
def tap(key):
    for down in (True,False):
        r=ui.cmd({'execute':'input-send-event','arguments':{'events':[{'type':'key','data':{
            'down':down,'key':{'type':'qcode','data':key}}}]}})
        if 'error' in r:raise RuntimeError(r)
        time.sleep(.06)
def shot(name):
    path=out/(name+'.ppm');r=ui.cmd({'execute':'screendump','arguments':{'filename':str(path)}})
    if 'error' in r:raise RuntimeError(r)
    im=Image.open(path).convert('RGB');im.save(out/(name+'.png'))
    w=win_by_title(str(serial),'OpenLogit Effects');client_y=w['y']+w['h']-w['ch']
    return im.crop((w['x']+10,client_y+80,w['x']+650,client_y+400))
def different(a,b):return ImageChops.difference(a,b).getbbox() is not None
def card_x(im):
    # Inspect the actual colored card, away from the title and its glass. A
    # submission log can precede the compositor's display update by a frame.
    pixels=[x for x in range(im.width) if (lambda c:c[1]>100 and c[1]>c[0]+30 and c[2]>120)(im.getpixel((x,160)))]
    return min(pixels) if pixels else -1
def frames():return text().count('EFFECTS FRAME ')
record();configure(1280,800)
try:
    with tempfile.TemporaryDirectory(prefix='ol-effects-') as tmp,serial.open('wb') as log:
        sock=str(Path(tmp)/'qmp.sock')
        cmd=['qemu-system-x86_64','-cpu','max','-smp','4','-m','1G','-accel','tcg,thread=multi',
             '-cdrom',str(b/'logit.iso'),'-drive',f'file={b/"disk.img"},format=raw,if=none,id=d0',
             '-device','virtio-blk-pci,drive=d0','-snapshot','-boot','d','-vga','none',
             '-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-nic','none',
             '-serial','stdio','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
        (out/'command.json').write_text(json.dumps(cmd,indent=2));q=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT)
        wait(lambda:'[wm] launched Finder' in text(),'desktop')
        send('/bin/mkdir /tmp')
        send('/bin/tcc /usr/share/openlogit/effects.c -I/usr/include/openlogit -lopenlogit -o /tmp/effects')
        send('/tmp/effects');wait(lambda:'EFFECTS READY' in text(),'guest-built example')
        check('/tmp/effects loading' in text(),'guest compiles and executes native effects SDK example')
        ui=Session(sock,serial=str(serial));wait(lambda:'EFFECTS IDLE x=40' in text(),'initial idle');time.sleep(.4)
        initial=shot('initial')
        check(sum(g>100 and bb>100 and r<170 for r,g,bb in initial.getdata())>3000,
              'native translucent card and materials reach real scanout')
        mark=len(text());tap('spc')
        def intermediate():
            im=shot('intermediate');return im if 55<card_x(im)<415 else None
        middle=wait(intermediate,'spring intermediate on scanout',seconds=10)
        wait(lambda:'EFFECTS IDLE x=430' in text()[mark:],'spring completion');time.sleep(.4);final=shot('moved')
        check(different(initial,middle) and different(middle,final) and different(initial,final),
              'normal Space input displays an actual spring intermediate frame and endpoint')
        # This patch changes after departure. The exact whole-scene equality
        # after reverse movement below is the stronger no-trails oracle.
        check(final.getpixel((75,160))!=initial.getpixel((75,160)),
              'old card region is cleared when the layer moves')
        count=frames();time.sleep(.8);idle=shot('idle')
        check(frames()==count and not different(final,idle),'completed animation stops render submissions and leaves scanout stable')
        tap('b');wait(lambda:frames()>count and 'blur=1' in text(),'blur toggle');time.sleep(.4);blur=shot('blur')
        check(different(final,blur),'normal B input changes native layer blur on screen')
        count=frames();tap('b');wait(lambda:frames()>count,'blur disabled');time.sleep(.4);restored=shot('restored')
        check(not different(final,restored),'disabling blur restores the exact unfiltered displayed frame')
        mark=len(text());tap('spc');wait(lambda:'EFFECTS IDLE x=40' in text()[mark:],'reverse motion');time.sleep(.4)
        check(not different(initial,shot('returned')),'reverse movement clears both old and new effect bounds without trails')
        # The serial shell is waiting for this foreground example. Launch the
        # real Settings app through the dock, then use its actual switch.
        ui.launch_app('settings',title='Settings',probe=str(out/'pointer.ppm'))
        settings=wait(lambda:win_by_title(str(serial),'Settings'),'settings window');time.sleep(.4)
        ui.click_at_confirmed(str(out/'pointer.ppm'),settings['x']+584,
            settings['y']+settings['h']-settings['ch']+388)
        wait(lambda:'EFFECTS MOTION reduced=1' in text(),'live reduced-motion preference propagation')
        window=win_by_title(str(serial),'OpenLogit Effects');count=frames()
        ui.click_at_confirmed(str(out/'pointer.ppm'),window['x']+window['w']//2,window['y']+15)
        wait(lambda:frames()>count,'focus restored');time.sleep(.4);mark=len(text());tap('spc')
        wait(lambda:'EFFECTS IDLE x=430' in text()[mark:],'reduced-motion endpoint');time.sleep(.4)
        positions=[int(x) for x in re.findall(r'EFFECTS FRAME x=(\d+)',text()[mark:])]
        check(positions and all(x==430 for x in positions) and card_x(shot('reduced-motion'))==430,
              'live reduced-motion setting reaches existing consumer and displays only the endpoint')
        count=frames();time.sleep(.8)
        check(frames()==count,'reduced-motion interaction leaves no animation wake submissions')
        record(True)
        (out/'artifacts.json').write_text(json.dumps({name:hashlib.sha256((b/name).read_bytes()).hexdigest()
             for name in ('logit.iso','disk.img','sdk/libopenlogit.a')},indent=2))
except Exception as exc:
    checks.append({'name':str(exc),'passed':False});record();raise
finally:
    if ui:ui.f.close();ui.s.close()
    stop_owned(q)
print(f'OpenLogit effects guest: {len(checks)} checks passed',flush=True)
