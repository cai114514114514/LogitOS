#!/usr/bin/env python3
"""The shipped Clock draws through OpenLogit and reaches the actual scanout.

The window bounds come from the guest. Detect the moving blue second hand
inside its face, excluding the menu clock and the textual time readout. This
is display evidence, not a frame-counter or host-buffer substitute.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile,time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from qmp_ui import Session,configure,pt
from qmp_window import win_by_title,drag
from owned_process import stop_owned
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);a=p.parse_args();b=a.build.resolve()
out=b/'openlogit-guest';out.mkdir(exist_ok=True);checks=[]
def result(complete=False):
    (out/'result.json').write_text(json.dumps({'passed':bool(checks) and all(c['passed'] for c in checks),'complete':complete,'checks':checks},indent=2)+'\n')
def check(ok,label):
    checks.append({'name':label,'passed':bool(ok)});result();print(('PASS ' if ok else 'FAIL ')+label,flush=True)
    if not ok:raise AssertionError(label)
def wait(fn,label,timeout=120):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        v=fn()
        if v:return v
        time.sleep(.1)
    raise RuntimeError(label)
def blue_face(image,window):
    x,y,w,h=window['x'],window['y'],window['w'],window['h']
    top=y+h-pt(window['ch'])
    # Restrict to the central face and exclude the lower digital readout.
    box=(x+pt(24),top+pt(24),x+w-pt(24),y+h-pt(78))
    pixels=image.load();points=set()
    for yy in range(max(0,box[1]),min(image.height,box[3])):
        for xx in range(max(0,box[0]),min(image.width,box[2])):
            r,g,bb=pixels[xx,yy][:3]
            if bb>140 and bb>r+65 and bb>g+25:points.add((xx,yy))
    # A blue hub or an error label is not a clock hand. Require one connected
    # component with real needle length; character glyphs and the hub are too
    # small individually, even if their combined blue pixel count is large.
    remaining=set(points);best=set()
    while remaining:
        seed=remaining.pop();component={seed};pending=[seed]
        while pending:
            xx,yy=pending.pop()
            for dx,dy in [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]:
                neighbor=(xx+dx,yy+dy)
                if neighbor in remaining:remaining.remove(neighbor);component.add(neighbor);pending.append(neighbor)
        xs=[p[0] for p in component];ys=[p[1] for p in component]
        if max(max(xs)-min(xs),max(ys)-min(ys))>=pt(35) and len(component)>len(best):best=component
    return best
result()
artifacts={name:hashlib.sha256((b/name).read_bytes()).hexdigest() for name in ['logit.iso','clock.aex']}
for width,height in [(1280,800),(1920,1200)]:
    scale=configure(width,height);folder=out/str(width);folder.mkdir(exist_ok=True);serial=folder/'serial.log'
    # QEMU truncates this file only once its child starts. Reading an earlier
    # run in that interval would satisfy BOTH startup waits before this boot.
    serial.write_text('')
    with tempfile.TemporaryDirectory(prefix='ol-gfx-') as tmp:
        sock=str(Path(tmp)/'qmp.sock')
        cmd=['qemu-system-x86_64','-cpu','max','-smp','4','-m','512M','-accel','tcg,thread=multi',
            '-cdrom',str(b/'logit.iso'),'-boot','d','-drive',f'file={b/"disk.img"},format=raw,if=none,id=hd0',
            '-device','virtio-blk-pci,drive=hd0','-snapshot','-vga','none',
            '-device',f'virtio-gpu-pci,xres={width},yres={height}','-display','none','-nic','none',
            '-serial','file:'+str(serial),'-qmp','unix:'+sock+',server=on,wait=off','-no-reboot']
        (folder/'command.json').write_text(json.dumps(cmd,indent=2))
        log=(folder/'qemu.log').open('wb');qemu=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT);ui=None
        try:
            text=lambda:serial.read_text(errors='replace') if serial.exists() else ''
            wait(lambda:'desktop live' in text(),'desktop did not start',180)
            # Finder is started automatically AFTER "desktop live". Exclude
            # that unrelated launch before launch_app takes its serial mark;
            # otherwise it diagnoses a correct Clock click as the wrong app.
            wait(lambda:'[wm] launched Finder' in text(),'startup Finder did not launch')
            ui=Session(sock,serial=str(serial));probe=str(folder/'probe.ppm')
            ui.launch_app('clock',title='Clock',probe=probe)
            wait(lambda:'OPENLOGIT_CLOCK api=1.1 backend=software ready' in text(),'Clock did not initialize OpenLogit')
            window=wait(lambda:win_by_title(str(serial),'Clock'),'guest did not report Clock bounds')
            ui.goto(width-pt(40),pt(40));time.sleep(1)
            points=[]
            for i in range(4):
                ppm=folder/f'frame-{i}.ppm';ui.screendump(str(ppm),settle=.1)
                image=Image.open(ppm).convert('RGB');image.save(folder/f'frame-{i}.png');points.append(blue_face(image,window));time.sleep(1.1)
            check(min(map(len,points))>12,f'{scale}%: OpenLogit clock face reaches display scanout')
            check(any(len(points[i]^points[0])>12 for i in range(1,len(points))),f'{scale}%: second hand changes on displayed frames')
            before=window.copy();x=window['x']+window['w']-1;y=window['y']+window['h']-1
            drag(ui,probe,x,y,x+pt(60),y+pt(60));ui.goto(width-pt(40),pt(40));time.sleep(2)
            window=win_by_title(str(serial),'Clock')
            check(window and window['cw']>before['cw'] and window['ch']>before['ch'],f'{scale}%: real window resize changes render target dimensions')
            ppm=folder/'resized.ppm';ui.screendump(str(ppm),settle=.1);image=Image.open(ppm).convert('RGB');image.save(folder/'resized.png')
            check(len(blue_face(image,window))>12,f'{scale}%: recreated OpenLogit surface displays after resize')
            (folder/'window.json').write_text(json.dumps(window,indent=2))
        finally:
            if ui:
                try:ui.f.close();ui.s.close()
                except OSError:pass
            stop_owned(qemu);log.close()
result(True);(out/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
print(f'OpenLogit guest: {len(checks)} display checks passed',flush=True)
