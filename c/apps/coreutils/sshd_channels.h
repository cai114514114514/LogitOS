#ifndef LOGIT_SSHD_CHANNELS_H
#define LOGIT_SSHD_CHANNELS_H
/* 2026-09-11: replaces the one-channel relay. The connection reader owns
 * requests/transport; one pump owns child I/O, kill/wait and fd retirement.
 * Each input queue has its own advertised window, so a paused command does
 * not prevent a second command or a key exchange from making progress. */
static int reply_msg(struct conn_ctx *cc,const uint8_t *p,int n)
{
    if(n<1)return -1;
    if(cc->kex_busy){
        if(n>512||cc->nreplies==64)return -1;
        int i=cc->nreplies++;cc->replies[i].len=n;memcpy(cc->replies[i].data,p,n);return 0;
    }
    return send_msg(cc,p,n);
}
static struct channel_ctx *channel_find(struct conn_ctx *cc,uint32_t id)
{
    for(int i=0;i<SSHD_CHANNELS;i++)if(cc->channels[i].used&&cc->channels[i].local_chan==id)return &cc->channels[i];
    return 0;
}
static struct channel_ctx *channel_alloc(struct conn_ctx *cc)
{
    for(int i=0;i<SSHD_CHANNELS;i++){
        struct channel_ctx *ch=&cc->channels[i];
        /* Retired channels own no fds and never send another packet. Local
         * ids increase instead of recycling, so a late peer CLOSE cannot
         * address a replacement channel. Do not charge those close replies
         * against the active-channel budget during an overlapping transfer. */
        if(ch->used==3&&ch->sent_close)ch->used=0;
        if(ch->used)continue;
        memset(ch,0,sizeof *ch);ch->conn=cc;ch->index=i;ch->local_chan=cc->next_channel++;
        ch->child_in_w=ch->child_out_r=ch->child_err_r=ch->pty_master=ch->pty_slave=-1;
        ch->opened_ns=monotonic_ns();ch->used=1;return ch;
    }
    return 0;
}
static void close_fd(int *fd){if(*fd>=0){sys_close(*fd);*fd=-1;}}
static int forward_enabled(void)
{
    struct logit_stat s;
    return st_lstat("/etc/sshd.forward.enabled",&s)==0&&s.uid==0&&(s.mode&LST_IFMT)==LST_IFREG&&!(s.mode&022);
}
static int ipv4_target(const char *s,uint32_t *ip)
{
    if(c_streq(s,"localhost")){*ip=0x7f000001u;return 0;}
    unsigned v=0;int j=0;
    for(int part=0;part<4;part++){
        unsigned n=0;int digits=0;while(s[j]>='0'&&s[j]<='9'){n=n*10+s[j++]-'0';if(n>255||++digits>3)return -1;}
        if(!digits||(part<3?s[j++]!='.':s[j]!=0))return -1;v=(v<<8)|n;
    }
    *ip=v;return 0;
}
static int direct_open(struct channel_ctx *ch,const uint8_t *data,int n)
{
    if(!forward_enabled())return -1;
    char host[256],origin[256];int hlen,olen;uint32_t port,origin_port;
    int o=ssh_r_string_cpy(data,0,n,host,sizeof host,&hlen);o=ssh_r_u32(data,o,n,&port);
    o=ssh_r_string_cpy(data,o,n,origin,sizeof origin,&olen);o=ssh_r_u32(data,o,n,&origin_port);
    uint32_t ip;if(o!=n||o<0||hlen>=(int)sizeof host||olen>=(int)sizeof origin||!port||port>65535||ipv4_target(host,&ip)<0)return -1;
    int fd=sys_socket(LOGIT_AF_INET,LOGIT_SOCK_STREAM,0);if(fd<0)return -1;
    struct logit_sockaddr dest;sockaddr_set(&dest,ip,port);
    if(_sys(SYS_CONNECT,fd,(long)&dest,sizeof dest)<0){sys_close(fd);return -1;}
    int rd=sys_dup(fd);if(rd<0){sys_close(fd);return -1;}
    ch->forward=1;ch->child_in_w=fd;ch->child_out_r=rd;return 0;
}
static int spawn_child(struct channel_ctx *ch, const char *cmd /* NULL = shell */)
{
    int ip[2], op[2], ep[2]; /* separate stdout/stderr preserve remote command semantics */
    if (ch->pty_master>=0) {
        ip[0]=ch->pty_slave;ip[1]=ch->pty_master;
        op[0]=sys_dup(ch->pty_master);op[1]=sys_dup(ch->pty_slave);
        ep[0]=-1;ep[1]=sys_dup(ch->pty_slave);
        if(op[0]<0||op[1]<0||ep[1]<0){sys_close(op[0]);sys_close(op[1]);sys_close(ep[1]);return -1;}
    } else {
        if (sys_pipe(ip) < 0) return -1;
        if (sys_pipe(op) < 0) { sys_close(ip[0]); sys_close(ip[1]); return -1; }
        if (sys_pipe(ep) < 0) {
            sys_close(ip[0]); sys_close(ip[1]); sys_close(op[0]); sys_close(op[1]);
            return -1;
        }
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(ep[0]); sys_close(ep[1]);
        sys_close(ip[0]); sys_close(ip[1]); sys_close(op[0]); sys_close(op[1]);
        if(ch->pty_master>=0) { ch->pty_master=ch->pty_slave=ch->child_in_w=-1; }
        return -1;
    }
    if (pid == 0) {
        struct logit_sigaction normal={0};_sys(SYS_SIGACTION,LOGIT_SIGPIPE,(long)&normal,0);
        sys_dup2(ip[0], 0);
        sys_dup2(op[1], 1);
        sys_dup2(ep[1], 2);
        /* fork inherits the whole process fd table, including other sessions
         * and the listener. Only the command's three streams belong in exec. */
        for (int fd = 3; fd < LOGIT_POLL_MAX; fd++) sys_close(fd);

        if (_sys(SYS_SETGROUPS, 0, 0, 0) < 0 || sys_setgid(ch->conn->acct.gid) < 0 || sys_setuid(ch->conn->acct.uid) < 0) {
            errs("sshd: could not drop privileges to the authenticated user\n");
            app_exit(126);
        }
        if (sys_chdir(ch->conn->acct.home) < 0) { errs("sshd: home unavailable\n"); app_exit(126); }

        static char envh[ACCT_PATH + 8], envu[ACCT_NAME + 8];
        char *e = envh; const char *pre = "HOME="; int k = 0;
        for (int i = 0; pre[i]; i++) e[k++] = pre[i];
        for (int i = 0; ch->conn->acct.home[i] && k < (int)sizeof envh - 1; i++) e[k++] = ch->conn->acct.home[i];
        e[k] = 0;
        char *e2 = envu; const char *pre2 = "USER="; k = 0;
        for (int i = 0; pre2[i]; i++) e2[k++] = pre2[i];
        for (int i = 0; ch->conn->acct.name[i] && k < (int)sizeof envu - 1; i++) e2[k++] = ch->conn->acct.name[i];
        e2[k] = 0;
        char envterm[70];c_strcpy(envterm,"TERM=",sizeof envterm);c_strcpy(envterm+5,ch->term,sizeof envterm-5);
        char *envp[] = { envh, envu, (char *)"PATH=/bin", ch->pty_master>=0?envterm:0, 0 };

        if (ch->subsystem) {
            char *argv[] = { (char *)"sftpd", 0 };
            sys_execve("/bin/sftpd", argv, envp);
        } else if (cmd) {
            char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, 0 };
            sys_execve(ch->conn->acct.shell[0] ? ch->conn->acct.shell : "/bin/sh", argv, envp);
        } else {
            char *argv[] = { (char *)"sh", 0 };
            sys_execve(ch->conn->acct.shell[0] ? ch->conn->acct.shell : "/bin/sh", argv, envp);
        }
        errs("sshd: exec failed\n");
        app_exit(127);
    }

    sys_close(ip[0]);
    sys_close(op[1]);
    sys_close(ep[1]);
    ch->pty_slave=-1;
    ch->child_err_r = ep[0];
    ch->child_pid = pid;
    ch->child_in_w = ip[1];
    ch->child_out_r = op[0];
    return 0;
}

