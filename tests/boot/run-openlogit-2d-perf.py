#!/usr/bin/env python3
"""Paired guest-clock timings with every frame independently observed on screen.

The same guest, input sequence and object file serve both SDK archives. The
host controls pacing but never supplies frame timing. Warmup frames are saved
and excluded explicitly. Window submission duration is not a vblank duration.
"""
import argparse
import hashlib
import json
import math
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from qmp_ui import Session,configure
from qmp_window import win_by_title
from qmp_repaint import Meter
from owned_process import stop_owned

parser=argparse.ArgumentParser()
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--output',default='perf-guest')
args=parser.parse_args()
build=args.build.resolve()
out=build/args.output
out.mkdir(exist_ok=True)
serial=out/'serial.log'
serial.write_text('')
process=ui=None
results={}
checks=[]

def log():
    return serial.read_text(errors='replace')

def record(complete=False):
    (out/'result.json').write_text(json.dumps({'complete':complete,'checks':checks,'runs':results,
        'passed':bool(checks) and all(item['passed'] for item in checks)},indent=2)+'\n')

def check(ok,name):
    checks.append({'name':name,'passed':bool(ok)})
    record()
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    if not ok: raise AssertionError(name)

def wait(predicate,name,timeout=120):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if process.poll() is not None or 'BENCH ERROR' in log(): raise RuntimeError('guest failure: '+name)
        result=predicate()
        if result: return result
        time.sleep(.015)
    raise TimeoutError(name)

def send(command):
    process.stdin.write((command+'\n').encode())
    process.stdin.flush()

def tap(key):
    for down in [True,False]:
        response=ui._input([{'type':'key','data':{'down':down,'key':{'type':'qcode','data':key}}}])
        if 'error' in response: raise RuntimeError(response)
        time.sleep(.02)

def window():
    return win_by_title(str(serial),'OpenLogit 2D Bench')

def position_window():
    # Window creation cascades and eventually wraps. Fix the actual geometry
    # before warmup so desktop overlap and compositor damage are comparable.
    for attempt in range(3):
        w=window()
        if (w['x'],w['y']) == (160,90):
            ui.settle_pointer(str(out/'pointer.ppm'),100,50)
            return w
        ui.settle_pointer(str(out/'pointer.ppm'),w['x']+300,w['y']+15)
        ui._input([{'type':'btn','data':{'button':'left','down':True}}])
        time.sleep(.08)
        ui.settle_pointer(str(out/'pointer.ppm'),460,105)
        ui._input([{'type':'btn','data':{'button':'left','down':False}}])
        time.sleep(.25)
    raise RuntimeError('benchmark window did not reach fixed geometry')

def screenshot():
    path=out/'frame.ppm'
    response=ui.cmd({'execute':'screendump','arguments':{'filename':str(path)}})
    if 'error' in response: raise RuntimeError(response)
    image=Image.open(path).convert('RGB')
    w=window()
    x,y=w['x'],w['y']+w['h']-w['ch']
    return image.crop((x,y,x+600,y+400))

def sample(tag,kind,index):
    pattern=rf'BENCH FRAME tag={tag} case={kind} n={index} ns=(\d+) hash=(\d+) copied=(\d+) damage=(\d+),(\d+),(\d+),(\d+)'
    matches=re.findall(pattern,log())
    return tuple(map(int,matches[-1])) if matches else None

def statistics_for(samples):
    values=sorted(row['ns']/1e6 for row in samples[8:])
    return {'median_ms':statistics.median(values),'p95_ms':values[math.ceil(len(values)*.95)-1],
            'samples':len(values),'warmup_excluded':8,'p95_definition':'nearest rank'}

configure(1280,800)
record()
try:
    with tempfile.TemporaryDirectory(prefix='ol-perf-') as temporary,serial.open('wb') as stream:
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
        meter=Meter(ui,str(serial))
        # Alternate which library goes first across workloads; retain all raw
        # samples so clock noise cannot be hidden behind one aggregate number.
        for kind in range(3):
            variants=['before','after'] if kind%2==0 else ['after','before']
            for variant in variants:
                tag=f'{variant}{kind}'
                send(f'/bin/bench2d-{variant} {tag} {kind}')
                wait(lambda:f'BENCH READY tag={tag} case={kind}' in log() and window(),tag+' startup')
                geometry=position_window()
                time.sleep(.4)
                before=meter.mark()
                run={'frames':[],'observed_display_frames':0,'window_geometry':geometry}
                results[tag]=run
                for index in range(48):
                    tap('n')
                    row=wait(lambda:sample(tag,kind,index),tag+f' sample {index}')
                    marker=(index+1,(kind+1)*50,180)
                    def displayed():
                        image=screenshot()
                        return image if image.getpixel((12,12))==marker else None
                    image=wait(displayed,tag+f' display {index}',20)
                    run['observed_display_frames']+=1
                    run['frames'].append({'index':index,'ns':row[0],'surface_hash':row[1],
                                          'copied_bytes':row[2],'damage':list(row[3:]),
                                          'screen_sha256':hashlib.sha256(image.tobytes()).hexdigest()})
                    if index==47: image.save(out/(tag+'.png'))
                after=meter.mark()
                run['statistics']=statistics_for(run['frames'])
                run['compositor_delta']={key:after.get(key,0)-before.get(key,0) for key in after} if before and after else None
                rss=re.findall(rf'BENCH DONE tag={tag} case={kind} rss=(\d+)',log())
                run['rss_frames']=int(rss[-1]) if rss else None
                check(run['observed_display_frames']==48,tag+' has 48 independently observed display frames')
                tap('esc')
                wait(lambda:window() is None,tag+' exit',20)
            old,new=results[f'before{kind}'],results[f'after{kind}']
            check(all(a['surface_hash']==b['surface_hash'] and a['screen_sha256']==b['screen_sha256']
                      for a,b in zip(old['frames'],new['frames'])),f'case {kind} SDK and displayed pixels match every frame')
            ratio=new['statistics']['p95_ms']/old['statistics']['p95_ms']
            new['p95_ratio_to_baseline']=ratio
            print(f'case {kind}: before p95={old["statistics"]["p95_ms"]:.3f} ms; '
                  f'after={new["statistics"]["p95_ms"]:.3f} ms; ratio={ratio:.4f}',flush=True)
            # Report every workload even when one breaches the budget, so the
            # next fix can distinguish a common transport cost from a paint path.
            checks.append({'name':f'case {kind} p95 growth <= 10%','passed':ratio<=1.10})
            record()
        record(True)
        paths=[build/'logit.iso',build/'disk.img',build/'baseline/libopenlogit.a',build/'sdk/libopenlogit.a',
               build/'bench/openlogit_2d.o',build/'bench/2d-before.elf',build/'bench/2d-after.elf',
               ROOT/'tests/bench/openlogit_2d.c']
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
raise SystemExit(0 if all(check['passed'] for check in checks) else 1)
