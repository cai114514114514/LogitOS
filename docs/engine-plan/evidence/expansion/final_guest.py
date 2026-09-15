import sys,subprocess,time,pathlib,re,json
from PIL import Image
sys.path.insert(0,'tests/qmp');from qmp_ui import Session
out=pathlib.Path('/tmp/logitos-browser-expansion-evidence');serial=out/'final.serial.txt';serial.write_text('');sock=str(out/'final.sock')
p=subprocess.Popen(['qemu-system-x86_64','-cpu','max','-cdrom','build/logit.iso','-drive','file=build/disk.img,format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-boot','d','-snapshot','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-netdev','user,id=n0','-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+sock+',server,nowait'],stdout=open(out/'final.qemu.log','w'),stderr=subprocess.STDOUT)
def wait(marker,timeout=45,start=0):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  s=serial.read_text(errors='replace')[start:]
  if marker in s:return s
  if p.poll() is not None:raise RuntimeError('QEMU exited')
  time.sleep(.2)
 raise RuntimeError('Guest marker missing: '+marker)
def snap(name):
 f=out/(name+'.ppm');u.screendump(str(f));Image.open(f).save(out/(name+'.png'))
def nav(url):
 u.key_mods(('ctrl',),'l',settle=.25);u.typ(url);u.key('ret')
try:
 wait('desktop live',100);time.sleep(3);u=Session(sock,serial=str(serial));u.launch_app('browser');time.sleep(3)
 u.key_mods(('ctrl',),'t',settle=.25);u.typ('http://10.0.2.2:18769/index.html');u.key('ret');wait('EXPANSION-RESULT');time.sleep(1);snap('final-workbench')
 nav('http://10.0.2.2:18769/storage.html');wait('SESSION:');time.sleep(2);u.click_at(242,378);time.sleep(2);snap('final-storage-written')
 s=serial.read_text(errors='replace');tokens=re.findall(r'guest-\d+',s);assert tokens,'no painted written token';token=tokens[-1];print('WROTE',token,flush=True)
 u.click_at(155,114);time.sleep(2);u.launch_app('browser');time.sleep(3);mark=len(serial.read_text(errors='replace'));u.key('ret');tail=wait('SESSION:',start=mark);time.sleep(2);snap('final-storage-reopened');tail=serial.read_text(errors='replace')[mark:];assert token in tail,'local marker lost after browser process restart';assert 'absent' in tail and 'tab-only' not in tail,'session marker unexpectedly restored';print('REOPEN PASS',token,flush=True)
 nav('http://10.0.2.2:18769/modal.html');time.sleep(3);snap('final-modal-open');u.click_at(410,432);u.key('esc');time.sleep(2);snap('final-modal-closed')
 (out/'final-verdict.json').write_text(json.dumps({'storage_token':token,'reopened_local':True,'reopened_session_absent':True,'workbench_marker':True,'modal_visual_steps':'open, background click, Escape; inspect screenshots'},indent=2));print('FINAL GUEST COMPLETE',flush=True)
finally:
 p.terminate();p.wait()
