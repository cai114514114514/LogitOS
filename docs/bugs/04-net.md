# c/net 全量缺陷审计（tcp/ip/udp/dns/http/hpack/tls/ssh/link/core）

日期：2026-09-16

## 方法与覆盖

- 审计对象：`c/net/` 全部 40 个 .c（约 23,200 行），按优先级深读：`transport/tcp.c`（全文）、
  `ip/reasm.c`、`ip/ip.c`、`transport/udp.c`、`ip/icmp.c`、`dns/dns.c`（全文）、
  `http/http2.c`、`http/hpack.c`、`http/cookies.c`、`http/http1.c`、`http/hpool.c`、`http/url.c`、
  `tls/tls.c`、`tls/tls12.c`、`tls/tls_server.c`（全文）、`tls/tls_int.h`、`ssh/` 全部 9 个文件（全文）、
  `core/netlock.c`、`core/net.c`、`core/lsock.c`、`core/route.c`；略读 `link/arp.c`、`core/unix.c`、
  `core/dhcp.c`、`tls/x509.c`、`http/ws.c`。
- 去重先行：读 `docs/CODE_AUDIT.md`（2026-08-04 审计及其修复批记录）、
  `docs/NET_BUG_REVIEW_2026-09-09.md`（N01–N09）、grep `CLAUDE.md` 的 "OPEN BUG"（仅 munmap TLB 一条，非 net）。
  2026-08-04 审计中 net/tls 的严重与高危项（C2/M2/M3/S5–S8、TCP 校验和、RST 窗口校验等）已在源码中确认修复
  （如 `tls.c:1601/1969` 的 `blen-16` 拒绝、`tls.c:1407-1423` Certificate 边界、`tls.c:1563` CertVerify 存在性门），
  本报告不重复。
- 本文全部发现给出 file:line 与代码引用；逻辑推演无运行复现的条目如实标注。未修改任何源码。

---

## 发现

### [critical] [CONFIRMED] UDP 接收队列入队无单槽长度上限 —— 分片 UDP 数据报可越界写内核 .bss

位置：`c/net/transport/udp.c:210-219`（溢出点 214），缓冲声明 `udp.c:21,33`

```c
        } else {
            int tail = (s->qhead + s->qcount) % UDP_QUEUES;
            memcpy(s->q[tail], payload, dlen);      /* dlen 最大 65527，槽只有 1500 字节 */
```

`struct udp_sock` 的 `q[UDP_QUEUES][UDP_SLOT]` 每槽 `UDP_SLOT = 1500`（udp.c:21），且 `q` 是结构体
最后一个成员。入队前**没有任何 `dlen <= UDP_SLOT` 检查**。

`dlen` 的上界来自 IP 层：`ip_input`（ip.c:333-342）把 IPv4 分片交给 `reasm_input`（`REASM_MAX = 65536`，
reasm.c:12），重组完成后以 `l4len`（最大 65535）调用 `udp_input`；`udp_input` 只检查
`udp_len > len`（udp.c:195），于是 `dlen = udp_len - 8` 最大 65527。一个两个分片即可构造：
任一同网段/在径主机向本机任一**已绑定**的 UDP 端口发一个重组后约 4 KB 的 UDP 数据报，
`memcpy` 把 1500 字节槽写溢出约 2.5 KB，内容即数据报载荷（攻击者全可控）；`dlen` 更大时
（最长 65527 字节）可一路写穿整个 `socks[8]`（约 50 KB）并越过数组进入相邻 .bss。

可达性：
- DHCP 客户端套接字固定绑定端口 68 且在租约期内长期存在（dhcp.c 持 `sock` 用于续租）——端口可预测；
- DNS 客户端在解析期间绑定随机临时端口（dns.c:518-523，49152+），在径攻击者可直接照抄源端口应答
  （源端口/txid 校验只防离径伪造，dns.c:733）。

后果：远程触发的内核静态内存越界写、写入内容部分可控，可破坏相邻 socket 状态乃至 .bss 内其他对象。
2026-08-04 审计修复的 `e1000` RX 长度校验只约束单帧，不约束重组结果，故不能拦截此路径。

修复建议：入队前 `if (dlen > UDP_SLOT) { s->drops++; return; }`；或把 `udp_input` 对 `dlen`
的截断/拒绝显式化（对齐 `tls_recv` 里 `blen-16 > sizeof s->app` 的既有模式），并同步修 N02
在 `udp_recv` 的 u16 窄化（见"已知问题"）。

---

### [high] [CONFIRMED] TCP 零窗口探测字节的 ACK 永远被判非法 —— 窗口重开信号丢失，连接可永久停滞