static void channel_start(struct channel_ctx *ch)
{
    if(ch->child_in_w>=0)sys_set_nonblock(ch->child_in_w);
    if(ch->child_out_r>=0)sys_set_nonblock(ch->child_out_r);
    if(ch->child_err_r>=0)sys_set_nonblock(ch->child_err_r);
    __atomic_store_n(&ch->used,2,__ATOMIC_RELEASE);
}
static int channel_pty(struct channel_ctx *ch,const uint8_t *data,int n)
{
    const uint8_t *term,*modes;int tlen,mlen;uint32_t cols,rows,x,y;
    int o=ssh_r_string(data,0,n,&term,&tlen);o=ssh_r_u32(data,o,n,&cols);o=ssh_r_u32(data,o,n,&rows);
    o=ssh_r_u32(data,o,n,&x);o=ssh_r_u32(data,o,n,&y);o=ssh_r_string(data,o,n,&modes,&mlen);
    if(o!=n||o<0||tlen<1||tlen>63||cols>65535||rows>65535||x>65535||y>65535||ch->pty_master>=0)return -1;
    for(int i=0;i<tlen;i++){if(term[i]<32||term[i]>126)return -1;ch->term[i]=term[i];}ch->term[tlen]=0;
    int fds[2];if(_sys(SYS_PTY_OPEN,(long)fds,0,0)<0)return -1;
    ch->pty_master=fds[0];ch->pty_slave=fds[1];ch->child_in_w=fds[0];
    struct logit_termios a;struct logit_winsize w={rows,cols,x,y};
    if(_sys(SYS_PTY_CTL,fds[0],LPTY_GETATTR,(long)&a)<0)return -1;
    for(int i=0;i<mlen;){int op=modes[i++];if(!op||op>=160)break;
        uint32_t v;int z=ssh_r_u32(modes,i,mlen,&v);if(z<0)return -1;i=z;
        switch(op){
        case 1:a.cc[LPTY_VINTR]=v;break;case 2:a.cc[LPTY_VQUIT]=v;break;
        case 3:a.cc[LPTY_VERASE]=v;break;case 4:a.cc[LPTY_VKILL]=v;break;case 5:a.cc[LPTY_VEOF]=v;break;
        case 36:a.iflag=v?a.iflag|LPTY_ICRNL:a.iflag&~LPTY_ICRNL;break;
        case 50:a.lflag=v?a.lflag|LPTY_ISIG:a.lflag&~LPTY_ISIG;break;
        case 51:a.lflag=v?a.lflag|LPTY_ICANON:a.lflag&~LPTY_ICANON;break;
        case 53:a.lflag=v?a.lflag|LPTY_ECHO:a.lflag&~LPTY_ECHO;break;
        case 54:a.lflag=v?a.lflag|LPTY_ECHOE:a.lflag&~LPTY_ECHOE;break;
        case 70:a.oflag=v?a.oflag|LPTY_OPOST:a.oflag&~LPTY_OPOST;break;
        case 72:a.oflag=v?a.oflag|LPTY_ONLCR:a.oflag&~LPTY_ONLCR;break;
        default:break;
        }
    }
    return _sys(SYS_PTY_CTL,fds[0],LPTY_SETATTR,(long)&a)<0||_sys(SYS_PTY_CTL,fds[0],LPTY_SETWIN,(long)&w)<0?-1:0;
}

