#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual Unix/lsock/poll/wait code, with semantic controls first."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
# These INET branches are outside this test. Reaching one aborts by name; no
# transport response or readiness is invented to make local sockets pass.
UNUSED='tcp_file_poll tcp_poll_wake tcp_recv tcp_wait_readable tcp_send_nb tcp_wait_writable tcp_listen_close tcp_close udp_close raw_icmp_close raw_icmp_open tcp_connect_owned tcp_connect_status udp_bind tcp_listen_addr tcp_accept tcp_accept_wait tcp_peer tcp_listen_port tcp_local tcp_set_nodelay tcp_shutdown_write raw_icmp_recv udp_recv raw_icmp_send udp_send_to tcp_server_stats net_addr net_lock net_unlock'.split()
def run(command):
 p=subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p

def variant(build,name,control=None):
 out=build/name;out.mkdir(parents=True,exist_ok=True)
 flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-D_FORTIFY_SOURCE=0','-pthread','-fsanitize=address,undefined']
 for d in ['c','c/kernel/core','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/cpu','c/kernel/cpu/acpi','c/kernel/cpu/irq','c/kernel/cpu/smp','c/kernel/cpu/acpi','c/kernel/cpu/irq','c/kernel/cpu/smp','c/kernel/sched','c/kernel/exec','c/kernel/exec/load','c/kernel/exec/signal','c/kernel/exec/fd','c/kernel/exec/load','c/kernel/exec/signal','c/kernel/exec/fd','c/kernel/mm','c/kernel/mm/phys','c/kernel/mm/virt','c/kernel/mm/cache','c/kernel/mm/reclaim','c/kernel/mm/phys','c/kernel/mm/virt','c/kernel/mm/cache','c/kernel/mm/reclaim','c/drivers/timer','c/net/core','c/net/transport','c/net/ip','c/fs','c/fs/vfs','c/fs/logitfs','c/fs/cache','c/fs/ramfs','c/fs/ctl','c/fs/procfs','c/fs/vfs','c/fs/logitfs','c/fs/cache','c/fs/ramfs','c/fs/ctl','c/fs/procfs','include/abi']:
  flags+=['-iquote',str(ROOT/d)]
 source=ROOT/'c/net/core/unix.c';unix=source.read_text()
 if control=='register':
  old='if (LOGIT_HAVE(poll_wait)) poll_wait(pt, q);';assert unix.count(old)==1;unix=unix.replace(old,'(void)q;',1)
 if control=='peer-shutdown':
  old=' || (peer->rd_shut && peer->wr_shut)';assert unix.count(old)==1;unix=unix.replace(old,'',1)
 if control=='datagram-wake':
  old='waitq_wake_all(&unix_dgram_pollq);';assert unix.count(old)==1;unix=unix.replace(old,'',1)
 if control:
  unix=re.sub(r'#include "(\.\./[^"\n]+)"',lambda m:'#include "'+str((source.parent/m[1]).resolve())+'"',unix)
  source=out/'unix.c';source.write_text(unix)
 config=out/'net-config.c';config.write_text('#include "net.h"\nstruct net_config net_cfg;\n')
 objects=[]
 for label,path,extra in [('config',config,[]),('unix',source,[]),('lsock',ROOT/'c/net/core/lsock.c',['-DUNIX_POLL_NEGCTL_NVAL'] if control=='nval' else []),('sched',ROOT/'tests/unit/pollhost/hostsched.c',['-Dsched_block_self_unlock_until=unix_host_block_until']),('wait',ROOT/'c/kernel/sync/wait.c',[]),('poll',ROOT/'c/kernel/exec/fd/kpoll.c',[])]:
  obj=out/(label+'.o');run(flags+extra+['-c',path,'-o',obj]);objects.append(obj)
 stubs=out/'unused-inet.c';stubs.write_text('#include <stdio.h>\n#include <stdlib.h>\n'+''.join('long '+fn+'(void){fprintf(stderr,"UNEXPECTED INET: '+fn+'\\n");abort();}\n' for fn in UNUSED))
 exe=out/'test';run(flags+[ROOT/'tests/unit/unix_poll_test.c',stubs,*objects,'-o',exe])
 p=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=20);text=p.stdout+p.stderr;(out/'test.log').write_text(text)
 labels={'nval':'listener: valid empty listener is not NVAL','register':'registration: query-to-sleep event cannot be lost','datagram-wake':'datagram: peer drain wakes aggregate poll queue','peer-shutdown':'stream: full peer shutdown wakes HUP-only poll'}
 if control:
  if p.returncode!=1 or 'FAIL: '+labels[control] not in text:raise RuntimeError('INCONCLUSIVE '+name+'\n'+text)
  print('CONTROL',name,':',next(line for line in text.splitlines() if 'FAIL: '+labels[control] in line),flush=True)
 elif p.returncode:raise RuntimeError(text)
 else:print(text,end='',flush=True)

def main():
 p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--negative',action='store_true');a=p.parse_args();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
 paths=['c/net/core/unix.c','c/net/core/lsock.c','c/kernel/exec/fd/kpoll.c','c/kernel/sync/wait.c','tests/unit/unix_poll_test.c','tests/unit/unix_poll_test.py']
 (b/'sources.json').write_text(json.dumps({s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in paths},indent=2)+'\n')
 if a.negative:
  for control in ['nval','register','datagram-wake','peer-shutdown']:variant(b,control,control)
 else:variant(b,'positive')
if __name__=='__main__':main()