位置：`c/net/transport/tcp.c:1689-1696`（探测不推进 `snd_nxt`）+ `tcp.c:1503-1506`（ACK 合法上界用 `snd_nxt`）

```c
                if (now - c->persist_tick >= p) {
                    send_seg(c, ACK | PSH, c->snd_nxt, 1);        /* 发出 snd_nxt 处的 1 字节 */
                    if (seq_lt(c->snd_max, c->snd_nxt + 1)) c->snd_max = c->snd_nxt + 1;
                    c->persist_tick = now;                        /* snd_nxt 不动 */
```

```c
    if (seq_lt(c->snd_nxt, seg_ack)) {         /* acknowledges bytes never sent */
        send_ack(c);
        return;                                 /* 探测的 ACK 走到这里被整个丢弃 */
    }
```

对端窗口为 0 时 persist 探测把 `snd_nxt` 处一字节发上线，但**不推进 `snd_nxt`**。对端（真实栈
接受 RCV.NXT 处的零窗口探测）接受该字节并回 `ACK = snd_nxt+1`，同时携带重开的窗口。我们的
ACK 路径第一道门（tcp.c:1503）以 `snd_nxt` 为"已发送"上界，`snd_nxt < seg_ack` 判为非法
直接 `send_ack; return`——该 ACK 里携带的窗口更新（`update_send_window` 在门之后，
tcp.c:1509）与 SACK 一并被丢弃。

此后每个探测字节的 ACK 都被同样丢弃：`snd_wnd` 永远停在 0（只有 `update_send_window` 能改它），
persist 无限期重发同一字节（该路径不受 `rtx_retries > 8` 的拆连上限约束——那个分支要求
`c->rtx_running`，而全量 ACK 后 rtx 已停）。对端应用排空接收缓冲后**没有任何其它机制**能让我们
看到窗口重开：纯接收方不会自发发包。若对端在等我们的请求体（上传、POST、流水线），双方互相等待，
连接永久停滞且无任何错误上报。

修复建议：探测后推进游标（`c->snd_nxt += 1;`，与 `output_one` 发 FIN 的处理一致，tcp.c:887），
或将 1503 的上界改为 `c->snd_max`（`snd_max` 已含探测字节，tcp.c:1693）。

---

### [high] [CONFIRMED] 发送侧 MSS 不为接收侧 SACK 块预留选项空间 —— 满尺寸数据段叠加 SACK 超过 MTU 被 `ip_send` 静默丢弃

位置：`c/net/transport/tcp.c:821-828`（`eff_mss` 只扣时间戳选项）、`tcp.c:556-575`（数据段可附 SACK 块）、
`tcp.c:746`（`af_send` 返回值被忽略）、`c/net/ip/ip.c:235-237`（1500 字节发送缓冲）

```c
static uint32_t eff_mss(struct tcp_conn *c)
{
    uint32_t m = c->peer_mss;                     /* ≤ 1460 */
    ...
    uint32_t opt = data_optlen(c);                /* 只算时间戳：ts_ok ? 12 : 0 */
    return m > opt + 1 ? m - opt : 1;
}
```

`build_options` 会在**带数据的 ACK 段**上追加 SACK 块（`n_ooo > 0` 时最多 4 块）：
无时间戳时选项区最大 4 + 32 = 36 字节，有时间戳时 12 + 4 + 24 = 40 字节。
而 `output_one` 按 `eff_mss`（最多 1460/1448）装 payload，段总长可达
20 + 36 + 1460 = 1516 字节（或 20 + 40 + 1448 = 1508），既超过真实 MTU 1500，也超过
`ip_send` 的 `uint8_t pkt[1500]`：

```c
    uint8_t pkt[1500];
    if (sizeof(struct ip_hdr) + len > sizeof pkt)
        return -1;                                 /* 静默丢弃，send_seg 不看返回值 */
```

触发条件：一条连接**同时**有发满 MSS 的数据（发送方向）与接收方向乱序（`n_ooo > 0`，
即双向传输 + 丢包/乱序）——典型于 SFTP/SCP、双向 SSH、代理转发这类此机现已承载的负载
（c5c2c9783/e58517ced 之后本机是服务器）。此后该段由 RTO 重发，但 `retransmit_head`
同样按 MSS 装载、同样超限丢弃，直到 `tcp_poll` 的黑洞兜底在**第 3 次 RTO** 把 `pmtu_mss`
压到 508（tcp.c:1724-1725）才恢复——每次命中损失约 3 个 RTO 的吞吐并伴随 cwnd 崩塌
（`cc_on_rto`）。`pmtu_mss` 一旦降级对连接终身生效，之后无谓地用小包。

