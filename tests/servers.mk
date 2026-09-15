# Normal SSH/HTTP interoperability, using the shipped daemons and temporary
# accounts on a private disk. No fixture-only daemon or global known_hosts.
$(BUILD)/server_semantics_test: tests/unit/server_semantics_test.c c/apps/coreutils/httpd_protocol.h c/net/ssh/ssh_conn.c c/net/ssh/ssh_wire.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wno-sign-compare -Ic/apps/coreutils -Ic/net/ssh $< c/net/ssh/ssh_conn.c c/net/ssh/ssh_wire.c -o $@

# The control removes Range support from a PRIVATE copy of the same header;
# the ordinary bounded-range check must visibly fail. Product source stays put.
$(BUILD)/server_semantics_neg: tests/unit/server_semantics_test.c c/apps/coreutils/httpd_protocol.h c/net/ssh/ssh_conn.c c/net/ssh/ssh_wire.c
	@mkdir -p $(BUILD)/server-neg
	python3 -c 'from pathlib import Path; p=Path("c/apps/coreutils/httpd_protocol.h"); s=p.read_text().replace("*first = 0; *count = size;", "*first = 0; *count = size; return 0;"); Path("$(BUILD)/server-neg/httpd_protocol.h").write_text(s)'
	$(CC) -O2 -Wall -Wextra -Wno-sign-compare -I$(BUILD)/server-neg -Ic/net/ssh $< c/net/ssh/ssh_conn.c c/net/ssh/ssh_wire.c -o $@

test-server-semantics-neg: $(BUILD)/server_semantics_neg
	@$(BUILD)/server_semantics_neg > $(BUILD)/server-neg/result.log 2>&1; code=$$?; cat $(BUILD)/server-neg/result.log; test $$code -ne 0 && rg -q 'FAIL bounded range' $(BUILD)/server-neg/result.log

test-server-semantics: test-server-semantics-neg $(BUILD)/server_semantics_test
	@$(BUILD)/server_semantics_test

