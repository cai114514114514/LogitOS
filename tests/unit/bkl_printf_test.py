#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-printf");a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];b=pathlib.Path(a.build);b.mkdir(parents=True,exist_ok=True)
s=(r/"c/kernel/diag/kprintf.c").read_text().replace('#include "../../drivers/core/io_lock.h"','#include "'+str(r/"c/drivers/core/io_lock.h")+'"')
source=b/"kprintf.c";source.write_text(s)
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-I"+str(r/"c/kernel/core"),"-I"+str(r/"c/drivers/char")]
(b/"io.h").write_text("#include <stdint.h>\nuint8_t inb(uint16_t);\nvoid outb(uint16_t,uint8_t);\n")
serial=(r/"c/drivers/char/serial.c").read_text()
# Force a switch after the UART leaf unlock, never while its FIFO is owned.
serial=serial.replace('#include "serial.h"', '#include "serial.h"\nvoid printf_byte_pause(void);').replace("if (ready) return;", "if (ready) { printf_byte_pause(); return; }")
base += ["-I"+str(b),"-I"+str(r/"c/drivers/core")]
for control in ("printf-no-lock","serial-no-message-lock",None):
    name=control or "printf-positive";exe=b/name;cmd=base[:]
    if control:cmd += ["-DPRINTF_CTL_RACE"]
    if control=="printf-no-lock":cmd += ["-DIO_NO_LOCK"]
    sp=b/"serial.c"
    sp.write_text(serial.replace("uint64_t flags = kprintf_console_enter();", "uint64_t flags = UINT64_MAX;") if control=="serial-no-message-lock" else serial)
    subprocess.run(cmd+[str(r/"tests/unit/bkl_printf_test.c"),str(source),str(sp),"-o",str(exe)],check=True)
    q=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if control:
        if q.returncode==0 or "FAIL: simultaneous kprintf messages stay parseable and complete" not in out:raise SystemExit("control did not detect interleaved records\n"+out)
        print("CONTROL",control,"caught: interleaved formatted/raw records")
    elif q.returncode:raise SystemExit(out)
    else:print(out,end="")
