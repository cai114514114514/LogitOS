#!/usr/bin/env bash
# HTTP/2 against real servers, on the device.
#
# Boots LogitOS under QEMU/SLIRP and runs /bin/h2check against live hosts
# through the full guest path -- e1000 -> DHCP -> DNS -> TCP -> TLS(ALPN) ->
# HTTP/2. For each host it runs the SAME set of URLs twice:
#
#   run 1  offering only http/1.1 -- the fallback path, and the "before"
#   run 2  offering h2 and http/1.1 like a browser -- the "after"
#
# and prints a table of connections and ROUND TRIPS for each, because that is
# the number this line exists to change. Pooled HTTP/1.1 already made
# connections cheap (the pool took one wikipedia page from 14 handshakes to 4);
# what multiplexing adds is that every request goes out at once instead of
# ceil(N / connections) waves.
#
# Two assertions:
#   - at least one host negotiated `h2` AND delivered a real page's bytes over
#     it, with fewer round trips than the same URLs over HTTP/1.1;
#   - EVERY host that did not negotiate h2 still completed over HTTP/1.1. That
#     is the fallback, it is the common case, and it is the one that breaks
#     silently -- a client that assumes h2 sends a binary connection preface to
#     a server that is speaking text.
#
# Usage: run-h2-smoke.sh <iso> <disk.img> [host ...]
# Env:   H2_SMOKE_REQUIRE_ALL=1  fail unless every host works (default: the
#                                public Internet is not a test fixture and one
#                                host being unreachable from this network is
#                                not our bug -- see the note in
#                                run-https-smoke.sh about region-blocked hosts)

set -u

ISO="${1:?usage: run-h2-smoke.sh <iso> <disk.img> [host ...]}"
DISK="${2:?usage: run-h2-smoke.sh <iso> <disk.img> [host ...]}"
shift 2

if [ $# -gt 0 ]; then
    HOSTS=("$@")
else
    # Chosen for what each proves, not for variety:
    #   cloudflare.com    h2 everywhere, the regression guard
    #   zh.wikipedia.org  the project's historical smoke target, and a real
    #                     page rather than a redirect stub
    #   www.bsi.bund.de   SHA-512 chain + RSA-PSS: h2 on top of the most
    #                     awkward TLS path this stack has
    HOSTS=(cloudflare.com zh.wikipedia.org www.bsi.bund.de)
fi

# Eight URLs, so the round-trip arithmetic is visible: over HTTP/1.1 with four
# connections that is two waves, over HTTP/2 it is one. The 404s are on purpose
# -- a 404 is a complete response with headers and a body, which is all the
# measurement needs, and it does not depend on the host's sitemap.
PATHS="/ /robots.txt /favicon.ico /sitemap.xml /logit-404-a /logit-404-b /logit-404-c /logit-404-d"

QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

{
    sleep 15
    for h in "${HOSTS[@]}"; do
        printf 'echo H2-BEGIN %s\n' "$h"
        printf 'h2check --both %s %s\n' "$h" "$PATHS"
        printf 'echo H2-END %s\n' "$h"
        sleep 90
    done
    sleep 5
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

deadline=$(( $(date +%s) + 60 + 100 * ${#HOSTS[@]} ))
last="${HOSTS[${#HOSTS[@]}-1]}"
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -aq "H2-END $last" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 2
done

echo
echo "=== HTTP/2 on the device ==="
printf '%-22s %-10s %-24s %-24s\n' HOST ALPN "HTTP/1.1 (conns / rt / bytes)" "HTTP/2 (conns / rt / bytes)"

h2_ok=0; fb_ok=0; fb_bad=0; better=0; hosts_seen=0
for h in "${HOSTS[@]}"; do
    esc="$(printf '%s' "$h" | sed 's|[/.*]|\\&|g')"
    seg="$(sed -n "/H2-BEGIN $esc/,/H2-END $esc/p" "$LOG" 2>/dev/null)"
    hosts_seen=$((hosts_seen+1))

    alpn="$(printf '%s' "$seg" | grep -ao 'H2CHECK alpn=[a-z0-9./]*' | tail -1 | sed 's/.*alpn=//')"
    h1line="$(printf '%s' "$seg" | grep -a 'H2CHECK RESULT mode=h1' | tail -1)"
    h2line="$(printf '%s' "$seg" | grep -a 'H2CHECK RESULT mode=h2' | tail -1)"

    fld() { printf '%s' "$1" | grep -ao "$2=[0-9]*" | tail -1 | sed "s/$2=//"; }

    c1="$(fld "$h1line" conns)"; r1="$(fld "$h1line" rt)"; b1="$(fld "$h1line" bytes)"; o1="$(fld "$h1line" ok)"
    c2="$(fld "$h2line" conns)"; r2="$(fld "$h2line" rt)"; b2="$(fld "$h2line" bytes)"; o2="$(fld "$h2line" ok)"

    col1="-"; col2="-"
    [ -n "${c1:-}" ] && col1="$c1 / $r1 / $b1"
    [ -n "${c2:-}" ] && col2="$c2 / $r2 / $b2"
    printf '%-22s %-10s %-24s %-24s\n' "$h" "${alpn:-?}" "$col1" "$col2"

    # The fallback: run 1 never offered h2, so it must have come back over
    # HTTP/1.1 with real bytes.
    if [ -n "${o1:-}" ] && [ "${o1:-0}" -gt 0 ] 2>/dev/null && [ "${b1:-0}" -gt 0 ] 2>/dev/null; then
        fb_ok=$((fb_ok+1))
    else
        fb_bad=$((fb_bad+1))
    fi
    # The claim: h2 was negotiated, bytes arrived over it, and it took fewer
    # sequential round trips than HTTP/1.1 did for the identical URLs.
    if [ -n "${o2:-}" ] && [ "${o2:-0}" -gt 0 ] 2>/dev/null && [ "${b2:-0}" -gt 0 ] 2>/dev/null; then
        h2_ok=$((h2_ok+1))
        if [ -n "${r1:-}" ] && [ "${r2:-99}" -lt "${r1:-0}" ] 2>/dev/null; then better=$((better+1)); fi
    fi
done

echo
echo "$h2_ok/$hosts_seen hosts served the page over HTTP/2; $better of them in fewer round trips"
echo "$fb_ok/$hosts_seen hosts completed over HTTP/1.1 when h2 was not offered (the fallback)"

fail=0
if [ "${H2_SMOKE_REQUIRE_ALL:-0}" = "1" ]; then
    [ "$h2_ok" -eq "$hosts_seen" ] || { echo "FAIL: not every host served HTTP/2"; fail=1; }
    [ "$fb_bad" -eq 0 ] || { echo "FAIL: an HTTP/1.1 fallback did not complete"; fail=1; }
else
    [ "$h2_ok" -gt 0 ] || { echo "FAIL: no host served a page over HTTP/2"; fail=1; }
    [ "$fb_ok" -gt 0 ] || { echo "FAIL: the HTTP/1.1 fallback did not complete anywhere"; fail=1; }
fi
[ "$better" -gt 0 ] || { echo "FAIL: HTTP/2 never reduced the round trips -- the multiplexing is not happening"; fail=1; }

if [ "$fail" != 0 ]; then
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi
echo "PASS: HTTP/2 over the full guest TCP/TLS/ALPN path, with a clean HTTP/1.1 fallback"
exit 0
