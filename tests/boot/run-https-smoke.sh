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
# PHASE 2 IS ABOUT THE OTHER HALF: WHAT A FAILURE SAYS.
#
# Everything above asks whether a site opens. Nothing above could ask what the
# machine tells a person when one does not -- and that is a different client.
# `net get` is SYS_HTTP_GET, the KERNEL's blocking fetcher (c/apps/logit.h:151);
# the Browser is `bfetch` in ring 3 over the non-blocking socket ABI, and the
# two share no code below the URL. So phase 2 boots the machine again, drives
# the real Browser over QMP, and reads the sentence off the serial console.
#
# It asks for TWO deliberately broken hosts, not one, for the reason stated at
# the top of this file: a table of one URL cannot show a distinction. One is
# refused by the certificate chain, one does not resolve, and the assertion is
# that they produce DIFFERENT sentences and that each names its own cause. A
# build that folds every transport failure into one string passes any test that
# only asks "did it fail" -- which is what this file used to be able to ask.
#
# Usage: run-https-smoke.sh <iso> <disk.img> [url ...]
# Env:   HTTPS_SMOKE_REQUIRE_ALL=1  fail unless EVERY url succeeds (default: the
#                                   run passes if at least one does, because the
#                                   public internet is not a test fixture and a
#                                   single site being down is not our bug).
#        HTTPS_SMOKE_PHASE=net|browser|both   which half to run (default both).
#                                   `browser` skips the reachability table so the
#                                   failure-message phase can be iterated on in
#                                   one boot instead of two.
#        HTTPS_SMOKE_BAD_TLS=<url>  the host whose chain must be refused
#        HTTPS_SMOKE_BAD_CONN=<url> the endpoint nothing is listening on

