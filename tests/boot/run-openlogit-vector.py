#!/usr/bin/env python3
"""Native 2D SDK acceptance through guest compilation, input and screen pixels."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image, ImageChops

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from qmp_ui import Session,configure
from qmp_window import win_by_title,cmd_key
from owned_process import stop_owned

parser=argparse.ArgumentParser()
parser.add_argument('--build',required=True,type=Path)
build=parser.parse_args().build.resolve()
out=build/'vector-guest'
out.mkdir(exist_ok=True)
serial=out/'serial.log'
serial.write_text('')
checks=[]
actions=[]
process=ui=None

def log():
    return serial.read_text(errors='replace')

def record(complete=False):
    (out/'result.json').write_text(json.dumps({'complete':complete,
        'passed':bool(checks) and all(check['passed'] for check in checks),
        'checks':checks,'actions':actions},indent=2)+'\n')

def check(ok,name):
    checks.append({'name':name,'passed':bool(ok)})
    record()
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    if not ok: raise AssertionError(name)

def wait(condition,name,seconds=120):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if process.poll() is not None or 'VECTOR ERROR' in log() or 'error: include file' in log():
            raise RuntimeError('guest failure: '+name)
        value=condition()
        if value: return value
        time.sleep(.05)
    raise TimeoutError(name)

def send(command):
    actions.append({'shell':command})
    process.stdin.write((command+'\n').encode())
    process.stdin.flush()

def tap(key):
    actions.append({'key':key})
    for down in [True,False]:
        response=ui._input([{'type':'key','data':{'down':down,'key':{'type':'qcode','data':key}}}])
        if 'error' in response: raise RuntimeError(response)
        time.sleep(.05)

def window():
    return win_by_title(str(serial),'OpenLogit Vector Studio')

def point(x,y):
    w=window()
    return w['x']+x,w['y']+w['h']-w['ch']+y

def click(x,y):
    actions.append({'click':[x,y]})
    ui.click_at_confirmed(str(out/'pointer.ppm'),*point(x,y))

def shot(name):
    path=out/(name+'.ppm')
    response=ui.cmd({'execute':'screendump','arguments':{'filename':str(path)}})
    if 'error' in response: raise RuntimeError(response)
    image=Image.open(path).convert('RGB')
    x,y=point(0,0)
    image=image.crop((x,y,x+900,y+560))
    image.save(out/(name+'.png'))
    return image

def stage(image):
    return image.crop((24,112,600,456))

def different(a,b):
    return ImageChops.difference(a,b).getbbox() is not None

def frames():
    pattern=r'VECTOR FRAME n=(\d+) page=(\d+) playing=(\d+) x=(\d+) y=(\d+) damage=(\d+),(\d+),(\d+),(\d+) copied=(\d+) ns=(\d+)'
    return [tuple(map(int,match)) for match in re.findall(pattern,log())]

def settle(name):
    last=None
    stable=time.monotonic()
    deadline=stable+30
    while time.monotonic()<deadline:
        current=frames()
        if current!=last:
            last=current
            stable=time.monotonic()
        if current and time.monotonic()-stable>.8: return shot(name)
        if 'VECTOR ERROR' in log(): raise RuntimeError('render failure')
        time.sleep(.1)
    raise TimeoutError('idle: '+name)

configure(1280,800)
record()
try:
    with tempfile.TemporaryDirectory(prefix='ol-vector-') as temporary,serial.open('wb') as stream:
        socket=str(Path(temporary)/'qmp.sock')
        command=['qemu-system-x86_64','-cpu','max','-smp','4','-m','1G','-accel','tcg,thread=multi',
                 '-cdrom',str(build/'logit.iso'),'-drive',f'file={build/"disk.img"},format=raw,if=none,id=d0',
                 '-device','virtio-blk-pci,drive=d0','-snapshot','-boot','d','-vga','none',
                 '-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-nic','none',
                 '-serial','stdio','-qmp',f'unix:{socket},server=on,wait=off','-no-reboot']
        (out/'command.json').write_text(json.dumps(command,indent=2))
        process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=stream,stderr=subprocess.STDOUT)
        wait(lambda:'[wm] launched Finder' in log(),'desktop')
        ui=Session(socket,serial=str(serial))
        send('/bin/mkdir /tmp')
        source=' '.join('/usr/share/openlogit/vector/'+name+'.c' for name in ['main','artwork','ui'])
        send('/bin/tcc '+source+' -I/usr/include/openlogit -lopenlogit -o /tmp/vector-studio')
        send('/tmp/vector-studio')
        wait(lambda:'VECTOR READY' in log() and window(),'guest-compiled Vector Studio')
        initial=settle('initial')
        check('/tmp/vector-studio loading' in log(),'guest TCC compiles and launches the three-file 2D consumer')
        check(len(set(stage(initial).getdata()))>300,'native viewport displays gradients and antialiased vector geometry')
        count=len(frames())
        time.sleep(.8)
        check(len(frames())==count,'idle vector workspace stops frame submissions')
        for key,name in [('c','path-clip'),('d','dashed-stroke')]:
            tap(key)
            changed=settle(name)
            check(different(stage(initial),stage(changed)),name+' changes actual pixels')
            tap(key)
            restored=settle(name+'-restored')
            check(not different(stage(initial),stage(restored)),name+' restores exact original pixels')
        tap('2')
        sprites=settle('sprites')
        check(different(stage(initial),stage(sprites)),'sprite page displays atlas crops and nine-slice panel')
        tap('s')
        check(different(stage(sprites),stage(settle('atlas-next'))),'cycling atlas source rectangles changes displayed sprites')
        tap('r')
        settle('atlas-reset')
        click(840,374)
        wide=settle('nine-slice-wide')
        check(different(stage(sprites),stage(wide)),'nine-slice size control changes panel while keeping cropped sprites')
        tap('r')
        settle('reset-width')
        tap('spc')
        wait(lambda:frames()[-1][2]==1,'sprite animation')
        a=shot('rotate-a')
        wait(lambda:different(stage(a),stage(shot('rotate-b'))),'actual affine rotation',15)
        check(True,'shared timeline rotates atlas sprites on screen')
        tap('spc')
        paused=settle('sprites-paused')
        count=len(frames())
        time.sleep(.8)
        check(len(frames())==count and not different(stage(paused),stage(shot('sprites-stable'))),
              'pause freezes sprite transforms and stops rendering')
        tap('3')
        tap('r')
        layers=settle('layers')
        # Move focus away from the amount slider before using arrow movement.
        click(270,80)
        tap('3')
        settle('layers-focus')
        tap('right')
        moved=settle('layer-moved')
        check(different(stage(layers),stage(moved)),'retained card movement changes screen position')
        tap('left')
        restored=settle('layer-restored')
        check(not different(stage(layers),stage(restored)),'moving back clears old coverage and restores exact stage pixels')
        last=frames()[-1]
        check(last[7]*last[8]<900*560//4 and last[9]<900*560*8//4,
              'retained movement submits and transports a bounded damage rectangle')
        click(680,374)
        check(different(stage(layers),stage(settle('layer-opacity'))),'layer opacity updates compositing against cached background')
        tap('spc')
        wait(lambda:frames()[-1][2]==1,'retained animation')
        cmd_key(ui,'m')
        wait(lambda:window()['min']==1,'minimized')
        time.sleep(.4)
        count=len(frames())
        time.sleep(.8)
        check(len(frames())==count,'minimized retained animation stops rendering')
        for _ in range(4):
            cmd_key(ui,'tab')
            time.sleep(.3)
            if window()['min']==0: break
        wait(lambda:window()['min']==0 and len(frames())>count,'restore')
        check(True,'window restoration repaints the retained scene')
        tap('spc')
        settle('restored-paused')
        ui.launch_app('settings',title='Settings',probe=str(out/'pointer.ppm'))
        settings=wait(lambda:win_by_title(str(serial),'Settings'),'Settings')
        time.sleep(.4)
        ui.click_at_confirmed(str(out/'pointer.ppm'),settings['x']+584,
                              settings['y']+settings['h']-settings['ch']+388)
        wait(lambda:'VECTOR MOTION reduced=1' in log(),'reduced motion')
        w=window()
        ui.click_at_confirmed(str(out/'pointer.ppm'),w['x']+w['w']//2,w['y']+15)
        settle('reduced')
        tap('spc')
        settle('reduced-play')
        count=len(frames())
        time.sleep(.8)
        check(not frames()[-1][2] and len(frames())==count,'system reduced motion leaves no continuous 2D wakeups')
        record(True)
        paths=[build/'logit.iso',build/'disk.img',build/'sdk/libopenlogit.a',build/'sdk/vector-studio.elf']
        (out/'artifacts.json').write_text(json.dumps({str(path):hashlib.sha256(path.read_bytes()).hexdigest()
                                                    for path in paths},indent=2)+'\n')
except Exception as error:
    checks.append({'name':str(error),'passed':False})
    record()
    raise
finally:
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(process)
print(f'Vector guest: {len(checks)} checks passed',flush=True)