修复建议：`output_one` 计算本段 `len` 时按"本段实际要携带的选项字节数"（时间戳 + 本次
`build_options` 将产出的 SACK 长度，或保守地 `MAXOPT`）收缩 payload；或 `send_seg`
超限时去掉 SACK 块重装一次。

---

### [medium] [CONFIRMED] `conn_closed` / ICMP 硬错误不停重传定时器 —— 已死连接按 RTO 继续重传最长约 3 分钟

位置：`c/net/transport/tcp.c:1106-1128`（`conn_closed` 未 `rtx_stop`）、`tcp.c:1705-1757`
（`tcp_poll` 的 RTO 分支对 `CLOSED` 态无守卫）、`tcp.c:2379-2410`（`tcp_error` 同样不清定时器）

```c
static void conn_closed(struct tcp_conn *c)
{
    c->state = CLOSED;
    if (!c->in_backlog && !c->app_owned)
        c->used = 0;
    waitq_wake_all(&rx_wq);
}                                                 /* rtx_running 原样保留 */
```

RST（`conn_closed`）或 ICMP destination-unreachable 硬错误（`tcp_error`）把状态置 CLOSED 时，
若当时有数据未获确认（`rtx_running == 1` 且 `snd_una < snd_max`），重传定时器继续走：
`tcp_poll` 的 RTO 分支对 CLOSED 连接落入 `else` 分支（tcp.c:1739），`retransmit_head`
向一个对端已不存在的连接反复重发，每次都招回对端 RST，直到 `rtx_retries > 8`
（回退后约 1+2+4+8+16+32+60+60 ≈ 3 分钟）才由 `conn_closed` 清槽。对 `app_owned`
的连接（描述符仍被应用持有）槽位保留是设计，但**重传**不是：CLOSED 后数据不可能被确认。
每条这类死连接在最长 3 分钟内持续产生无意义的线上流量与 CPU。

修复建议：`conn_closed()` 与 `tcp_error()` 的硬错误分支里 `rtx_stop(c); c->persist_running = 0;`；
并在 `tcp_poll` 的 RTO 分支顶部对 `state == CLOSED` 直接 `continue`。

---

### [low] [CONFIRMED] IPv4 重组恰好 65536 字节时 `l4len` 回绕为 0

位置：`c/net/ip/reasm.c:98-104`

```c
    if (s->have_last && s->rcvd == s->total) {
        ...
        out->l4len = (uint16_t)s->total;    /* total == 65536 时回绕为 0 */
```

`REASM_MAX = 65536` 允许 `off + dlen == 65536`（reasm.c:76 的拒绝条件是 `> REASM_MAX`），
而 `l4len` 是 uint16——恰好 64 KiB 的重组结果长度变成 0，UDP/TCP 收到空载荷静默丢弃。
需要对端发 IP 总长超 65535 的非法分片（如 jumbo 帧），常规线上不可达，仅记录边界。

修复建议：`if (s->total > 65535) { s->used = 0; return 0; }` 或把 `REASM_MAX` 收到 65535。

---

### [low] [CONFIRMED] backlog 在握手完成瞬间被打满时， refusal RST 携带非法序列号 —— 现代对端会丢弃

位置：`c/net/transport/tcp.c:1455-1462`（同型问题也在 `tcp_listen_close`，tcp.c:2229-2243）

```c
        if (backlog_push(c) != 0) {
            send_reset(dst, src, lport, rport, c->snd_nxt, 0, 0);
```

拒绝已三步握手的连接时，RST 的 `seq = c->snd_nxt`（我们的 ISN+1）——与客户端的接收窗口
（以客户端 ISN 为基准）完全无关，落入其窗口的概率约 `wnd/2^32`。实现 RFC 5961 的对端
将非精确 RST 静默丢弃并 challenge-ACK：客户端于是持有一条 ESTABLISHED 但永远无人应答的
连接，直到自身的写超时。触发窗口窄（SYN 与最后 ACK 之间 backlog 恰好打满），故评 low。

修复建议：拒绝已建连的回话时改发 `RST|ACK, seq=0, ack=c->rcv_nxt` 形式（对端以 ACK 匹配接受），
或至少发送带合法 `seq = c->snd_una` 的数据前 RST。

---

### [low] [CONFIRMED] `h2_request` / `pump_send_bodies` 忽略 `tx_frame` 返回值 —— OOM 时帧静默丢失

位置：`c/net/http/http2.c:493`（请求 HEADERS/CONTINUATION 循环）、`http2.c:543`（空体 END_STREAM DATA）

```c
        tx_frame(c, first ? H2_F_HEADERS : H2_F_CONTINUATION, flags, id, block + off, chunk);
```

