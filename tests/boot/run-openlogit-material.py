#!/usr/bin/env python3
"""Guest-built SDK UI, normal input and real scanout; no FPS claim from logs.

Pixel patches establish material/animation output. Logs synchronize completion
and count idle submissions only. The host prerequisite runs a disabled-VM
negative control against the ordinary numeric pixel oracle.
"""
import argparse, hashlib, json, re, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image, ImageChops
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure
from qmp_window import win_by_title, cmd_key
from owned_process import stop_owned
p = argparse.ArgumentParser(); p.add_argument('--build', required=True, type=Path); args = p.parse_args()
b = args.build.resolve(); out = b / 'material-guest'; out.mkdir(exist_ok=True)
serial = out / 'serial.log'; serial.write_text(''); checks=[]; actions=[]; q=ui=None

def log(): return serial.read_text(errors='replace')
def record(complete=False):
    (out/'result.json').write_text(json.dumps({'passed':bool(checks) and all(c['passed'] for c in checks),
        'complete':complete,'checks':checks,'actions':actions},indent=2)+'\n')
def check(ok, name):
    checks.append({'name':name,'passed':bool(ok)});record();print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    if not ok: raise AssertionError(name)
def wait(fn, name, seconds=120):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if q.poll() is not None: raise RuntimeError('guest exited: '+name)
        if 'error: include file' in log() or 'MATERIAL ERROR' in log():
            raise RuntimeError('guest compiler/material failure: inspect serial.log')
        result=fn()
        if result:return result
        time.sleep(.02)
    raise TimeoutError(name)
def send(line): actions.append({'shell':line});q.stdin.write((line+'\n').encode());q.stdin.flush()
def tap(key):
    actions.append({'key':key})
    for down in (True,False):
        result=ui._input([{'type':'key','data':{'down':down,'key':{'type':'qcode','data':key}}}])
        if 'error' in result:raise RuntimeError(result)
        time.sleep(.04)
def window(): return win_by_title(str(serial),'OpenLogit Material Studio')
def point(x,y):
    w=window();return w['x']+x,w['y']+w['h']-w['ch']+y
def click(x,y):
    actions.append({'click':[x,y]});x,y=point(x,y);ui.click_at_confirmed(str(out/'pointer.ppm'),x,y)
def shot(name):
    path=out/(name+'.ppm');result=ui.cmd({'execute':'screendump','arguments':{'filename':str(path)}})
    if 'error' in result:raise RuntimeError(result)
    im=Image.open(path).convert('RGB');im.save(out/(name+'.png'));x,y=point(0,0)
    client=im.crop((x,y,x+760,y+540));client.save(out/(name+'-client.png'));return client
def frames():
    return [tuple(map(int,m)) for m in re.findall(r'MATERIAL FRAME n=(\d+) p=(\d+) ripple=(\d+) busy=(\d+) tint=(\d+) gain=(\d+) active=(\d+) passes=(\d+)',log())]
def different(a,b):return ImageChops.difference(a,b).getbbox() is not None
def bar(im):return sum(im.getpixel((x,180))==(74,219,194) for x in range(44,324))
def crop(im):return im.crop((408,170,708,306))
def intermediate():
    im=shot('progress-middle');return im if 8<bar(im)<272 else None
