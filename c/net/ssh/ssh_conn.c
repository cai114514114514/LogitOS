#include "ssh_conn.h"
#include "ssh_wire.h"
#include "ssh.h"

int ssh_parse_channel_open(const uint8_t *payload, int len,
                           char *type, int typemax,
                           uint32_t *peer_chan, uint32_t *peer_win, uint32_t *peer_maxpkt)
{
    if (len < 1 || payload[0] != SSH_MSG_CHANNEL_OPEN) return -1;
    int off = ssh_r_string_cpy(payload, 1, len, type, typemax, 0);
    if (off < 0) return -1;
    off = ssh_r_u32(payload, off, len, peer_chan);
    off = ssh_r_u32(payload, off, len, peer_win);
    off = ssh_r_u32(payload, off, len, peer_maxpkt);
    return off < 0 ? -1 : 0;
}

int ssh_build_channel_open_confirmation(uint32_t recipient_chan, uint32_t sender_chan,
                                        uint32_t init_win, uint32_t max_pkt,
                                        uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    off = ssh_w_u32(out, off, outmax, recipient_chan);
    off = ssh_w_u32(out, off, outmax, sender_chan);
    off = ssh_w_u32(out, off, outmax, init_win);
    return ssh_w_u32(out, off, outmax, max_pkt);
}

int ssh_build_channel_open_failure(uint32_t recipient_chan, uint32_t reason_code,
                                   uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_OPEN_FAILURE);
    off = ssh_w_u32(out, off, outmax, recipient_chan);
    off = ssh_w_u32(out, off, outmax, reason_code);
    off = ssh_w_cstring(out, off, outmax, "");
    return ssh_w_cstring(out, off, outmax, "");
}

int ssh_parse_channel_request(const uint8_t *payload, int len,
                              uint32_t *chan, char *type, int typemax,
                              int *want_reply, const uint8_t **data, int *datalen)
{
    if (len < 1 || payload[0] != SSH_MSG_CHANNEL_REQUEST) return -1;
    int off = ssh_r_u32(payload, 1, len, chan);
    if (off < 0) return -1;
    off = ssh_r_string_cpy(payload, off, len, type, typemax, 0);
    if (off < 0) return -1;
    off = ssh_r_bool(payload, off, len, want_reply);
    if (off < 0) return -1;
    *data = payload + off;
    *datalen = len - off;
    return 0;
}

int ssh_build_channel_success(uint32_t chan, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_SUCCESS);
    return ssh_w_u32(out, off, outmax, chan);
}

int ssh_build_channel_failure(uint32_t chan, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_FAILURE);
    return ssh_w_u32(out, off, outmax, chan);
}

int ssh_parse_exec_command(const uint8_t *data, int datalen, char *cmd, int cmdmax)
{
    return ssh_r_string_cpy(data, 0, datalen, cmd, cmdmax, 0) < 0 ? -1 : 0;
}

int ssh_build_channel_data(uint32_t chan, const uint8_t *data, int datalen, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_DATA);
    off = ssh_w_u32(out, off, outmax, chan);
    return ssh_w_string(out, off, outmax, data, datalen);
}

int ssh_parse_channel_data(const uint8_t *payload, int len, uint32_t *chan,
                           const uint8_t **data, int *datalen)
{
    if (len < 1 || payload[0] != SSH_MSG_CHANNEL_DATA) return -1;
    int off = ssh_r_u32(payload, 1, len, chan);
    if (off < 0) return -1;
    return ssh_r_string(payload, off, len, data, datalen) < 0 ? -1 : 0;
}

int ssh_build_window_adjust(uint32_t chan, uint32_t bytes, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    off = ssh_w_u32(out, off, outmax, chan);
    return ssh_w_u32(out, off, outmax, bytes);
}

int ssh_parse_window_adjust(const uint8_t *payload, int len, uint32_t *chan, uint32_t *bytes)
{
    if (len < 1 || payload[0] != SSH_MSG_CHANNEL_WINDOW_ADJUST) return -1;
    int off = ssh_r_u32(payload, 1, len, chan);
    if (off < 0) return -1;
    return ssh_r_u32(payload, off, len, bytes) < 0 ? -1 : 0;
}

int ssh_build_eof(uint32_t chan, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_EOF);
    return ssh_w_u32(out, off, outmax, chan);
}

int ssh_build_close(uint32_t chan, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_CLOSE);
    return ssh_w_u32(out, off, outmax, chan);
}

int ssh_parse_close(const uint8_t *payload, int len, uint32_t *chan)
{
    if (len < 1 || payload[0] != SSH_MSG_CHANNEL_CLOSE) return -1;
    return ssh_r_u32(payload, 1, len, chan) < 0 ? -1 : 0;
}

int ssh_build_exit_status(uint32_t chan, uint32_t status, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_CHANNEL_REQUEST);
    off = ssh_w_u32(out, off, outmax, chan);
    off = ssh_w_cstring(out, off, outmax, "exit-status");
    off = ssh_w_bool(out, off, outmax, 0);
    return ssh_w_u32(out, off, outmax, status);
}

int ssh_build_disconnect(uint32_t reason, const char *msg, uint8_t *out, int outmax)
{
    int off = ssh_w_u8(out, 0, outmax, SSH_MSG_DISCONNECT);
    off = ssh_w_u32(out, off, outmax, reason);
    off = ssh_w_cstring(out, off, outmax, msg);
    return ssh_w_cstring(out, off, outmax, "");
}