set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

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
    # and two the user hit by hand and reported as RC-5, each proving a
    # different half of what landed today:
    #   bing.com           refuses x25519 and answers with a HelloRetryRequest
    #                      naming secp256r1. The old client sent x25519 alone
    #                      and rejected HRR outright, so this was not slow, it
    #                      was shut. Nothing else in this list exercises HRR.
    #   www.baidu.com      needs BOTH halves at once: it does not speak 1.3 at
    #                      all AND wants a NIST curve -- TLS 1.2 with
    #                      ECDHE-RSA-AES128-GCM-SHA256 over secp256r1. It is
    #                      also the only site here whose content is fully
    #                      server-rendered (~250 K characters of text), which
    #                      makes it the one that should actually LOOK like a
    #                      page rather than an empty SPA shell.
    URLS=(https://cloudflare.com/ https://zh.wikipedia.org/ https://www.bsi.bund.de/
          https://sectigo.com/ https://www.mas.gov.sg/ https://www.cbuae.gov.ae/
          https://bing.com/ https://www.baidu.com/)
fi

QEMU="${QEMU:-qemu-system-x86_64}"
PHASE="${HTTPS_SMOKE_PHASE:-both}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---- PHASE 1: reachability, through the kernel's blocking client -----------
# Not indented, because the body below is unchanged and an indentation-only
# diff over 60 lines hides the one line that is not.
if [ "$PHASE" = "browser" ]; then
echo "(reachability table skipped: HTTPS_SMOKE_PHASE=browser)"
else

# Each fetch is bracketed by an echo marker so the per-site handshake lines can
# be sliced back out of one continuous serial log.
{
    logit_wait_for_shell "$LOG" 180
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

# Phase 1's machine is done with. Take it down before phase 2 boots its own --
# two QEMUs with -smp 4 on one host make phase 2's timings meaningless.
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null
QPID=""

fi   # end PHASE 1

# ---- PHASE 2: what the Browser SAYS when https fails -----------------------
if [ "$PHASE" != "net" ]; then
echo
echo "=== the sentence a person is shown when https fails ==="

BAD_TLS="${HTTPS_SMOKE_BAD_TLS:-https://untrusted-root.badssl.com/}"
BAD_CONN="${HTTPS_SMOKE_BAD_CONN:-https://127.0.0.1:1/}"

# Inline rather than a file in tests/qmp/ on purpose: this is the gate for one
# change, it is 90 lines, and keeping it here means the whole assertion is
# readable in the file whose name the failure will be reported under. If it
# grows a second question to ask, it should become tests/qmp/qmp_browser_why.py
# and this block should call it.
python3 - "$ISO" "$DISK" "$(dirname "$0")/../qmp" "$BAD_TLS" "$BAD_CONN" <<'PY'
import os
import subprocess
import sys
import tempfile
import time

iso, disk, qmpdir, bad_tls, bad_conn = sys.argv[1:6]
sys.path.insert(0, os.path.abspath(qmpdir))
from qmp_ui import Session, dock_icon, BROWSER_SLOT          # noqa: E402

# The two hosts and the sentence each MUST produce. The strings are quoted here,
# not imported from include/abi/sockerr.h: a test that reads the string under
# test asserts that a constant equals itself and passes whatever it becomes.
#
#   untrusted-root.badssl.com  a real server with a real handshake whose chain
#                              ends at a root we do not hold. It reaches
#                              c/net/core/sock.c:474 -> SOCK_E_TLS. badssl.com
#                              is maintained to be exactly this; a self-signed
#                              or expired host of your own works via
#                              HTTPS_SMOKE_BAD_TLS.
#   127.0.0.1:1                nothing listens there, on any machine, so this
#                              half of the assertion needs no Internet at all
#                              and cannot be broken by somebody's resolver.
#                              sock.c:419-442 -> SOCK_E_CONN.
#
# AND NOT AN UNRESOLVABLE NAME, which is what this pair was first written with.
# `https://no-such-host.invalid/` is reserved by RFC 2606 and cannot be
# registered -- and it RESOLVED, measured on this machine, to 198.18.0.92, a
# synthetic address out of the benchmarking range. The network answers for every
# name, so SOCK_E_DNS is not reachable here and the run reported our decode as
# broken when the resolver was. This file already documents the same
# interference for www.cbuae.gov.ae. A DNS-failure case belongs in a test with
# its own resolver, not in one that borrows the operator's.
WANT = [(bad_tls, "TLS refused: handshake or certificate verification failed"),
        (bad_conn, "the connection was refused or the network is down")]

# What the build before this change said, for all four SOCK_E_* alike. Named so
# a failure reads "the codes are being folded again" rather than "a string moved".
GENERIC = ("socket error", "connect failed", "sock_open failed",
           "could not fetch the page")

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
tmp = tempfile.mkdtemp(prefix="https_smoke_why_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

rc = 1
reasons = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ctrl(ui, qcode):
    """Ctrl+<key>. qmp_ui has key_shift and no ctrl form; this is that, in the
    same shape, rather than a second copy of qmp_ui."""
    down = [{"type": "key", "data": {"key": {"type": "qcode", "data": "ctrl"}, "down": True}},
            {"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}]
    up = [{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}},
          {"type": "key", "data": {"key": {"type": "qcode", "data": "ctrl"}, "down": False}}]
    ui._input(down)
    ui._input(up)
    time.sleep(0.4)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    die("never saw %s (waited %ds)" % (what, secs))


try:
    wait_serial("LOGIT_BOOT_OK", 240, "the kernel to boot")
    wait_serial("desktop live", 90, "the desktop")
    time.sleep(3)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):
        if "launched Browser" in serial():
            break
        time.sleep(4)
        ui.click_at(*dock_icon(BROWSER_SLOT))
    else:
        die("the Dock never launched the Browser")
    time.sleep(8)              # ~3 MB .aex off virtio-blk, ELF load, first paint

    for url, want in WANT:
        mark = len(serial())
        # CTRL+T, NOT A CLICK ON THE ADDRESS BAR, and this cost a run to find.
        # browser.c:2818 starts the app with `editing = 1`, so the FIRST url
        # typed after launch navigates whether or not anything focused the bar.
        # browser.c:3092 clears it on Enter, and only browser.c:3131 -- a click
        # with a window-relative y ABOVE VIEW_Y (TABH+BARH, ~64 pt) -- sets it
        # again. (420,145) in SCREEN coordinates is ~85 pt down the window, i.e.
        # in the viewport, so it never focused anything; the second url of a run
        # was typed into a browser that was not listening and produced not one
        # line of guest output. Ctrl+T (browser.c:2859-2864) clears the url AND
        # sets editing, in one keystroke that depends on no geometry at all.
        ctrl(ui, "t")
        ui.typ(url)
        time.sleep(0.5)
        ui.key("ret")
        # T_RESOLVE is 4 s and T_TLS is 20 s (c/net/core/sock.c:38,41), so both
        # of these fail fast; the budget is for the emulator, not the protocol.
        end = time.time() + 90
        line = None
        while time.time() < end:
            s = serial()[mark:]
            hit = [l for l in s.splitlines() if "[browser] page fetch failed:" in l]
            if hit:
                line = hit[-1]
                break
            if "[browser] load done:" in s:
                die("%s LOADED. It is supposed to be refused -- either the "
                    "network is answering for it (a captive portal or an "
                    "intercepting resolver) or the trust check did not run." % url)
            if proc.poll() is not None:
                die("QEMU exited while loading " + url)
            time.sleep(0.5)
        if line is None:
            # "no verdict" has two very different causes -- the navigation never
            # happened, or it happened and hung -- and the compositor's per-second
            # perf line drowns both out of a plain tail. Filter it, and photograph
            # the window: an address bar still holding the PREVIOUS url is the
            # first answer, and it is not visible in any log.
            shot = os.path.join(tmp, "noverdict.ppm")
            try:
                ui.screendump(shot)
            except Exception:                                 # noqa: BLE001
                shot = "(screendump failed)"
            quiet = [l for l in serial()[mark:].splitlines()
                     if "[wm] perf" not in l and "[mm] " not in l]
            print("----- guest output since the URL was typed -----")
            print("\n".join(quiet[-60:]))
            print("----- screenshot: %s -----" % shot)
            die("%s produced no verdict at all within 90s" % url)
        why = line.split("page fetch failed:", 1)[1].strip()
        if why.endswith(")") and " (" in why:
            why = why.rsplit(" (", 1)[0]
        reasons.append((url, why, want))
        print("      %-40s -> %s" % (url, why))

    ok = True
    for url, why, want in reasons:
        good = why == want
        print(("ok:   " if good else "FAIL: ") +
              "%s names its own cause (%r)" % (url, why))
        ok = ok and good
        for g in GENERIC:
            if g in why:
                print("FAIL: %s still answers with the generic %r" % (url, g))
                ok = False
    # The point of using two hosts. Equal strings here means the codes are being
    # folded again, and every per-host assertion above could still be made to
    # pass by one sentence that happened to match.
    if reasons[0][1] == reasons[1][1]:
        print("FAIL: a refused certificate and a dead endpoint produced the SAME "
              "sentence -- the distinction is gone")
        ok = False
    else:
        print("ok:   the two failures produce two different sentences")
    rc = 0 if ok else 1
finally:
    proc.kill()

print("\n%s: the Browser names why an https page failed" % ("PASS" if rc == 0 else "FAIL"))
print("      artefacts in %s" % tmp)
sys.exit(rc)
PY
brc=$?
[ "$brc" -eq 0 ] || exit 1

fi   # end PHASE 2

exit 0