static int channel_request(struct channel_ctx *ch,const uint8_t *buf,int n)
{
    uint32_t id;char type[40];int want,len;const uint8_t *data;
    if(ssh_parse_channel_request(buf,n,&id,type,sizeof type,&want,&data,&len)<0)return -1;
    int ok=0;
    if(ch->used==1&&c_streq(type,"pty-req"))ok=channel_pty(ch,data,len)==0;
    else if(ch->used==1&&(c_streq(type,"shell")||c_streq(type,"exec")||c_streq(type,"subsystem"))){
        char cmd[512];const char *command=0;int valid=1;
        if(c_streq(type,"exec")){valid=ssh_parse_exec_command(data,len,cmd,sizeof cmd)==0;command=cmd;}
        if(c_streq(type,"subsystem")){
            valid=ssh_parse_exec_command(data,len,cmd,sizeof cmd)==0&&c_streq(cmd,"sftp")&&ch->pty_master<0;
            ch->subsystem=valid;
        }
        if(valid&&spawn_child(ch,command)==0){
            if(want){uint8_t p[16];int z=ssh_build_channel_success(ch->peer_chan,p,sizeof p);if(reply_msg(ch->conn,p,z)<0)return -1;}
            channel_start(ch);return 0;
        }
    }else if(ch->used==2&&ch->pty_master>=0&&c_streq(type,"window-change")){
        uint32_t cols,rows,x,y;int o=ssh_r_u32(data,0,len,&cols);o=ssh_r_u32(data,o,len,&rows);
        o=ssh_r_u32(data,o,len,&x);o=ssh_r_u32(data,o,len,&y);
        if(o==len&&o>=0&&cols<=65535&&rows<=65535&&x<=65535&&y<=65535){
            struct logit_winsize w={rows,cols,x,y};ok=_sys(SYS_PTY_CTL,ch->child_out_r,LPTY_SETWIN,(long)&w)==0;}
    }
    if(want){uint8_t p[16];int z=ok?ssh_build_channel_success(ch->peer_chan,p,sizeof p):ssh_build_channel_failure(ch->peer_chan,p,sizeof p);
        if(reply_msg(ch->conn,p,z)<0)return -1;}
    /* A required setup operation failed. End this channel while leaving other
     * channels usable; do not execute a pipelined command after a failed PTY. */
    if(!ok&&ch->used==1&&(c_streq(type,"pty-req")||c_streq(type,"exec")||c_streq(type,"subsystem")||c_streq(type,"shell")))ch->input_eof=ch->recv_close=1;
    return 0;
}

