#!/bin/sh
# SSH_ASKPASS helper for a non-interactive password-auth test. OpenSSH
# invokes this instead of a tty prompt when SSH_ASKPASS_REQUIRE=force is set
# (8.4+), so no setsid/DISPLAY trick is needed. The password is a throwaway
# test account's, created fresh by this same test run -- see run-ssh-test.sh.
echo "$SSHTEST_PW"
