#!/usr/bin/env bash
# IPv6 on the machine, against real libslirp -- five claims, each asserted
# separately, plus the control that says IPv4 did not pay for any of it.
#
# The five:
#   DAD    a link-local address is formed from the MAC and survives Duplicate
#          Address Detection before anything is allowed to use it
#   RA     a Router Advertisement is received, validated and believed
#   SLAAC  a routable address is autoconfigured from the RA's prefix, and runs
#          its own DAD
#   ND     a neighbour's link-layer address is resolved through NS/NA
#   FETCH  a real 32 KiB HTTP body arrives over IPv6 -- the only one of the five
#          that means the stack is USABLE rather than merely talkative
#
# and the control:
#   V4     in the SAME boot, with IPv6 fully configured, an IPv4 fetch of the
#          same file returns byte-identical content; and in a second boot with
#          SLIRP's IPv6 switched off, so does everything else.
#
# The FETCH goes to SLIRP's own IPv6 host address, which libslirp forwards to
# the host's loopback -- the v6 twin of the 10.0.2.2 trick every other boot
# test here uses. It is driven through /bin/socktest because that is the one
# CLI in this OS that takes a host and a port as SEPARATE arguments:
# c/net/http/url.c splits a URL's authority at the first ':' and so cannot
# express an IPv6 literal at all (see the report -- that gap belongs to the
# HTTP line, not to this one).
#
# Usage: run-ip6-test.sh <iso> <disk.img>

set -u

ISO="${1:?usage: run-ip6-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-ip6-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
PORTFILE="$TMP/port"

# BOTH families must be named explicitly. `-netdev user,ipv6=on` on its own does
# NOT mean "add IPv6" -- QEMU reads a lone ipv6=on as IPv6-ONLY and switches
# IPv4 off, so the guest's DHCP fails and every v4 connection is refused before
# it starts. That looks exactly like "IPv6 broke IPv4", and it is not; it cost
# an afternoon here, so it is written down rather than just fixed.
NET6="user,id=n0,ipv4=on,ipv6=on"
NET4="user,id=n0,ipv4=on,ipv6=off"

# QEMU's default e1000 MAC, hence a deterministic EUI-64 link-local address.
LL="fe80::5054:ff:fe12:3456"
# libslirp's default IPv6 network: fec0::/64, host/router fec0::2. Site-local,
# which is the interesting part -- RFC 6724 ranks fec0::/10 BELOW IPv4, so this
# host correctly prefers IPv4 to it for ordinary destinations. Reaching it here
# is explicit (socktest is given the literal), not the result of a preference.
V6HOST="fec0::2"
V6SLAAC="fec0::5054:ff:fe12:3456"
# On-link (the RA advertised fec0::/64) but nobody is there, so Neighbour
# Discovery solicits and is never answered. The interesting half of an address
# family is what it does when the address is dead.
V6DEAD="fec0::9"

