#!/usr/bin/env bash
# The M27 shell suite, host-side.
#
# tests/boot/run-shell-as-test.sh is the real gate -- it runs on LogitOS, against
# LogitOS's own coreutils. This runs the SAME command sequence through the same
# fsroot/as/examples/ash.as on the build host, where it costs two seconds instead
# of three minutes. Everything the shell itself is responsible for (tokenising,
# the pipe/redirect grammar, folding stages with `|>`, `<-`/`->`, exit status,
# reading its own stdin as a port) is exercised here; only the guest kernel is
# not. A parse or pipeline bug shows up here long before QEMU boots.
#
# The commands are the ones in tests/boot/run-shell-test.sh, rewritten only where
# a path has to be one the host also has.
set -u
ASC="${1:?usage: run-ash-shell.sh <asc>}"
ROOT="$PWD"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/docs"
printf 'readme line one\nreadme line two\nreadme line three\n' > "$TMP/docs/readme.txt"

{
  echo "uname"
  echo "echo hello-logit-shell"
  echo "ls /bin | wc"
  echo "cat $TMP/docs/readme.txt | wc"
  echo "mkdir $TMP/cptest"
  echo "echo cpmvprobe > $TMP/cptest/a.txt"
  echo "cp $TMP/cptest/a.txt $TMP/cptest/b.txt"
  echo "cat $TMP/cptest/b.txt"
  echo "mv $TMP/cptest/b.txt $TMP/cptest/c.txt"
  echo "ls $TMP/cptest"
  echo "rm $TMP/cptest/a.txt"
  echo "rm $TMP/cptest/c.txt"
  echo "exit"
} | "$ROOT/$ASC" "$ROOT/fsroot/as/examples/ash.as" > "$TMP/out" 2>&1

fails=0
check() {   # check <name> <pattern>
    if grep -aq -- "$2" "$TMP/out"; then printf 'PASS %s\n' "$1"
    else printf 'FAIL %s (no /%s/ in output)\n' "$1" "$2"; fails=$((fails+1)); fi
}
absent() {
    if grep -aq -- "$2" "$TMP/out"; then printf 'FAIL %s (unexpected /%s/)\n' "$1" "$2"; fails=$((fails+1))
    else printf 'PASS %s\n' "$1"; fi
}

check banner        "ash: LogitOS AetherScript shell"
check echo_builtin  "hello-logit-shell"
check pipe_ls_wc    "^ash\$ *[0-9]"          # `ls /bin | wc` produced counts
check redirect_cat  "cpmvprobe"              # `echo > file` then cp then cat read it back
check mv_listing    "c.txt"
check exit_clean    "ash: bye"
absent no_errors    "^ash: [a-z].*error"
# The file `>` actually wrote must be gone after the rm's -- proves the shell ran
# them rather than printing plausible output.
if [ -e "$TMP/cptest/a.txt" ] || [ -e "$TMP/cptest/c.txt" ]; then
    echo "FAIL rm_effective (files still present)"; fails=$((fails+1))
else
    echo "PASS rm_effective"
fi

if [ "$fails" -ne 0 ]; then
    echo "----- ash output -----"; cat "$TMP/out"; echo "----------------------"
fi
echo "ash-shell: $fails failed"
[ "$fails" -eq 0 ]
