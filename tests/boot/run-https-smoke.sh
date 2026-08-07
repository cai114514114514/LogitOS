#!/usr/bin/env bash
# Real-Internet HTTPS smoke test: boot Logit under QEMU/SLIRP and fetch live
# sites through the full guest path -- e1000 -> DHCP -> DNS -> TCP -> TLS
# -> HTTP -- asserting the `net get` marker line with a nonzero byte count.
# Requires outbound Internet from the host; TCG timing is relaxed.
#
# It takes a LIST of URLs now, not one, and prints a per-site table with the
# handshake each one actually negotiated. That is the point: the interesting
# claim about this layer is not "TLS works" but "these specific sites, which
# could not be opened, now open" -- and a table of one URL cannot show that.
#
# Usage: run-https-smoke.sh <iso> <disk.img> [url ...]
# Env:   HTTPS_SMOKE_REQUIRE_ALL=1  fail unless EVERY url succeeds (default: the
#                                   run passes if at least one does, because the
#                                   public internet is not a test fixture and a
#                                   single site being down is not our bug).

set -u

ISO="${1:?usage: run-https-smoke.sh <iso> <disk.img> [url ...]}"
DISK="${2:?usage: run-https-smoke.sh <iso> <disk.img> [url ...]}"
shift 2

if [ $# -gt 0 ]; then
    URLS=("$@")
else
    # The default set is chosen for what each one PROVES, not for variety:
    #   cloudflare.org     x25519, the common path -- the regression guard
    #   zh.wikipedia.org   the historical smoke target (RSA leaf, big page)
    #   bsi.bund.de        SHA-512 chain + RSA-PSS CertificateVerify
    # and, TLS 1.2-only -- each of these answered a TLS 1.3 ClientHello with
    # `alert 70 protocol_version` and was simply unreachable before 1.2 landed:
    #   sectigo.com        ECDHE-RSA-CHACHA20-POLY1305. Worth watching: it was
    #                      recorded as fixed in M26 (TCP reassembly), then went
    #                      1.2-only and nobody noticed, because the smoke test
    #                      used to print one total instead of a per-site line.
    #   www.mas.gov.sg     ECDHE-ECDSA-AES256-GCM-SHA384 -- the SHA-384 PRF and
    #                      transcript, i.e. the branch AES-256 exists for.
    #   www.cbuae.gov.ae   the third probed 1.2-only host. NOTE: on a network
    #                      that intercepts DNS this one answers `alert 112
    #                      unrecognized_name` -- the same answer `openssl
    #                      s_client` gets from the same host, i.e. the SNI is
    #                      reaching something that is not the real server. That
    #                      is the environment, not the TLS stack; the run still
    #                      passes because the default is "at least one".
    #
    # www.google.com was in this list and has been removed. It is unreachable
    # from entire regions, and this test is run by people in them -- so it did
    # not report "the network is restricted here", it reported OUR TLS as
    # broken, and took the blocking fetch's full timeout chain to do it. A test
    # whose result depends on the operator's location is testing the operator.
    # Pass URLs as arguments to check a specific site.
    URLS=(https://cloudflare.com/ https://zh.wikipedia.org/ https://www.bsi.bund.de/
          https://sectigo.com/ https://www.mas.gov.sg/ https://www.cbuae.gov.ae/)
fi

QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# Each fetch is bracketed by an echo marker so the per-site handshake lines can
# be sliced back out of one continuous serial log.
{
    sleep 15
    for u in "${URLS[@]}"; do
        printf 'echo SMOKE-BEGIN %s\n' "$u"
        printf 'net get %s\n' "$u"
        printf 'echo SMOKE-END %s\n' "$u"
        sleep 25
    done
    sleep 5
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the last marker, or for QEMU to die, or for the overall budget.
deadline=$(( $(date +%s) + 60 + 30 * ${#URLS[@]} ))
last="${URLS[${#URLS[@]}-1]}"
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -aq "SMOKE-END $last" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done

echo
echo "=== HTTPS reachability ==="
ok=0; bad=0; n12=0; n13=0
for u in "${URLS[@]}"; do
    seg="$(sed -n "/SMOKE-BEGIN $(printf '%s' "$u" | sed 's|[/.*]|\\&|g')/,/SMOKE-END/p" "$LOG" 2>/dev/null)"
    bytes="$(printf '%s' "$seg" | grep -ao 'http bytes [0-9]*' | tail -1 | awk '{print $3}')"
    hs="$(printf '%s' "$seg" | grep -ao '\[tls\] \(ServerHello\|HelloRetryRequest\|ServerKeyExchange\|chain of\).*' | tr '\n' '|')"
    # The negotiated version is the claim this test now exists to make, so pull
    # it out into its own column instead of leaving it buried in the log line.
    # head, not tail: a fetch that follows a redirect handshakes more than once,
    # and the version that matters is the one the REQUESTED host negotiated --
    # sectigo.com is 1.2 and redirects to a www host that is 1.3, so taking the
    # last line would report exactly the opposite of the interesting fact.
    ver="$(printf '%s' "$seg" | grep -ao 'ServerHello: TLS 1\.[23]' | head -1 | grep -ao '1\.[23]')"
    if [ -n "${bytes:-}" ] && [ "${bytes:-0}" -gt 0 ] 2>/dev/null; then
        printf 'PASS  %-28s TLS%-4s %8s bytes   %s\n' "$u" "${ver:-?}" "$bytes" "$hs"
        ok=$((ok+1))
        [ "$ver" = "1.2" ] && n12=$((n12+1))
        [ "$ver" = "1.3" ] && n13=$((n13+1))
    else
        why="$(printf '%s' "$seg" | grep -ao '\[tls\].*\|net: fetch failed.*' | tr '\n' '|')"
        printf 'FAIL  %-28s %-7s %8s          %s\n' "$u" "-" "-" "${why:-no output}"
        bad=$((bad+1))
    fi
done
echo "$ok reachable, $bad not  ($n13 over TLS 1.3, $n12 over TLS 1.2)"

if [ "${HTTPS_SMOKE_REQUIRE_ALL:-0}" = "1" ]; then
    [ "$bad" -eq 0 ] || { echo "FAIL: not every url was reachable"; exit 1; }
else
    [ "$ok" -gt 0 ] || {
        echo "FAIL: no successful 'http bytes' marker from any live HTTPS fetch"
        echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
        exit 1; }
fi
echo "PASS: live HTTPS fetch over the full guest TCP/TLS path"
exit 0