cleanup() {
    for p in ${QPID:-} ${HPID:-}; do
        kill "$p" 2>/dev/null; wait "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

# One server, bound to :: so it answers over IPv6 and over IPv4-mapped alike --
# so "the same file" really is the same file and the two phases' FNV hashes are
# comparable without qualification.
python3 - "$TMP" "$PORTFILE" <<'PY' &
import http.server, pathlib, socket, sys
root = pathlib.Path(sys.argv[1]); pf = pathlib.Path(sys.argv[2])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(32768)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_a): pass
class Dual(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6
srv = Dual(("::", 0), lambda *a, **k: Quiet(*a, directory=str(root), **k))
pf.write_text(str(srv.server_address[1]), encoding="ascii")
srv.serve_forever()
PY
HPID=$!

for _ in $(seq 1 100); do
    [ -s "$PORTFILE" ] && break
    kill -0 "$HPID" 2>/dev/null || { echo "FAIL: host HTTP server exited"; exit 1; }
    sleep 0.05
done
[ -s "$PORTFILE" ] || { echo "FAIL: host HTTP server did not publish a port"; exit 1; }
PORT="$(cat "$PORTFILE")"

# The byte pattern is fixed, so the hash of a correct 32768-byte body is fixed
# too. Asserting it -- rather than just "some bytes arrived" -- is what makes
# "the same file over both families" a real claim.
WANT_FNV=4213874117

# boot <logfile> <netdev-options> <shell command> [shell command ...]
#
# One command per argument, with a real pause between them: the guest's serial
# console is read by /bin/sh a line at a time and a second line pushed in while
# the first command is still running is not reliably buffered.
boot() {
    local log="$1" netopt="$2"; shift 2
    local cmds=("$@")
    { sleep 14; for c in "${cmds[@]}"; do printf '%s\n' "$c"; sleep 20; done; sleep 10; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
        -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        -netdev "$netopt" -device e1000,netdev=n0 \
        -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local budget=90
    while [ $budget -gt 0 ]; do
        grep -aq "IP6TEST_DONE" "$log" && break
        kill -0 "$QPID" 2>/dev/null || break
        sleep 1; budget=$((budget - 1))
    done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    QPID=
}

fails=0
say() {    # say <ok?> <label> <detail>
    if [ "$1" = "0" ]; then printf 'PASS  %-6s %s\n' "$2" "$3"
    else printf 'FAIL  %-6s %s\n' "$2" "$3"; fails=$((fails + 1)); fi
}
has() { grep -aqF "$2" "$1"; }

# ---------------------------------------------------------------------------
# Phase A: IPv6 up. Fetch over IPv6, then over IPv4 in the same boot.
# ---------------------------------------------------------------------------
LOGA="$TMP/a.log"
boot "$LOGA" "$NET6" \
     "socktest $V6HOST $PORT /probe.bin 4" \
     "socktest 10.0.2.2 $PORT /probe.bin 4" \
     "socktest $V6DEAD $PORT /probe.bin 1" \
     "echo IP6TEST_DONE"

echo "=== phase A: SLIRP with IPv6 on ==="

# 1. DAD -- and the ORDER matters: tentative first, ok after. An address that
#    is announced "ok" without ever having been tentative was never probed.
if has "$LOGA" "[ip6] link-local $LL tentative (dad)" && \
   has "$LOGA" "[ip6] dad ok $LL/64" && \
   [ "$(grep -an "tentative (dad)" "$LOGA" | head -1 | cut -d: -f1)" -lt \
     "$(grep -an "dad ok $LL/64" "$LOGA" | head -1 | cut -d: -f1)" ] 2>/dev/null
then say 0 DAD "link-local $LL formed from the MAC and passed DAD"
else say 1 DAD "no tentative->ok transition for $LL"; fi

# 2. Router Advertisement, from a link-local source, believed.
if grep -aq "\[ip6\] router fe80::[0-9a-f:]* lifetime [0-9]*s" "$LOGA"
then say 0 RA "$(grep -am1 -o '\[ip6\] router .*' "$LOGA")"
else say 1 RA "no router advertisement accepted"; fi

# 3. SLAAC: the routable address, formed from the RA prefix + EUI-64, and its
#    own DAD. A stack that skipped DAD on the SLAAC address would still print
#    the slaac line.
if has "$LOGA" "[ip6] slaac $V6SLAAC/64" && has "$LOGA" "[ip6] dad ok $V6SLAAC/64"
then say 0 SLAAC "$V6SLAAC autoconfigured from the RA prefix and passed DAD"
else say 1 SLAAC "no SLAAC address, or it never passed DAD"; fi

# 4. Neighbour Discovery: an NS/NA exchange that ended in a resolved MAC.
if grep -aq "\[ip6\] nd [0-9a-f:]* -> \([0-9a-f][0-9a-f]:\)\{5\}[0-9a-f][0-9a-f] \(reachable\|stale\)" "$LOGA"
then say 0 ND "$(grep -am1 -o '\[ip6\] nd .*' "$LOGA")"
else say 1 ND "no neighbour was resolved through NS/NA"; fi

# 5. The claim that matters: a real body, over IPv6, with the right bytes.
V6LINE="$(grep -a "SOCKTEST_OK" "$LOGA" | head -1)"
if has "$LOGA" "[sock] connected $V6HOST via ipv6" && \
   printf '%s' "$V6LINE" | grep -q "bytes=32768 fnv=$WANT_FNV"
then say 0 FETCH "32768 bytes over IPv6 from [$V6HOST]:$PORT, fnv=$WANT_FNV"
else say 1 FETCH "no complete IPv6 fetch (sock line: ${V6LINE:-none})"; fi

# 6. Control, same boot: IPv4 still returns the identical file while IPv6 is
#    fully configured. This is the regression that would actually happen --
#    QEMU turns SLIRP's IPv6 on by default, so every existing test in this
#    tree now boots with a configured v6 stack whether it asked for one or not.
# Note both SOCKTEST_OK lines are character-for-character identical -- that IS
# the claim ("the same file over both families"), so the two runs are told
# apart by counting them and by the per-family [sock] line, not by comparing
# their text.
V4LINE="$(grep -a "SOCKTEST_OK" "$LOGA" | sed -n 2p)"
NOK="$(grep -ac "SOCKTEST_OK" "$LOGA")"
if has "$LOGA" "[sock] connected 10.0.2.2 via ipv4" && \
   printf '%s' "$V4LINE" | grep -q "bytes=32768 fnv=$WANT_FNV" && \
   [ "$NOK" -ge 2 ]
then say 0 V4 "IPv4 fetch in the same boot is byte-identical (fnv=$WANT_FNV)"
else say 1 V4 "IPv4 regressed while IPv6 was up (sock line: ${V4LINE:-none})"; fi

# 7. A dead IPv6 destination must FAIL, not hang. Neighbour Discovery solicits
#    fec0::9 three times, nobody answers, ip6_output starts refusing, and TCP
#    runs out its handshake -- and the socket has to end up in SOCK_P_ERROR
#    rather than parked forever holding a connection slot. The machine must
#    still be alive afterwards, which the IP6TEST_DONE marker below proves.
if has "$LOGA" "SOCKTEST_FAIL" && has "$LOGA" "IP6TEST_DONE"
then say 0 DEAD6 "an unreachable IPv6 address fails cleanly and the guest lives on"
else say 1 DEAD6 "a dead IPv6 destination did not fail cleanly"; fi

# ---------------------------------------------------------------------------
# Phase B: IPv6 off at the router. The host still forms a link-local address
# and solicits (that is correct -- it cannot know there is no router), but
# nothing routable may appear, and IPv4 must behave exactly as before.
# ---------------------------------------------------------------------------
LOGB="$TMP/b.log"
boot "$LOGB" "$NET4" \
     "socktest 10.0.2.2 $PORT /probe.bin 4" \
     "echo IP6TEST_DONE"

echo "=== phase B: SLIRP with IPv6 off (the v4-only control) ==="

B4="$(grep -a "SOCKTEST_OK" "$LOGB" | head -1)"
if printf '%s' "$B4" | grep -q "bytes=32768 fnv=$WANT_FNV" && \
   has "$LOGB" "[sock] connected 10.0.2.2 via ipv4"
then say 0 V4ONLY "a v4-only network fetches the identical file over IPv4"
else say 1 V4ONLY "the v4-only path regressed (sock line: ${B4:-none})"; fi

if ! grep -aq "\[ip6\] slaac" "$LOGB" && ! grep -aq "\[ip6\] router" "$LOGB"
then say 0 NOV6 "and configures no routable IPv6 address when no router answers"
else say 1 NOV6 "a routable IPv6 address appeared with no RA on the link"; fi

# The link-local address is still formed and still passes DAD -- IPv6 does not
# depend on a router for that, and asserting it here is what distinguishes
# "correctly found no router" from "the v6 stack did not run at all".
if has "$LOGB" "[ip6] dad ok $LL/64"
then say 0 LLONLY "the link-local address is still formed and DAD-checked"
else say 1 LLONLY "no link-local address on a router-less link"; fi

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: IPv6 DAD, RA, SLAAC, ND and a real fetch on the machine; IPv4 unchanged"
    exit 0
fi
echo "FAIL: $fails of the IPv6 on-device claims did not hold"
echo "----- phase A serial -----"; grep -a "ip6\]\|sock\]\|SOCKTEST" "$LOGA" || cat "$LOGA"
echo "----- phase B serial -----"; grep -a "ip6\]\|sock\]\|SOCKTEST" "$LOGB" || cat "$LOGB"
exit 1