static int global_request(struct conn_ctx *cc,const uint8_t *buf,int n)
{
    char name[64];int want,namelen;int o=ssh_r_string_cpy(buf,1,n,name,sizeof name,&namelen);
    o=ssh_r_bool(buf,o,n,&want);if(o<0||namelen>=(int)sizeof name)return -1;
    uint8_t rep[8];int z=1,ok=0;
    if(c_streq(name,"keepalive@openssh.com"))ok=1;
    else if(c_streq(name,"tcpip-forward")||c_streq(name,"cancel-tcpip-forward")){
        char address[32];int alen;uint32_t port;
        o=ssh_r_string_cpy(buf,o,n,address,sizeof address,&alen);o=ssh_r_u32(buf,o,n,&port);
        if(o!=n||o<0||alen>=(int)sizeof address||port>65535)return -1;
        if(c_streq(name,"cancel-tcpip-forward")){
            for(int i=0;i<SSHD_REMOTE_LISTENERS;i++)if(cc->remote[i].fd>=0&&cc->remote[i].port==port&&c_streq(cc->remote[i].name,address)){
                close_fd(&cc->remote[i].fd);ok=1;break;}
        }else if(forward_enabled()&&(c_streq(address,"localhost")||c_streq(address,"127.0.0.1"))&&(!port||port>=1024)){
            int slot=-1;for(int i=0;i<SSHD_REMOTE_LISTENERS;i++)if(cc->remote[i].fd<0){slot=i;break;}
            if(slot>=0){
                /* Remote forwarding binds loopback explicitly. Port 0 asks us
                 * to allocate a local listener; only a successful bind/listen
                 * is returned, and cancellation releases that exact listener. */
                int fd=-1;uint32_t chosen=port;
                for(int attempt=0;attempt<(port?1:64);attempt++){
                    if(!port){uint32_t r;rnd((uint8_t *)&r,sizeof r);chosen=40000+r%9000;}
                    fd=sys_socket(LOGIT_AF_INET,LOGIT_SOCK_STREAM,0);if(fd<0)break;
                    struct logit_sockaddr a;sockaddr_set(&a,0x7f000001u,chosen);
                    if(sys_bind(fd,&a)==0&&sys_listen(fd,4)==0)break;sys_close(fd);fd=-1;
                }
                if(fd>=0){sys_set_nonblock(fd);cc->remote[slot].fd=fd;cc->remote[slot].port=chosen;c_strcpy(cc->remote[slot].name,address,sizeof cc->remote[slot].name);ok=1;
                    if(!port)z=ssh_w_u32(rep,1,sizeof rep,chosen);}
            }
        }
    }
    rep[0]=ok?SSH_MSG_REQUEST_SUCCESS:SSH_MSG_REQUEST_FAILURE;
    return want?reply_msg(cc,rep,ok?z:1):0;
}

