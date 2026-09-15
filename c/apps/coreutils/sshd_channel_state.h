#ifndef LOGIT_SSHD_CHANNEL_STATE_H
#define LOGIT_SSHD_CHANNEL_STATE_H
/* Four independent flow-control windows share one encrypted transport. A
 * blocked child's stdin consumes only its own bounded queue; writing it in
 * the network reader would also block other channels and NEWKEYS. */
#ifndef SSHD_CHANNELS
#define SSHD_CHANNELS 4
#endif
#define SSHD_REMOTE_LISTENERS 2
#define SSHD_INPUT_CAP (64u*1024u)
struct conn_ctx;
struct channel_ctx {
    struct conn_ctx *conn;
    int index;
    uint32_t local_chan,peer_chan,peer_window,peer_maxpkt,recv_used;
    volatile int used; /* 0 free, 1 requests, 2 relay, 3 drained, 4 peer open pending */
    int subsystem,forward;
    int pty_master,pty_slave;
    char term[64];
    int child_pid,child_in_w,child_out_r,child_err_r;
    int child_status,reaped;
    volatile int input_eof,recv_close,sent_close;
    unsigned input_head,input_count;
    uint8_t input[SSHD_INPUT_CAP];
    unsigned long long opened_ns;
};
struct remote_listener {int fd;uint32_t port;char name[32];};
#endif
