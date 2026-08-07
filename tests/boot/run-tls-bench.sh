#!/usr/bin/env bash
# Where the seconds of a TLS handshake actually go, measured on the machine.
#
# WHY THIS EXISTS
# ---------------
# c/apps/browser/browser_rt.c schedules the whole fetch layer around one
# sentence -- that a handshake on an emulated CPU "costs SECONDS" -- and until
# this harness there was no number behind it, let alone a number per phase. A
# handshake is a round trip, an ECDH, a DER parse, one to five signature
# verifications, a key schedule and some bulk AEAD, and those differ from each
# other by three orders of magnitude. "TLS is slow" names no code; this does.
#
# HOW IT WORKS
# ------------
# The instrument is kprof (c/kernel/core/kprof.c) driven from the guest's own
# serial shell -- no debugger, no host tooling, no second profiler:
#
#     echo spans 1 > /dev/kprof      turn span recording on
#     echo reset   > /dev/kprof      zero the accumulators (once per site)
#     net get <url>                  ONE real handshake plus the fetch
#     cat /dev/kprof                 the span table
#
# The spans are placed in c/net/tls/{tls,tls12}.c and c/net/tls/x509.c; see
# c/net/tls/tls_prof.h for the list and for why each one is where it is.
#
# WHAT THE NUMBERS MEAN, AND WHAT THEY DO NOT
# -------------------------------------------
# These are TCG numbers. They are not a claim about hardware -- QEMU's
# interpreter costs roughly an order of magnitude, and it does not cost it
# uniformly (a table-driven byte loop and a 64x64 multiply are not slowed by the
# same factor). What they ARE is the ratio that decides what to optimise, on the
# machine the user actually runs, which is the only place the browser is slow.
#
# tls_netwait is the round-trip term: time inside net_poll()/net_idle() while
# the handshake waits for the peer. It is measured rather than inferred so that
# "the handshake took 4 s" can be split into "3 s of that was the network, which
# no crypto change touches" versus the opposite -- which is the entire question.
#
# Usage: run-tls-bench.sh <iso> <disk.img> [url ...]
# Env:   TLS_BENCH_SMP    (default 4)      QEMU -smp
#        TLS_BENCH_SETTLE (default 18)     seconds to wait for boot
#        TLS_BENCH_EACH   (default 30)     seconds budgeted per site
set -u

ISO="${1:?usage: run-tls-bench.sh <iso> <disk.img> [url ...]}"
DISK="${2:?usage: run-tls-bench.sh <iso> <disk.img> [url ...]}"
shift 2

if [ $# -gt 0 ]; then
    URLS=("$@")
else
    # Two sites, chosen because they are the two handshakes the real web
    # actually hands us and they exercise disjoint code:
    #   cloudflare.com  TLS 1.3, x25519, ECDSA leaf -- the common path
    #   www.baidu.com   TLS 1.2 only, secp256r1, ECDHE-RSA -- the NIST bignum
    #                   and the RSA verifier, i.e. everything x25519 avoids
    URLS=(https://cloudflare.com/ https://www.baidu.com/)
fi

QEMU="${QEMU:-qemu-system-x86_64}"
SMP="${TLS_BENCH_SMP:-4}"
SETTLE="${TLS_BENCH_SETTLE:-18}"
EACH="${TLS_BENCH_EACH:-30}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

{
    sleep "$SETTLE"
    printf 'echo spans 1 > /dev/kprof\n'
    sleep 2
    for u in "${URLS[@]}"; do
        printf 'echo reset > /dev/kprof\n'
        sleep 1
        printf 'echo TLSBENCH-BEGIN %s\n' "$u"
        printf 'net get %s\n' "$u"
        sleep "$EACH"
        printf 'cat /dev/kprof\n'
        sleep 8                 # the span table is ~15 serial lines and the
                                # console is slow; 4 s truncated it mid-table
        printf 'echo TLSBENCH-END %s\n' "$u"
        sleep 1
    done
    sleep 3
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp "$SMP" \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

deadline=$(( $(date +%s) + SETTLE + 40 + (EACH + 12) * ${#URLS[@]} ))
last="${URLS[${#URLS[@]}-1]}"
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -aq "TLSBENCH-END $last" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done

any=0
for u in "${URLS[@]}"; do
    esc="$(printf '%s' "$u" | sed 's|[/.*]|\\&|g')"
    seg="$(sed -n "/TLSBENCH-BEGIN $esc/,/TLSBENCH-END/p" "$LOG" 2>/dev/null)"
    ver="$(printf '%s\n' "$seg" | grep -ao 'ServerHello: TLS 1\.[23]' | head -1 | grep -ao '1\.[23]')"
    grp="$(printf '%s\n' "$seg" | grep -ao 'group [a-z0-9]*' | head -1 | awk '{print $2}')"
    bytes="$(printf '%s\n' "$seg" | grep -ao 'http bytes [0-9]*' | tail -1 | awk '{print $3}')"
    echo
    echo "=== $u   TLS ${ver:-?}  group ${grp:-?}  body ${bytes:-0} bytes  (TCG, -smp $SMP) ==="
    # The denominator has to be present or the table is meaningless -- a run
    # whose serial output was cut off mid-table used to divide by zero in awk
    # and report nothing at all about the rows it DID capture.
    tbl="$(printf '%s\n' "$seg" | grep -a '^span ' || true)"
    printf '%s\n' "$tbl" | grep -aq '^span tls_handshake ' || tbl=""
    if [ -z "$tbl" ]; then
        echo "  (no span table -- did the fetch reach a handshake?)"
        printf '%s\n' "$seg" | grep -a '\[tls\]' | head -6
        continue
    fi
    any=1
    printf '%s\n' "$tbl" | awk '
        { name=$2; cnt=$3; tot=$4; mx=$5;
          n[name]=cnt; t[name]=tot; m[name]=mx; order[++k]=name
          if (name == "tls_handshake") hs=tot }
        END {
            printf "  %-18s %6s %12s %12s %12s   %s\n",
                   "phase","calls","total_ms","avg_us","max_us","share of handshake"
            for (i=1;i<=k;i++) { nm=order[i]
                share = (hs>0) ? 100.0*t[nm]/hs : 0
                printf "  %-18s %6d %12.2f %12.1f %12.1f   %6.1f%%\n",
                       nm, n[nm], t[nm]/1e6, (n[nm]?t[nm]/n[nm]/1e3:0), m[nm]/1e3, share
            }
        }'
done

[ "$any" = 1 ] || {
    echo "FAIL: no span table from any site"
    echo "----- serial tail -----"; tail -60 "$LOG"; exit 1; }
echo
echo "PASS: per-phase handshake breakdown captured"
exit 0