static int channel_dispatch(struct conn_ctx *cc,const uint8_t *buf,int n)
{
    if(n<1)return 0;
    if(buf[0]==SSH_MSG_DISCONNECT)return -1;
    if(buf[0]==SSH_MSG_GLOBAL_REQUEST)return global_request(cc,buf,n);
    if(buf[0]==SSH_MSG_CHANNEL_OPEN){
        const uint8_t *type;int tlen;uint32_t peer,win,maxpkt;
        int o=ssh_r_string(buf,1,n,&type,&tlen);o=ssh_r_u32(buf,o,n,&peer);o=ssh_r_u32(buf,o,n,&win);o=ssh_r_u32(buf,o,n,&maxpkt);
        if(o<0||!maxpkt)return -1;
        int session=tlen==7&&c_strncmp((const char *)type,"session",7)==0,forward=tlen==12&&c_strncmp((const char *)type,"direct-tcpip",12)==0;
        struct channel_ctx *ch=(session||forward)?channel_alloc(cc):0;
        if(ch){ch->peer_chan=peer;ch->peer_window=win;ch->peer_maxpkt=maxpkt<OUR_MAX_PACKET?maxpkt:OUR_MAX_PACKET;
            if(forward&&direct_open(ch,buf+o,n-o)<0){ch->used=0;ch=0;}}
        uint8_t p[80];int z;
        if(!ch)z=ssh_build_channel_open_failure(peer,SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,p,sizeof p);
        else z=ssh_build_channel_open_confirmation(peer,ch->local_chan,OUR_INIT_WINDOW,OUR_MAX_PACKET,p,sizeof p);
        if(reply_msg(cc,p,z)<0)return -1;
        if(ch&&forward)channel_start(ch);return 0;
    }
    uint32_t id;if(ssh_r_u32(buf,1,n,&id)<0)return -1;
    struct channel_ctx *ch=channel_find(cc,id);if(!ch)return 0;
    if(buf[0]==SSH_MSG_CHANNEL_OPEN_CONFIRMATION&&ch->used==4){
        uint32_t peer,win,maxpkt;int o=ssh_r_u32(buf,5,n,&peer);o=ssh_r_u32(buf,o,n,&win);o=ssh_r_u32(buf,o,n,&maxpkt);
        if(o<0||!maxpkt)return -1;ch->peer_chan=peer;ch->peer_window=win;ch->peer_maxpkt=maxpkt<OUR_MAX_PACKET?maxpkt:OUR_MAX_PACKET;channel_start(ch);return 0;
    }
    if(buf[0]==SSH_MSG_CHANNEL_OPEN_FAILURE&&ch->used==4){close_fd(&ch->child_in_w);close_fd(&ch->child_out_r);ch->used=0;return 0;}
    if(buf[0]==SSH_MSG_CHANNEL_REQUEST)return channel_request(ch,buf,n);
    if(buf[0]==SSH_MSG_CHANNEL_DATA){
        const uint8_t *data;int len;
        if(ssh_parse_channel_data(buf,n,&id,&data,&len)<0||len<0||len>(int)OUR_MAX_PACKET)return -1;
        if(ch->used!=2||ch->input_eof)return 0;
        spin_lock(&cc->lock);
        if((unsigned)len>OUR_INIT_WINDOW-ch->recv_used||(unsigned)len>SSHD_INPUT_CAP-ch->input_count){spin_unlock(&cc->lock);return -1;}
        unsigned tail=(ch->input_head+ch->input_count)%SSHD_INPUT_CAP;
        for(int i=0;i<len;i++)ch->input[(tail+i)%SSHD_INPUT_CAP]=data[i];
        ch->input_count+=len;ch->recv_used+=len;spin_unlock(&cc->lock);return 0;
    }
    if(buf[0]==SSH_MSG_CHANNEL_WINDOW_ADJUST){
        uint32_t credit;if(ssh_parse_window_adjust(buf,n,&id,&credit)<0)return -1;
        spin_lock(&cc->lock);if(credit>0xffffffffu-ch->peer_window){spin_unlock(&cc->lock);return -1;}
        ch->peer_window+=credit;spin_unlock(&cc->lock);return 0;
    }
    if(buf[0]==SSH_MSG_CHANNEL_EOF)ch->input_eof=1;
    if(buf[0]==SSH_MSG_CHANNEL_CLOSE)ch->recv_close=ch->input_eof=1;
    return 0;
}

