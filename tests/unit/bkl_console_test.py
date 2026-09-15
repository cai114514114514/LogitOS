#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Race the real console leaves; controls must fail before positives run."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-console");p.add_argument("--source-dir");a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];b=pathlib.Path(a.build);b.mkdir(parents=True,exist_ok=True)
src=pathlib.Path(a.source_dir) if a.source_dir else r/"c/drivers/char"
(b/"io.h").write_text("#include <stdint.h>\nuint8_t inb(uint16_t);\nvoid outb(uint16_t,uint8_t);\n")
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-DVGA_HOST_TEST","-I"+str(b),"-I"+str(r/"c/drivers/char"),"-I"+str(r/"c/drivers/core"),"-I"+str(r/"c/kernel/core")]
vga=(src/"vga.c").read_text();serial=(src/"serial.c").read_text()
def run(name,mode,defs=(),expect=None):
    vs=vga
    if mode=="vga" and expect:
        vs=vs.replace('#include "vga.h"','#include "vga.h"\nint bkl_vga_index(int);')
        vs=vs.replace('VGA[cursor_row * VGA_WIDTH + cursor_col] =','VGA[bkl_vga_index(cursor_row * VGA_WIDTH + cursor_col)] =')
    vp=b/(name+"-vga.c");sp=b/(name+"-serial.c");vp.write_text(vs);sp.write_text(serial);exe=b/name
    subprocess.run(base+list(defs)+[str(r/"tests/unit/bkl_console_test.c"),str(vp),str(sp),"-o",str(exe)],check=True,cwd=r)
    q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if expect:
        if q.returncode==0 or expect not in out:raise SystemExit("control missed intended assertion: "+name+"\n"+out)
        print("CONTROL",name,"caught:",expect)
    elif q.returncode:raise SystemExit(out)
    else:print(out,end="")
run("vga-no-lock","vga",["-DIO_NO_LOCK"],"FAIL: VGA simultaneous writers preserve both characters")
run("serial-no-lock","serial",["-DIO_NO_LOCK","-DSERIAL_CTL_RACE"],"FAIL: UART ready and write are one transaction")
run("vga-positive","vga")
run("serial-positive","serial")