`tx_frame` 在 `tx_push` 分配失败时返回 `H2_E_NOMEM`，此处不检查：首帧写失败时连接的 HPACK
出向表与对端入向表从此错位（后续索引化字段对端解出错误头），且流被标记 HALF_CLOSED_LOCAL
而服务器从未见到请求。仅 OOM 可达，故 low；但失败形态（静默错位）值得一行 `if (...) return`。

修复建议：循环内检查返回值，失败走 `conn_fail(c, H2_ERR_INTERNAL_ERROR, H2_E_NOMEM)`。

---

### [low] [SUSPECTED] `tcp_connect` 超时清槽与状态翻转之间存在无锁读窗口

位置：`c/net/transport/tcp.c:1859-1865`（`tcp_connect_status` 不持 net_lock 读 `used/state`）、
`tcp.c:1867-1883`（`tcp_connect` 在最后一次无锁读之后才持锁 `used = 0`）

```c
int tcp_connect_status(int id)
{
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];          /* 无 net_lock */
    if (!c->used || c->state == CLOSED) return -1;
    return c->state == SYN_SENT ? 0 : 1;
}
```

多核下 `tcp_connect` 的收尾序列"无锁读 status==0 → 持锁 `used=0`"之间，RX softirq 可在
另一核把连接推进 ESTABLISHED；此时清槽会丢弃一条对端已建连的连接（对端随后被 RST）。
后果是偶发的连接浪费而非内存破坏，且需要 SMP 与恰好同时到达的 SYN-ACK；标 SUSPECTED。

修复建议：最后一次 `tcp_connect_status` 改为在 `net_lock` 下内联判定，或清槽前复核状态。

---

## 已知问题（未重复上报）

以下条目在既往文档已有记录，本轮仅核实现状：

- **N01–N09（docs/NET_BUG_REVIEW_2026-09-09.md）**：
  - N02（UDP 长度 u16 窄化）：`udp.c:104` 接收端原样未修；本轮补充一个同源新入口
    `lsock.c:689`（`lsock_sendto` 把 `int len` 窄化为 `(uint16_t)len` 后调 `udp_send_to`，
    成功时却返回原始 `len`）。
  - N03（UNIX record socket 等待谓词）：`unix.c:567` 的 poll 谓词已含记录槽条件
    （`out->count < UNIX_BUF && (!c->records || out->rcount < UNIX_RECS)`），阻塞路径改为
    等 `unix_change` 变更计数后重测完整谓词（unix.c:12）——疑似已修，本轮未做忙循环复现验证。
  - N04（FIN_WAIT 2 秒回收半关闭）：tcp.c:1765 仍在；N05（libc 侧 0xFFFFFFFF 符号扩展）、
    N06（CNAME TTL 覆盖，dns.c:437-444 仅改 target 不并 TTL）、N07（DHCP 无 expiry，
    dhcp.c `apply_lease` 只存 `renew_at`）、N08（ws.c 空 Ping 块边界，ws.c:136-160 状态机
    仍依赖后续字节）、N09（http2.c:492 `first && last && blen == 0` 才置 END_STREAM）均原样。
- **2026-08-04 审计已修项**：C2/M2/M3/S5–S8、TCP/IP 入站校验和、RST 窗口校验、DNS txid
  随机化、x25519 全零检查、x509 BasicConstraints（x509.c:264 已解析 `is_ca`）等，均在现源码确认。
- **文档化设计决策（不作为缺陷）**：`tls_close` 不发 close_notify（tls.h 契约注释保留）；
  不支持 HelloRetryRequest 之外的会话恢复形态之外的能力（客户端 HRR 已支持，tls.c:980-1016）；
  X.509 不强制 pathLenConstraint/EKU/KeyUsage（x509.c:572 自述）；SSH 拒绝 kex-strict 与
  `psk_ke`、raw socket 拒绝 IP_HDRINCL（lsock.c:681-686 注释自述）；TCP 不记录对端通告窗口之外
  的拥塞细节为性能取舍；`net_lock` 为递归锁、长持有见 netlock.c 头注。
- **Stale 文档**：CODE_AUDIT.md "遗留风险清单"中 "UDP 入站校验和未验证" 已过时——
  udp.c:201-204 现在对非零校验和强制验证。
- 本轮审计过但未发现新问题的文件：`hpack.c`、`cookies.c`、`http1.c`、`hpool.c`、`url.c`、
  `route.c`、`netlock.c`、`net.c`、`icmp.c`、`arp.c`（读到 :756）、`ssh/*`（9 个文件全部）、
  `tls_server.c`、`tls12.c` 的记录层与 SKE 路径（`speer[97]` 恰好把 `plen ≤ 97` 钳住，
  `signed_data[165]` 满界不溢）。