static void channel_finish(struct channel_ctx *ch,int aborting)
{
    struct conn_ctx *cc=ch->conn;uint8_t p[32];int z;
    if(ch->child_pid&&!ch->reaped){
        if(aborting)_sys(SYS_KILL,ch->child_pid,LOGIT_SIGKILL,LOGIT_KILL_SIGNAL);
        int rc=(int)_sys(SYS_WAITPID,ch->child_pid,(long)&ch->child_status,1);
        if(rc==0||rc==SIG_E_INTR)return;
        ch->reaped=1;ch->child_pid=0;
    }
    if(!cc->stopping&&!ch->sent_close){
        /* No recipient id exists until the peer confirms a server-opened
         * channel. Retire a timed-out pending open without addressing id 0. */
        if(ch->used==4){close_fd(&ch->child_in_w);close_fd(&ch->child_out_r);ch->used=0;return;}
        if(!aborting){
            if(!ch->forward){z=ssh_build_exit_status(ch->peer_chan,ch->child_status,p,sizeof p);send_msg(cc,p,z);}
            z=ssh_build_eof(ch->peer_chan,p,sizeof p);send_msg(cc,p,z);
        }
        z=ssh_build_close(ch->peer_chan,p,sizeof p);send_msg(cc,p,z);ch->sent_close=1;
    }
    close_fd(&ch->child_in_w);close_fd(&ch->child_out_r);close_fd(&ch->child_err_r);close_fd(&ch->pty_slave);
    __atomic_store_n(&ch->used,3,__ATOMIC_RELEASE);
}