$(DISK): $(wildcard fsroot/www/*) examples/browser/signed-report.html

test-servers-os: $(ISO) $(DISK)
	python3 tests/boot/run-servers.py --build $(BUILD)

.PHONY: test-server-semantics test-server-semantics-neg test-servers-os
ci-host: test-server-semantics
ci-boot: test-servers-os

# sftpd uses the same account-scoped descriptor ABI as ordinary coreutils.
$(eval $(call CLI_RULE,sftpd))
$(DISK): $(BUILD)/sftpd.aex
ROOT_AEX_PACK += $(BUILD)/sftpd.aex:/bin/sftpd

$(BUILD)/sftpd_host: c/apps/coreutils/sftpd.c tests/unit/sftpd_host.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DSFTPD_HOST -Itests/unit -Iinclude/abi $< -o $@
$(BUILD)/sftpd_host_neg: c/apps/coreutils/sftpd.c tests/unit/sftpd_host.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -DSFTPD_HOST -DSFTPD_DISABLE_WRITE -Itests/unit -Iinclude/abi $< -o $@
test-sftpd-neg: $(BUILD)/sftpd_host_neg
	@python3 tests/unit/sftpd_test.py $< > $(BUILD)/sftp-neg.log 2>&1; rc=$$?; tail -3 $(BUILD)/sftp-neg.log; test $$rc -ne 0 && rg -q 'SFTP batch failed' $(BUILD)/sftp-neg.log
test-sftpd: test-sftpd-neg $(BUILD)/sftpd_host
	python3 tests/unit/sftpd_test.py $(BUILD)/sftpd_host
ci-host: test-sftpd
.PHONY: test-sftpd test-sftpd-neg

$(BUILD)/sftpd_links_neg: c/apps/coreutils/sftpd.c tests/unit/sftpd_host.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -DSFTPD_HOST -DSFTPD_DISABLE_LINKS -Itests/unit -Iinclude/abi $< -o $@
test-sftpd-links-neg: $(BUILD)/sftpd_links_neg
	@python3 tests/unit/sftpd_test.py $< > $(BUILD)/sftp-links-neg.log 2>&1; rc=$$?; tail -3 $(BUILD)/sftp-links-neg.log; test $$rc -ne 0 && rg -q 'AssertionError: SFTP links failed' $(BUILD)/sftp-links-neg.log
test-sftpd: test-sftpd-links-neg
test-servers-os: test-sftpd
.PHONY: test-sftpd-links-neg

# The existing TLS engine is a real userspace consumer here. Only its
# nonblocking record/crypto code is linked; TCP adapters use process fds.
HTTPS_SRC := c/apps/coreutils/httpd.c c/apps/coreutils/https_platform.c \
             c/net/tls/tls.c c/net/tls/tls_server.c c/net/tls/x509.c \
             $(filter-out c/crypto/aead/aes_ni.c,$(wildcard c/crypto/aead/*.c)) \
             $(wildcard c/crypto/hash/*.c c/crypto/pubkey/*.c c/crypto/kdf/*.c c/crypto/pq/*.c) \
             c/apps/libc/src/string.c
HTTPS_OBJ := $(patsubst %.c,$(BUILD)/httpsobj/%.o,$(HTTPS_SRC))
$(BUILD)/httpsobj/%.o: %.c c/apps/coreutils/https_transport.h c/net/tls/tls_domain.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(INCDIRS) -DHTTPD_TLS -DLOGIT_TLS_SINGLE_PROCESS -DKPROF_DISABLE -ffunction-sections -fdata-sections -c $< -o $@
$(BUILD)/httpsd.elf: $(HTTPS_OBJ) c/apps/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/httpsd.crt.o
	$(LD) --gc-sections -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/httpsd.crt.o $(HTTPS_OBJ)
$(BUILD)/httpsd.aex: $(BUILD)/httpsd.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ httpsd - '*' 150 150 150
$(DISK): $(BUILD)/httpsd.aex
ROOT_AEX_PACK += $(BUILD)/httpsd.aex:/bin/httpsd

$(BUILD)/pty_probe.elf: tests/boot/pty_probe.c $(LIBC_OBJS) c/apps/libc/src/pty_ctl.h include/abi/pty.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/pty_probe.crt.o
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/pty_probe.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/pty_probe.crt.o $(BUILD)/apps/pty_probe.o $(LIBC_OBJS)
test-servers-os: $(BUILD)/pty_probe.elf
$(BUILD)/inet_probe.elf: tests/boot/inet_probe.c $(LIBC_OBJS)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/inet_probe.crt.o
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/inet_probe.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/inet_probe.crt.o $(BUILD)/apps/inet_probe.o $(LIBC_OBJS)
test-servers-os: $(BUILD)/inet_probe.elf
$(BUILD)/sshdobj/c/apps/coreutils/sshd.o: include/abi/pty.h
$(BUILD)/sshdobj/c/apps/coreutils/sshd.o: c/apps/coreutils/sshd_channels.h c/apps/coreutils/sshd_channel_state.h
$(BUILD)/asobj/c/apps/libc/src/termios.o $(BUILD)/asobj/c/apps/libc/src/uio.o: c/apps/libc/src/pty_ctl.h include/abi/pty.h

$(BUILD)/login.elf: c/apps/coreutils/boot_services.h

$(BUILD)/server_probe.elf: tests/boot/server_probe.c c/apps/logit.h c/apps/clib.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/server_probe.crt.o
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/server_probe.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/server_probe.crt.o $(BUILD)/apps/server_probe.o

test-servers-os: $(BUILD)/server_probe.elf

$(BUILD)/ssh_hold.elf: tests/boot/ssh_hold.c c/apps/clib.h c/apps/logit.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/ssh_hold.crt.o
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/ssh_hold.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/ssh_hold.crt.o $(BUILD)/apps/ssh_hold.o
test-servers-os: $(BUILD)/ssh_hold.elf
$(BUILD)/ssh_service.elf: tests/boot/ssh_service.c c/apps/clib.h c/apps/logit.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/apps/ssh_service.crt.o
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/ssh_service.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/ssh_service.crt.o $(BUILD)/apps/ssh_service.o
test-servers-os test-ssh-multiplex-neg: $(BUILD)/ssh_service.elf

# A state-only ownership control; no generated network fault traffic. It
# removes the accepted descriptor's pin in the host model and must fail.
test-tcp-accepted-lifetime-neg:
	@mkdir -p $(BUILD)
	$(CC) -O2 -w -DTCP_ACCEPT_LIFETIME_NEGCTL -o $(BUILD)/tcp_accepted_neg tests/unit/tcp_test.c -Itests/unit/tcpstub -Ic/net/transport
	@$(BUILD)/tcp_accepted_neg > $(BUILD)/tcp-accepted-neg.log 2>&1; code=$$?; rg 'accepted lifetime|TCP protocol tests' $(BUILD)/tcp-accepted-neg.log; test $$code -ne 0 && rg -q 'FAIL.*accepted lifetime: closed transport' $(BUILD)/tcp-accepted-neg.log

test-tcp-host: test-tcp-accepted-lifetime-neg
.PHONY: test-tcp-accepted-lifetime-neg

test-server-profile-neg: $(BUILD)/lfs_snapshot
	@python3 tests/unit/server_profile_test.py --helper $(BUILD)/lfs_snapshot --negative > $(BUILD)/server-profile-neg.log 2>&1; code=$$?; tail -2 $(BUILD)/server-profile-neg.log; test $$code -ne 0 && rg -q 'AssertionError: administrator configuration retained' $(BUILD)/server-profile-neg.log

test-server-profile: test-server-profile-neg
	python3 tests/unit/server_profile_test.py --helper $(BUILD)/lfs_snapshot
ci-host: test-server-profile
.PHONY: test-server-profile test-server-profile-neg

# Keep the single-channel policy only in a private control binary. The same
# ordinary OpenSSH multiplex check must reject it before the product test runs.
$(BUILD)/sshd-one-channel.o: c/apps/coreutils/sshd.c c/apps/coreutils/sshd_channels.h c/apps/coreutils/sshd_channel_state.h $(BUILD)/sshd.elf
	$(CC) $(UCFLAGS) -DSSHD_CHANNELS=1 -Ic/apps/libc/include/uonly $(INCDIRS) -c $< -o $@
$(BUILD)/sshd-one-channel.elf: $(BUILD)/sshd-one-channel.o $(BUILD)/sshd.elf
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/sshd.crt0c.o $< $(filter-out $(BUILD)/sshdobj/c/apps/coreutils/sshd.o,$(SSHD_OBJS)) $(BUILD)/sshdobj/sshd_thread.o
test-ssh-multiplex-neg: $(ISO) $(DISK) $(BUILD)/sshd-one-channel.elf $(BUILD)/ssh_hold.elf $(BUILD)/server_probe.elf $(BUILD)/inet_probe.elf $(BUILD)/pty_probe.elf
	@python3 tests/boot/run-servers.py --build $(BUILD) --one-channel-control $(BUILD)/sshd-one-channel.elf > $(BUILD)/multiplex-neg.log 2>&1; rc=$$?; tail -8 $(BUILD)/multiplex-neg.log; test $$rc -ne 0 && rg -q '^FAIL multiple commands share one SSH connection while one stdin is paused' $(BUILD)/multiplex-neg.log
test-servers-os: test-ssh-multiplex-neg test-tcp-half-window
.PHONY: test-ssh-multiplex-neg

$(BUILD)/tcp_half_window: tests/unit/tcp_half_window_test.c tests/unit/tcp_test.c c/net/transport/tcp.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -w -Itests/unit/tcpstub -Ic/net/transport $< -o $@
$(BUILD)/tcp_half_window_neg: tests/unit/tcp_half_window_test.c tests/unit/tcp_test.c c/net/transport/tcp.c
	@mkdir -p $(BUILD)/tcp-half-neg
	python3 -c 'from pathlib import Path; s=Path("c/net/transport/tcp.c").read_text(); old="(c->state == ESTABLISHED || c->state == FIN_WAIT) &&"; assert s.count(old)==1; Path("$(BUILD)/tcp-half-neg/tcp.c").write_text(s.replace(old,"(c->state == ESTABLISHED) &&"))'
	$(CC) -O2 -w -Itests/unit/tcpstub -I$(BUILD)/tcp-half-neg -Ic/net/transport $< -o $@
test-tcp-half-window-neg: $(BUILD)/tcp_half_window_neg
	@$(BUILD)/tcp_half_window_neg > $(BUILD)/tcp-half-neg.log 2>&1; rc=$$?; cat $(BUILD)/tcp-half-neg.log; test $$rc -ne 0 && rg -q '^FAIL half-close drain advertises receive window' $(BUILD)/tcp-half-neg.log
test-tcp-half-window: test-tcp-half-window-neg $(BUILD)/tcp_half_window
	$(BUILD)/tcp_half_window
ci-host: test-tcp-half-window
.PHONY: test-tcp-half-window test-tcp-half-window-neg

$(BUILD)/tcp_readiness: tests/unit/tcp_readiness_test.c tests/unit/tcp_test.c c/net/transport/tcp.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -w -Itests/unit/tcpstub -Ic/net/transport $< -o $@
$(BUILD)/tcp_readiness_neg: tests/unit/tcp_readiness_test.c tests/unit/tcp_test.c c/net/transport/tcp.c
	@mkdir -p $(BUILD)/tcp-poll-neg
	python3 -c 'from pathlib import Path; s=Path("c/net/transport/tcp.c").read_text(); old="short tcp_file_poll(int id,int listener,struct poll_table *pt)\n{"; assert s.count(old)==1; s=s.replace(old,old+"\nreturn LPOLLNVAL;"); s=s.replace("c->state == CLOSED && !c->app_owned", "c->state == CLOSED"); Path("$(BUILD)/tcp-poll-neg/tcp.c").write_text(s)'
	$(CC) -O2 -w -Itests/unit/tcpstub -I$(BUILD)/tcp-poll-neg -Ic/net/transport $< -o $@
test-tcp-readiness-neg: $(BUILD)/tcp_readiness_neg
	@$(BUILD)/tcp_readiness_neg > $(BUILD)/tcp-poll-neg.log 2>&1; rc=$$?; cat $(BUILD)/tcp-poll-neg.log; test $$rc -ne 0 && rg -q '^FAIL: TCP poll idle connection' $(BUILD)/tcp-poll-neg.log && rg -q '^FAIL: TCP EOF retains' $(BUILD)/tcp-poll-neg.log
test-tcp-readiness: test-tcp-readiness-neg $(BUILD)/tcp_readiness
	$(BUILD)/tcp_readiness
test-servers-os: test-tcp-readiness
ci-host: test-tcp-readiness
.PHONY: test-tcp-readiness test-tcp-readiness-neg
