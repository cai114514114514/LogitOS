#ifndef LOGIT_SSH_CONN_H
#define LOGIT_SSH_CONN_H
#include <stdint.h>

/* RFC 4254 connection protocol: message parse/build only. The actual
 * fork+pipe+execve and the data pump between the child's pipes and the
 * channel are sshd.c's job (that is OS process/IO plumbing, not protocol) --
 * this file knows nothing about /bin/sh.
 *
 * ONE session channel per connection (this server does not implement
 * multiple channels on one connection -- OpenSSH's interactive client and
 * `ssh host cmd` both open exactly one, and a second CHANNEL_OPEN is refused
 * with SSH_OPEN_ADMINISTRATIVELY_PROHIBITED rather than silently ignored). */

int ssh_parse_channel_open(const uint8_t *payload, int len,
                           char *type, int typemax,
                           uint32_t *peer_chan, uint32_t *peer_win, uint32_t *peer_maxpkt);

int ssh_build_channel_open_confirmation(uint32_t recipient_chan, uint32_t sender_chan,
                                        uint32_t init_win, uint32_t max_pkt,
                                        uint8_t *out, int outmax);
int ssh_build_channel_open_failure(uint32_t recipient_chan, uint32_t reason_code,
                                   uint8_t *out, int outmax);

int ssh_parse_channel_request(const uint8_t *payload, int len,
                              uint32_t *chan, char *type, int typemax,
                              int *want_reply, const uint8_t **data, int *datalen);
int ssh_build_channel_success(uint32_t chan, uint8_t *out, int outmax);
int ssh_build_channel_failure(uint32_t chan, uint8_t *out, int outmax);

/* "exec" request data is one string (the command); "pty-req" and other
 * types are recorded/ignored by sshd.c without a dedicated parser here --
 * one string-extraction covers exec, which is the one whose content sshd.c
 * actually needs. */
int ssh_parse_exec_command(const uint8_t *data, int datalen, char *cmd, int cmdmax);

int ssh_build_channel_data(uint32_t chan, const uint8_t *data, int datalen, uint8_t *out, int outmax);
int ssh_parse_channel_data(const uint8_t *payload, int len, uint32_t *chan,
                           const uint8_t **data, int *datalen);

int ssh_build_window_adjust(uint32_t chan, uint32_t bytes, uint8_t *out, int outmax);
int ssh_parse_window_adjust(const uint8_t *payload, int len, uint32_t *chan, uint32_t *bytes);

int ssh_build_eof(uint32_t chan, uint8_t *out, int outmax);
int ssh_build_close(uint32_t chan, uint8_t *out, int outmax);
int ssh_parse_close(const uint8_t *payload, int len, uint32_t *chan);

/* channel-request "exit-status", want_reply = FALSE (RFC 4254 6.10). */
int ssh_build_exit_status(uint32_t chan, uint32_t status, uint8_t *out, int outmax);

int ssh_build_disconnect(uint32_t reason, const char *msg, uint8_t *out, int outmax);

#endif /* LOGIT_SSH_CONN_H */