static void channel_pump(struct channel_ctx *ch)
{
    struct conn_ctx *cc=ch->conn;
    int state=__atomic_load_n(&ch->used,__ATOMIC_ACQUIRE);
    if(!state||state==3)return;
    if(cc->stopping||ch->recv_close){channel_finish(ch,1);return;}
    if(state!=2)return;
    if(ch->pty_master>=0&&ch->child_pid){int sig;
        while((sig=(int)_sys(SYS_PTY_CTL,ch->child_out_r,LPTY_SIGNAL,0))>0)_sys(SYS_KILL,ch->child_pid,sig,LOGIT_KILL_SIGNAL);}
    uint8_t bytes[4096],packet[4109];
    spin_lock(&cc->lock);unsigned available=ch->input_count;if(available>sizeof bytes)available=sizeof bytes;
    for(unsigned i=0;i<available;i++)bytes[i]=ch->input[(ch->input_head+i)%SSHD_INPUT_CAP];spin_unlock(&cc->lock);
    if(available&&ch->child_in_w>=0){
        int n=sys_write(ch->child_in_w,bytes,available);
        if(n>0){
            spin_lock(&cc->lock);ch->input_head=(ch->input_head+n)%SSHD_INPUT_CAP;ch->input_count-=n;ch->recv_used-=n;spin_unlock(&cc->lock);
            int z=ssh_build_window_adjust(ch->peer_chan,n,packet,sizeof packet);
            if(send_msg(cc,packet,z)<0){cc->stopping=1;return;}
        }else if(n!=LSK_E_AGAIN&&n!=SIG_E_INTR){close_fd(&ch->child_in_w);}
    }
    if(ch->input_eof&&ch->input_count==0&&ch->child_in_w>=0){
        if(ch->forward)sys_shutdown(ch->child_in_w,LOGIT_SHUT_WR);
        if(ch->pty_master>=0){struct logit_termios a;
            if(_sys(SYS_PTY_CTL,ch->child_in_w,LPTY_GETATTR,(long)&a)==0&&(a.lflag&LPTY_ICANON)){
                int n=sys_write(ch->child_in_w,&a.cc[LPTY_VEOF],1);if(n==LSK_E_AGAIN)return;}}
        close_fd(&ch->child_in_w);
    }
    for(int stream=0;stream<2;stream++){
        int *fd=stream?&ch->child_err_r:&ch->child_out_r;if(*fd<0)continue;
        spin_lock(&cc->lock);uint32_t room=ch->peer_window;spin_unlock(&cc->lock);
        if(!room)continue;
        unsigned want=sizeof bytes;if(want>room)want=room;if(want>ch->peer_maxpkt)want=ch->peer_maxpkt;
        int n=sys_read(*fd,bytes,want);
        if(n==0){close_fd(fd);continue;}
        if(n==LSK_E_AGAIN||n==SIG_E_INTR)continue;
        if(n<0){close_fd(fd);continue;}
        int z=stream?ssh_build_channel_stderr(ch->peer_chan,bytes,n,packet,sizeof packet):ssh_build_channel_data(ch->peer_chan,bytes,n,packet,sizeof packet);
        if(send_msg(cc,packet,z)<0){cc->stopping=1;return;}
        spin_lock(&cc->lock);ch->peer_window-=n;spin_unlock(&cc->lock);
    }
    if(ch->child_out_r<0&&ch->child_err_r<0)channel_finish(ch,0);
}
static void multi_pump(void *arg)
{
    struct conn_ctx *cc=arg;
    for(;;){int pending=0;
        for(int i=0;i<SSHD_CHANNELS;i++){channel_pump(&cc->channels[i]);int s=cc->channels[i].used;if(s&&s!=3)pending=1;}
        if(cc->stopping&&!pending)return;
        sys_sleep_ms(2);
    }
}

