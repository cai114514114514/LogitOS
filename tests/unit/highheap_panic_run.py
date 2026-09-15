#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual panic range helpers/unwind loops, with required negative controls."""
from pathlib import Path
import argparse,re,subprocess,tempfile
ap=argparse.ArgumentParser();ap.add_argument('--source',default=str(Path(__file__).resolve().parents[2]/'c/kernel/core/panic.c'));ap.add_argument('--build',type=Path,required=True);a=ap.parse_args()
s=Path(a.source).read_text()
def func(name):
    m=re.search(r'(?m)^(?:static )?(?:int|void) '+name+r'\(',s)
    if not m: raise SystemExit(f'missing production function: {name}')
    pos=s.index('{',m.start()); depth=1; end=pos+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[m.start():end]
parts=[func(n) for n in ['kernel_text_contains','is_call_site','stack_readable','frame_ok','backtrace','print_frame','backtrace_print']]
preamble=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#define KIMAGE_LO 0x2000000UL
#include "physmap.h"
#undef PHYSMAP_BASE
#define PHYSMAP_BASE alias_base
#define STACK_SPAN (64*1024)
#define BT_MAX 24
#define RAM_BYTES (64*1024)
#define LOGIT_HAVE(x) 0
static uint8_t code[64];
#define _kernel_end (code+sizeof code)
static uint64_t alias_base=0xffff800000000000ull;
static int ready=1,low_ready=1,checks,logs;
int pmm_physmap_ready(void){return ready;}
int pmm_physmap_low_ready(void){return low_ready;}
static int pmm_is_ram(uint64_t p,size_t n){
  if(p>=PMM_LOW_LIMIT)p-=PMM_LOW_LIMIT;
  return n && p<RAM_BYTES && n<=RAM_BYTES-p;
}
static const char *ksym_lookup(uint64_t a,uint64_t *off){(void)a;*off=0;return NULL;}
static void klog(int level,const char *fmt,...){(void)level;(void)fmt;logs++;}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
'''
main=r'''
int main(int argc,char **argv){
  (void)argc;(void)argv;
  CHECK(!stack_readable(0x100000,16));
  CHECK(stack_readable(KIMAGE_LO,16));
  CHECK(stack_readable(PMM_LOW_LIMIT-16,16));
  CHECK(!stack_readable(PMM_LOW_LIMIT-8,16));
  CHECK(!stack_readable(PMM_LOW_LIMIT,16));
  CHECK(!stack_readable(1ull<<40,16));
  CHECK(!stack_readable(PHYSMAP_BASE-8,16));
  CHECK(stack_readable(PHYSMAP_BASE,16));
  CHECK(stack_readable(PHYSMAP_BASE+RAM_BYTES-16,16));
  CHECK(!stack_readable(PHYSMAP_BASE+RAM_BYTES-8,16));
  CHECK(!stack_readable(PHYSMAP_BASE+RAM_BYTES,16));
  CHECK(!stack_readable(PHYSMAP_BASE+PHYSMAP_SIZE,16));
  CHECK(!stack_readable(UINT64_MAX-7,16));
  CHECK(!stack_readable(UINT64_MAX,1));
  CHECK(!stack_readable(PHYSMAP_BASE,0));
  ready=low_ready=0;CHECK(!stack_readable(PHYSMAP_BASE,16));
  CHECK(!stack_readable(PHYSMAP_BASE+PMM_LOW_LIMIT,16));
  low_ready=1;CHECK(stack_readable(PHYSMAP_BASE,16));
  CHECK(!stack_readable(PHYSMAP_BASE+PMM_LOW_LIMIT,16));
  CHECK(!stack_readable(0x100000,16));
  CHECK(stack_readable(KIMAGE_LO,16));ready=1;
  CHECK(stack_readable(PHYSMAP_BASE+PMM_LOW_LIMIT,16));
  CHECK(!frame_ok(PHYSMAP_BASE+1,PHYSMAP_BASE));
  CHECK(!frame_ok(PHYSMAP_BASE+16,PHYSMAP_BASE+32));
  CHECK(!frame_ok(PHYSMAP_BASE+STACK_SPAN,PHYSMAP_BASE));
  CHECK(!frame_ok(UINT64_MAX-7,UINT64_MAX-63));
  uint8_t *arena=mmap(NULL,3*RAM_BYTES,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
  CHECK(arena!=MAP_FAILED);CHECK(mprotect(arena,RAM_BYTES,PROT_READ|PROT_WRITE)==0);
  alias_base=(uintptr_t)arena;code[11]=0xe8;
  uint64_t ret=(uintptr_t)(code+16),out[8]={0};
  uint64_t *f=(uint64_t*)arena;
  f[0]=(uintptr_t)(f+2);f[1]=ret;f[2]=(uintptr_t)(f+4);f[3]=ret;f[4]=0;f[5]=ret;
  CHECK(backtrace((uintptr_t)f,(uintptr_t)f,out,8)==3);
  CHECK(out[0]==ret&&out[1]==ret&&out[2]==ret);
  f[2]=(uintptr_t)f;CHECK(backtrace((uintptr_t)f,(uintptr_t)f,out,8)==2);
  memset(arena,0,RAM_BYTES);
  uint64_t *end=(uint64_t*)(arena+RAM_BYTES-16);end[0]=0;end[1]=ret;
  CHECK(backtrace((uintptr_t)end,(uintptr_t)end,out,8)==1);
  CHECK(backtrace((uintptr_t)(end+1),(uintptr_t)end,out,8)==0);
  logs=0;backtrace_print(0,(uintptr_t)end,(uintptr_t)end,8);CHECK(logs>=3);
  logs=0;backtrace_print(0,0,(uintptr_t)(arena+RAM_BYTES-8),8);CHECK(logs>=3);
  munmap(arena,3*RAM_BYTES);
  printf("PASS actual panic helpers/unwind: %d assertions, guarded high alias/holes/overflow\n",checks);
  return 0;
}
'''
if True:
 d=a.build.resolve();d.mkdir(parents=True,exist_ok=True)
 # Negatives execute before positive; an assertion or guard-page fault is required.
 for name,body in [
  ('no-low-readiness', '\n\n'.join(parts).replace('!pmm_physmap_low_ready()', '0')),
  ('full-ready-for-low', '\n\n'.join(parts).replace('!pmm_physmap_low_ready()', '!pmm_physmap_ready()')),
  ('no-frame-guard', '\n\n'.join(parts).replace(' &&\n           stack_readable(fp, 16)','')),
  ('no-scan-guard','\n\n'.join(parts).replace('            if (!stack_readable(p, 8)) break;\n','')),
  ('positive','\n\n'.join(parts))]:
  c=d/(name+'.c');exe=d/name;c.write_text(preamble+body+main)
  subprocess.run(['clang','-std=c11','-D_DARWIN_C_SOURCE','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(Path(a.source).resolve().parents[1]/'mm'),str(c),'-o',str(exe)],check=True)
  r=subprocess.run([str(exe)],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  (d/(name+'.log')).write_text(r.stdout+r.stderr)
  if name=='positive':
   print(r.stdout,end='');print(r.stderr,end='');assert r.returncode==0,r.returncode
  else:
   if name=='no-low-readiness':
    assert r.returncode==1 and '!stack_readable(PHYSMAP_BASE,16)' in r.stderr,r.stderr
   elif name=='full-ready-for-low':
    assert r.returncode==1 and 'CHECK' not in r.stderr and 'stack_readable(PHYSMAP_BASE,16)' in r.stderr,r.stderr
   elif name=='no-frame-guard':
    assert r.returncode==1 and 'FAIL' in r.stderr and '!frame_ok(UINT64_MAX-7' in r.stderr,r.stderr
   else:
    import signal
    assert r.returncode in (-signal.SIGBUS,-signal.SIGSEGV),r.returncode
   print(f'CONTROL CAUGHT {name}: exit {r.returncode} '+r.stderr.strip())
