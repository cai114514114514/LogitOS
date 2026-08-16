#!/usr/bin/env python3
"""Read the kernel's lock state out of a FROZEN guest, over QMP.

Pairs with tests/boot/run-smp-freeze-probe.sh. A ticket lock's (ticket,
serving) says how many cores are queued; spinlock_t's owner_ra/owner_cpu say
who has it -- and when the machine has stopped with every core spinning, the
holder is the only fact that matters and the only one the registers cannot
give you, because the holder is usually not on a core at all.

Usage: qmp_lockdump.py <qmp-unix-socket> <kernel.elf> [sym ...]
"""
import json
import socket
import subprocess
import sys


def symbols(elf):
    out = {}
    r = subprocess.run(["nm", elf], capture_output=True, text=True).stdout
    for ln in r.splitlines():
        p = ln.split()
        if len(p) >= 3:
            out[p[2]] = int(p[0], 16)
    return out


def nearest(elf_syms_sorted, addr):
    best, bestn = None, 0
    for a, n in elf_syms_sorted:
        if a <= addr and a > bestn:
            bestn, best = a, n
    return "%s+0x%x" % (best, addr - bestn) if best else "0x%x" % addr


def main():
    sock, elf = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or ["g_bkl", "g_sched_lock", "g_file_lock",
                             "g_proc_lock", "g_cred_lock", "kheap_lock"]
    syms = symbols(elf)
    text = sorted((a, n) for n, a in syms.items())

    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    f = s.makefile("rw")
    f.readline()
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()

    def mon(cmd):
        f.write(json.dumps({"execute": "human-monitor-command",
                            "arguments": {"command-line": cmd}}) + "\n")
        f.flush()
        return json.loads(f.readline()).get("return", "")

    def words(addr, n):
        out = []
        for chunk in mon("xp/%dwx 0x%x" % (n, addr)).split("\n"):
            parts = chunk.split()
            out += [int(w, 16) for w in parts[1:] if w.startswith("0x")]
        return out

    for nm in names:
        if nm not in syms:
            continue
        # struct spinlock_t: ticket, serving, (pad), owner_ra(8), owner_cpu
        w = words(syms[nm], 6)
        if len(w) < 6:
            print("    %-14s (unreadable)" % nm)
            continue
        ticket, serving = w[0], w[1]
        ra = w[2] | (w[3] << 32)
        cpu = w[4] if w[4] < 0x80000000 else w[4] - (1 << 32)
        held = "FREE" if cpu < 0 else ("held by cpu%d" % cpu)
        line = "    %-14s ticket=%-4u serving=%-4u queued=%-2u %s" % (
            nm, ticket, serving, ticket - serving, held)
        if cpu >= 0 or ra:
            line += "  taken at %s" % nearest(text, ra)
        print(line)

    if "g_bkl_owner" in syms:
        v = words(syms["g_bkl_owner"], 1)
        if v:
            o = v[0] if v[0] < 0x80000000 else v[0] - (1 << 32)
            print("    g_bkl_owner    = %d" % o)


if __name__ == "__main__":
    main()