record();configure(1280,800)
try:
    with tempfile.TemporaryDirectory(prefix='ol-material-') as tmp,serial.open('wb') as stream:
        socket=str(Path(tmp)/'qmp.sock')
        command=['qemu-system-x86_64','-cpu','max','-smp','4','-m','1G','-accel','tcg,thread=multi',
            '-cdrom',str(b/'logit.iso'),'-drive',f'file={b/"disk.img"},format=raw,if=none,id=d0',
            '-device','virtio-blk-pci,drive=d0','-snapshot','-boot','d','-vga','none',
            '-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-nic','none',
            '-serial','stdio','-qmp',f'unix:{socket},server=on,wait=off','-no-reboot']
        (out/'command.json').write_text(json.dumps(command,indent=2));q=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=stream,stderr=subprocess.STDOUT)
        wait(lambda:'[wm] launched Finder' in log(),'desktop');ui=Session(socket,serial=str(serial))
        send('/bin/mkdir /tmp')
        send('/bin/tcc /usr/share/openlogit/materials.c -I/usr/include/openlogit -lopenlogit -o /tmp/materials')
        send('/tmp/materials')
        # The first idle frame can also be interrupted by the WM's opening
        # animation log. Readiness is window creation; the next assertion checks
        # the actual pixels rather than requiring an intact FRAME trace first.
        wait(lambda:'MATERIAL READY' in log() and window(),'guest-compiled material UI')
        check('/tmp/materials loading' in log(),'installed SDK compiles and launches material consumer inside guest')
        time.sleep(.5);initial=shot('initial')
        check(bar(initial)==0 and len(set(crop(initial).getdata()))>50,'native UI displays empty progress and bilinear programmable texture')
        n=len(frames());time.sleep(.6)
        check(len(frames())==n,'initial paused UI stops render submissions')
        click(100,235)
        check(wait(intermediate,'actual progress intermediate',10) is not None,'mouse activation displays a genuine progress intermediate')
        wait(lambda:frames()[-1][1]==1000 and not frames()[-1][6],'progress endpoint');time.sleep(.3)
        check(bar(shot('progress-end'))==280,'shared timeline finishes at the exact displayed progress endpoint')
        n=len(frames());time.sleep(.6)
        check(len(frames())==n,'finished progress stops frame submissions')
        tap('l');wait(lambda:frames()[-1][3]==1,'loading enabled');a=shot('loading-a');time.sleep(.4);z=shot('loading-b')
        check(different(a.crop((44,374,244,384)),z.crop((44,374,244,384))),'loading timeline changes actual shader pixels')
        tap('l')
        # A kernel reclaim diagnostic interrupted FRAME n=50 inside busy=0.
        # The app had paused, but the strict trace regex retained busy=1 forever.
        # Synchronize on the actual paused caption, then check idle pixels below.
        paused_caption = initial.crop((44,348,330,370))
        wait(lambda:not different(paused_caption,shot('loading-paused').crop((44,348,330,370))),
             'displayed loading paused caption')
        time.sleep(.2)
        a=shot('paused');n=len(frames());time.sleep(.6)
        check(len(frames())==n and not different(a,shot('paused-stable')),'pausing loading freezes the displayed phase and stops rendering')
        before=shot('ripple-before').crop((50,290,318,346));tap('spc')
        def ripple_middle():
            im=shot('ripple-middle');return im if different(before,im.crop((50,290,318,346))) else None
        check(wait(ripple_middle,'actual ripple intermediate',10) is not None,'press feedback displays shader-driven expanding coverage')
        wait(lambda:frames()[-1][2]==1000 and not frames()[-1][6],'ripple cleared');time.sleep(.3)
        check(not different(before,shot('ripple-end').crop((50,290,318,346))),'ripple final frame removes all transient coverage')
        click(685,399);wait(lambda:frames()[-1][5]>=90,'slider uniform update');time.sleep(.25)
        warm=crop(shot('warm'));check(different(crop(initial),warm),'pointer slider changes bound uniforms and texture pixels')
        tap('right');wait(lambda:frames()[-1][5]==97,'keyboard slider increase')
        tap('left');wait(lambda:frames()[-1][5]==92,'keyboard slider decrease');time.sleep(.25)
        check(not different(warm,crop(shot('slider-keyboard'))),'focused slider arrows adjust and exactly restore its uniform')
        tap('c');wait(lambda:frames()[-1][4]==0,'tint disabled');time.sleep(.25);neutral=crop(shot('neutral'))
        check(different(neutral,warm),'tint toggle changes material output without changing source texture')
        tap('c');wait(lambda:frames()[-1][4]==1,'tint enabled');time.sleep(.25)
        check(not different(warm,crop(shot('warm-restored'))),'restoring tint reproduces the exact material result')
        tap('r');wait(lambda:frames()[-1][5]==55 and frames()[-1][1]==0 and not frames()[-1][3],'reset all controls');time.sleep(.25)
        reset=shot('reset')
        check(bar(reset)==0 and not different(crop(initial),crop(reset)),'reset restores progress, loading state and original texture uniforms')
        # Slider holds focus 5. Tab reaches progress 0; Enter follows the same
        # action path as a mouse click, without direct state manipulation.
        tap('tab');tap('ret');check(wait(intermediate,'keyboard progress intermediate',10) is not None,'Tab and Enter activate the focused material control')
        wait(lambda:frames()[-1][1]==1000 and not frames()[-1][6],'keyboard progress endpoint')
        tap('l');wait(lambda:frames()[-1][3]==1,'loading before minimize');cmd_key(ui,'m')
        wait(lambda:window()['min']==1,'minimized UI');time.sleep(.4);n=len(frames());time.sleep(.7)
        check(len(frames())==n,'minimized continuous animation stops render submissions')
        for _ in range(4):
            cmd_key(ui,'tab');time.sleep(.3)
            if window()['min']==0:break
        wait(lambda:window()['min']==0 and len(frames())>n,'restored UI');shot('restored')
        check(True,'normal window switching restores material rendering')
        tap('l');wait(lambda:frames()[-1][3]==0,'pause before settings')
        ui.launch_app('settings',title='Settings',probe=str(out/'pointer.ppm'))
        settings=wait(lambda:win_by_title(str(serial),'Settings'),'Settings');time.sleep(.4)
        ui.click_at_confirmed(str(out/'pointer.ppm'),settings['x']+584,settings['y']+settings['h']-settings['ch']+388)
        wait(lambda:'MATERIAL MOTION reduced=1' in log(),'live reduced motion');w=window()
        ui.click_at_confirmed(str(out/'pointer.ppm'),w['x']+w['w']//2,w['y']+15);time.sleep(.4)
        mark=len(frames());tap('p');tap('spc');tap('l')
        wait(lambda:len(frames())>mark and frames()[-1][3]==1,'reduced controls');time.sleep(.3)
        check(all(f[1]==1000 and f[2]==1000 and f[6]==0 for f in frames()[mark:]) and bar(shot('reduced'))==280,
              'live reduced motion uses endpoints and a static loading material')
        n=len(frames());time.sleep(.7)
        check(len(frames())==n,'reduced motion leaves no continuous material rendering')
        record(True)
        paths=[b/'logit.iso',b/'disk.img',b/'sdk/libopenlogit.a',b/'sdk/materials.elf',
               ROOT/'examples/openlogit/materials.c',ROOT/'examples/openlogit/ui_shaders.h',
               ROOT/'c/lib/gfx3d/material/ol_material.c',ROOT/'c/lib/gfx3d/shader/runtime/ols_vm.c']
        (out/'artifacts.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
except Exception as exc:
    checks.append({'name':str(exc),'passed':False});record();raise
finally:
    if ui:ui.f.close();ui.s.close()
    stop_owned(q)
print(f'OpenLogit material guest: {len(checks)} checks passed',flush=True)