static int remote_accept(struct conn_ctx *cc,struct remote_listener *r)
{
    /* Leave a completed connection in TCP's bounded backlog until a relay
     * slot is free. Accept-and-close here discarded ordinary overlapping
     * transfers while the previous channel's CLOSE was still in flight. */
    struct channel_ctx *ch=channel_alloc(cc);if(!ch)return 0;
    struct logit_sockaddr peer={0};int fd=sys_accept(r->fd,&peer,0);if(fd<0){ch->used=0;return 0;}
    int rd=sys_dup(fd);if(rd<0){sys_close(fd);ch->used=0;return 0;}
    ch->forward=1;ch->child_in_w=fd;ch->child_out_r=rd;ch->used=4;
    char origin[32];int j=0;
    for(int i=3;i>=0;i--){unsigned v=(peer.addr>>(i*8))&255;if(v>=100)origin[j++]='0'+v/100;if(v>=10)origin[j++]='0'+(v/10)%10;origin[j++]='0'+v%10;if(i)origin[j++]='.';}origin[j]=0;
    uint8_t p[256];int z=ssh_w_u8(p,0,sizeof p,SSH_MSG_CHANNEL_OPEN);
    z=ssh_w_cstring(p,z,sizeof p,"forwarded-tcpip");z=ssh_w_u32(p,z,sizeof p,ch->local_chan);
    z=ssh_w_u32(p,z,sizeof p,OUR_INIT_WINDOW);z=ssh_w_u32(p,z,sizeof p,OUR_MAX_PACKET);
    z=ssh_w_cstring(p,z,sizeof p,r->name);z=ssh_w_u32(p,z,sizeof p,r->port);z=ssh_w_cstring(p,z,sizeof p,origin);z=ssh_w_u32(p,z,sizeof p,peer.port);
    return reply_msg(cc,p,z);
}
static int run_channels(struct conn_ctx *cc)
{
    cc->pump_targ.fn=multi_pump;cc->pump_targ.ctx=cc;
    struct logit_thread_spec spec={0};spec.entry=(unsigned long)&sshd_thread_entry;
    spec.stack_top=(unsigned long)(g_pump_stack[cc->slot]+PUMP_STACK_SIZE);spec.arg=(unsigned long)&cc->pump_targ;
    int tid=sys_thread_create(&spec);if(tid<=0)return -1;
    int rc=0;uint8_t buf[SSH_MAX_PAYLOAD];
    while(!cc->stopping){
        unsigned long long now=monotonic_ns();
        if((g_rekey_bytes&&cc->key_bytes>=g_rekey_bytes)||(g_rekey_ns&&now-cc->key_since_ns>=g_rekey_ns)){
            if(do_kex(cc,0,0)<0){rc=-1;break;}
        }
        struct logit_pollfd fds[1+SSHD_REMOTE_LISTENERS];fds[0]=(struct logit_pollfd){cc->sockfd,LPOLLIN,0};
        for(int i=0;i<SSHD_REMOTE_LISTENERS;i++)fds[i+1]=(struct logit_pollfd){cc->remote[i].fd,LPOLLIN,0};
        int ready=(int)_sys(SYS_POLL,(long)fds,1+SSHD_REMOTE_LISTENERS,20);
        if(ready==SIG_E_INTR)continue;if(ready<0){rc=-1;break;}
        if(fds[0].revents){int n=recv_msg(cc,buf,sizeof buf);if(n<0||channel_dispatch(cc,buf,n)<0){rc=-1;break;}}
        for(int i=0;i<SSHD_REMOTE_LISTENERS;i++)if(fds[i+1].revents&&cc->remote[i].fd>=0&&remote_accept(cc,&cc->remote[i])<0){rc=-1;cc->stopping=1;}
        now=monotonic_ns(); /* accepted channels were created after the loop's first timestamp */
        for(int i=0;i<SSHD_CHANNELS;i++)if(cc->channels[i].used==4&&now-cc->channels[i].opened_ns>30000000000ull)cc->channels[i].recv_close=1;
    }
    cc->stopping=1;sys_shutdown(cc->sockfd,LOGIT_SHUT_RDWR);
    for(int i=0;i<SSHD_REMOTE_LISTENERS;i++)close_fd(&cc->remote[i].fd);
    _sys(SYS_THREAD_JOIN,tid,0,0);return rc;
}
#endif
