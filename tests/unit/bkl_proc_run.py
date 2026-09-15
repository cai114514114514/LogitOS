#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production file/FD concurrency. The ownership negative control runs first."""
from pathlib import Path
import argparse, re, subprocess
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',default='/tmp/logitos-bkl-20260910/proc/host');a=p.parse_args()
build=Path(a.build);build.mkdir(parents=True,exist_ok=True)
# Extract only independently linkable real FD functions, without hand-copying
# their implementation. Process scheduler/MM lifecycle remains a guest gate.
s=(root/'c/kernel/exec/proc.c').read_text()
def function(name, source=s):
    s=source
    m=re.search(r'^(?:static )?(?:int|void|struct file \*|struct thread \*)\s*'+name+r'\([^;]*?\)\s*\{',s,re.M)
    if not m: raise RuntimeError('missing production function '+name)
    start=m.start();pos=m.end();depth=1
    # Bodies here have no brace characters in strings/comments (assert the
    # extracted translation unit compiles, including all control variants).
    while depth:
        if s[pos]=='{':depth+=1
        elif s[pos]=='}':depth-=1
        pos+=1
    return s[start:pos]
fd=build/'fd.c';fd.write_text('#include <stddef.h>\n#include "proc.h"\n#include "file.h"\n'+ '\n'.join(function(n) for n in ['proc_fd_alloc','proc_fd_acquire','proc_fd_close','proc_fd_close_if','proc_fd_dup2','proc_fd_pair','proc_fd_clone','proc_fd_close_all']))
model=(root/'tests/unit/storhost/hostmodel.c').read_text()
start=model.index('/* --- locks /');end=model.index('/* --- the VFS model',start)
model=model[:start]+'''\nvoid sched_poll_wait(void) { abort(); }
void ksig_tty_claim_fg(void) { abort(); }
int ksig_tty_getc(void) { abort(); }
int ksig_interrupted(void) { return 0; }
struct waitq *ksig_tty_waitq(void) { abort(); }
int ksig_tty_avail(void) { return 0; }
int ksig_post_current(int signo) { (void)signo; return 0; }
'''+model[end:]
modelpath=build/'model.c';modelpath.write_text(model)
incs=['c','c/kernel/exec','c/kernel/mm','c/kernel/core','c/kernel/cpu','c/kernel/sched','c/fs','c/drivers/char','c/drivers/timer','include/abi']
flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-pthread','-fsanitize=address,undefined']
for inc in incs:flags+=['-iquote',str(root/inc)]
src=[root/'tests/unit/bkl_proc_test.c',modelpath,fd,root/'tests/unit/pollhost/hostsched.c',root/'c/kernel/core/wait.c',root/'c/kernel/exec/kpoll.c',root/'c/kernel/exec/file.c']
for name,extra,args in [('fd-borrow',['-DBKL_NEGCTL_FD_BORROW'],['control']),('positive',[],[])]:
    exe=build/name
    subprocess.run(flags+extra+[str(x) for x in src]+['-o',str(exe)],check=True)
    r=subprocess.run([str(exe)]+args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
    (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
    if name=='fd-borrow':
        if r.returncode==0 or 'FAIL acquire owns an independent reference' not in r.stdout:raise SystemExit('negative control failed to detect the missing reference')
        print('negative control: dangling FD ownership caught')
    elif r.returncode:raise SystemExit(r.returncode)

# Compile the actual descriptor state transitions and final-thread election.
# As with FD extraction, only architecture/platform seams are modelled.
u=(root/'c/kernel/sched/uthread.c').read_text()
exit_src=build/'exit.c'
exit_src.write_text(u[:u.index('int uthread_proc_live(')] + '\n' + function('uthread_release_self',u) + '\n' + (root/'tests/unit/bkl_exit_test.c').read_text())
for name,extra in [('exit-no-owner',['-DBKL_NEGCTL_EXIT_ELECTION']),('exit-positive',[])]:
    exe=build/name
    subprocess.run(flags+extra+[str(exit_src),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
    (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
    if name=='exit-no-owner':
        if r.returncode==0 or 'FAIL last exit records a durable unique owner' not in r.stdout:raise SystemExit('negative control failed to detect missing teardown owner')
        print('negative control: missing final-thread ownership caught')
    elif r.returncode:raise SystemExit(r.returncode)

# Synchronous fault ownership and futex physical aliases are exercised with
# deterministic interleavings, each preceded by its former-behavior control.
sig=(root/'c/kernel/exec/ksignal.c').read_text()
signal_src=build/'signal.c'
signal_src.write_text(sig[:sig.index('static void reset_locked(')]+'\n'+function('ksig_fault',sig)+'\n'+(root/'tests/unit/bkl_signal_test.c').read_text())
futex_src=build/'futex.c'
futex_src.write_text(u[:u.index('/* --- the table,')]+u[u.index('#define FBUCKETS '):u.index('/* --- syscall dispatch')]+(root/'tests/unit/bkl_futex_test.c').read_text())
for tag,source,define,assertion in [
 ('signal',signal_src,'BKL_NEGCTL_SIGNAL_OWNER','fault delivery stays on originating thread'),
 ('futex',futex_src,'BKL_NEGCTL_FUTEX_UNPINNED','futex compares pinned page after concurrent VA replacement')]:
    for control in [True,False]:
        name=tag+('-control' if control else '-positive');exe=build/name
        subprocess.run(flags+(['-D'+define] if control else [])+[str(source),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
        (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
        if control:
            if r.returncode!=1 or 'FAIL '+assertion not in r.stdout:raise SystemExit(name+' did not fail its named assertion')
            print('negative control: '+tag+' former behavior caught')
        elif r.returncode:raise SystemExit(r.returncode)

argv_src=build/'argv.c'
argv_src.write_text('#define _POSIX_C_SOURCE 200112L\n#include <stdint.h>\nint user_copy_from(void *,const void *,uint64_t);\n'+function('user_strnlen',(root/'c/kernel/exec/exec.c').read_text())+'\n'+(root/'tests/unit/bkl_argv_test.c').read_text())
for control in [True,False]:
    name='argv-'+('control' if control else 'positive');exe=build/name
    subprocess.run(flags+(['-DBKL_NEGCTL_ARGV_BYTEWISE'] if control else [])+[str(argv_src),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
    (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
    if control:
        if r.returncode!=1 or 'FAIL argv validation uses sixteen copy calls' not in r.stdout:raise SystemExit('argv bytewise control did not fail its round-trip bound')
        print('negative control: bytewise argv validation caught')
    elif r.returncode:raise SystemExit(r.returncode)

# Production scheduler latch: actual wake lookup/state transitions are linked;
# only the context switch is represented by the helper's park decision.
sched=(root/'c/kernel/sched/sched.c').read_text()
block=sched[sched.index('static void block_self('):sched.index('static int wake_locked(')]
if 'if (wake_pending_take_locked(self)) {' not in block:raise SystemExit('production block_self no longer consumes the tested latch')
wake_names=['wake_pending_take_locked','wake_pending_note_locked','wake_locked','by_id_locked','sched_wake_id']
(build/'wake_production.inc').write_text('\n'.join(function(n,sched) for n in wake_names))
for control in [True,False]:
    name='wake-'+('control' if control else 'positive');exe=build/name
    subprocess.run(flags+['-I'+str(build)]+(['-DBKL_NEGCTL_WAKE_LATCH'] if control else [])+[str(root/'tests/unit/bkl_wake_test.c'),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
    (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
    if control:
        if r.returncode!=1 or 'FAIL post between predicate check and park prevents sleeping' not in r.stdout:raise SystemExit('wake control did not fail its missing-event assertion')
        print('negative control: wake before park was discarded')
    elif r.returncode:raise SystemExit(r.returncode)

# Zombie reclamation must revalidate ownership after taking the remote AS
# guard: another reaper can free and reuse CR3 after the process snapshot.
(build/'reap_production.inc').write_text(function('oom_task_reap_dead'))
for control in [True,False]:
    name='reap-'+('control' if control else 'positive');exe=build/name
    subprocess.run(flags+['-I'+str(build)]+(['-DBKL_NEGCTL_OOM_REAP'] if control else [])+[str(root/'tests/unit/bkl_reap_test.c'),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
    (build/(name+'.log')).write_text(r.stdout);print(r.stdout,end='')
    if control:
        if r.returncode!=1 or 'FAIL reap rejects recycled CR3 after snapshot before guard' not in r.stdout:raise SystemExit('OOM control did not fail its recycled-address-space assertion')
        print('negative control: stale zombie CR3 ownership caught')
    elif r.returncode:raise SystemExit(r.returncode)
